/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#define KMSG_COMPONENT "zram"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/lockdep.h>
#include <linux/swap.h>
#include <linux/backing-dev.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/cpuhotplug.h>
#include <linux/wait_bit.h>

#include "zram_drv.h"
#include "zram_drv_internal.h"
#ifdef CONFIG_HYBRIDSWAP
#include "../hybridswap/hybridswap.h"
#endif

static DEFINE_IDR(zram_index_idr);
/* idr index must be protected */
static DEFINE_MUTEX(zram_index_mutex);

static int zram_major;
static const char *default_compressor = CONFIG_HYBRIDSWAP_ZRAM_DEF_COMP;

/* Module params (documentation at end) */
static unsigned int num_devices = 1;
/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size;

static const struct block_device_operations zram_devops;

static bool zram_free_page(struct zram *zram, size_t index);

/*
 * Sleep until no eswap operation claims @index, returning with the slot
 * locked.  A fault-out fetch (ZRAM_BATCHING_OUT) or a shrink writeback
 * (ZRAM_UNDER_WB) installs its result under this lock, so freeing or
 * overwriting the slot mid-flight would feed the reader whichever page
 * got stored last and corrupt the operation's accounting.  The wait can
 * span a full eswap IO, so sleep on the slot's flags instead of spinning;
 * wakers live at every site that clears either flag.
 *
 * Returns false only when the wait gave up with the inflight bits still
 * set, i.e. the completer is stuck or gone.  That is a real signal, so
 * it is reported as a WARN_ON_ONCE, and the caller must NOT free or
 * overwrite the slot: it would race the very writer this wait exists to
 * exclude.  On false the slot lock is still held, exactly as on the
 * quiesced return, so callers unlock either way.
 */
static bool zram_wait_slot_inflight(struct zram *zram, u32 index)
{
	unsigned long *flagsp = &zram->table[index].flags;
	unsigned long inflight = BIT(ZRAM_UNDER_WB);
	ktime_t start = ktime_get();
	int retries = 0;

	/*
	 * ZRAM_BATCHING_OUT only exists in a hybridswap build, and only
	 * hybridswap_page_fault_exit_check() - on the exit path of
	 * hybridswap_page_fault() - ever clears it, so without
	 * CONFIG_HYBRIDSWAP_CORE there is no fault-out fetch to wait for.
	 */
#ifdef CONFIG_HYBRIDSWAP_CORE
	inflight |= BIT(ZRAM_BATCHING_OUT);
#endif

	zram_slot_lock(zram, index);
	while (*flagsp & inflight) {
		zram_slot_unlock(zram, index);
		wait_var_event_timeout(flagsp, !(*flagsp & inflight),
				       msecs_to_jiffies(5000));
		zram_slot_lock(zram, index);
		if (!(*flagsp & inflight))
			break;
		if (++retries >= 2) {
			/*
			 * Completer is stuck or gone; degrade loudly.
			 *
			 * The log line is the durable half of this. The
			 * counter lives in zram->stats, which
			 * zram_reset_device_locked() zeroes, so a device
			 * that is reset afterwards keeps no tally while
			 * the line survives in the ring buffer. Printed
			 * with the slot lock held and without taking a
			 * lock of its own, because the only locks in
			 * reach from here are ones this path already
			 * holds.
			 */
			WARN_ON_ONCE(1);
			atomic64_inc(&zram->stats.slot_stuck);
			pr_info("zram %s: slot %u still claimed after %lld us, flags 0x%lx (inflight mask 0x%lx)\n",
				zram->disk->disk_name, index,
				ktime_us_delta(ktime_get(), start),
				*flagsp, inflight);
			return false;
		}
	}

	return true;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
				u32 index, int offset, struct bio *bio);


static int zram_slot_trylock(struct zram *zram, u32 index)
{
	return bit_spin_trylock(ZRAM_LOCK, &zram->table[index].flags);
}

static unsigned long zram_get_element(struct zram *zram, u32 index)
{
	return zram->table[index].element;
}

static inline bool zram_allocated(struct zram *zram, u32 index)
{
	return zram_get_obj_size(zram, index) ||
			zram_test_flag(zram, index, ZRAM_SAME) ||
			zram_test_flag(zram, index, ZRAM_WB);
}

#if PAGE_SIZE != 4096
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return bvec->bv_len != PAGE_SIZE;
}
#else
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return false;
}
#endif

/*
 * Check if request is within bounds and aligned on zram logical blocks.
 */
static inline bool valid_io_request(struct zram *zram,
		sector_t start, unsigned int size)
{
	u64 end, bound;

	/* unaligned request */
	if (unlikely(start & (ZRAM_SECTOR_PER_LOGICAL_BLOCK - 1)))
		return false;
	if (unlikely(size & (ZRAM_LOGICAL_BLOCK_SIZE - 1)))
		return false;

	end = start + (size >> SECTOR_SHIFT);
	bound = zram->disksize >> SECTOR_SHIFT;
	/* out of range range */
	if (unlikely(start >= bound || end > bound || start > end))
		return false;

	/* I/O request is valid */
	return true;
}

static void update_position(u32 *index, int *offset, struct bio_vec *bvec)
{
	*index  += (*offset + bvec->bv_len) / PAGE_SIZE;
	*offset = (*offset + bvec->bv_len) % PAGE_SIZE;
}

static inline void update_used_max(struct zram *zram,
					const unsigned long pages)
{
	unsigned long old_max, cur_max;

	old_max = atomic_long_read(&zram->stats.max_used_pages);

	do {
		cur_max = old_max;
		if (pages > cur_max)
			old_max = atomic_long_cmpxchg(
				&zram->stats.max_used_pages, cur_max, pages);
	} while (old_max != cur_max);
}

static inline void zram_fill_page(void *ptr, unsigned long len,
					unsigned long value)
{
	WARN_ON_ONCE(!IS_ALIGNED(len, sizeof(unsigned long)));
	memset_l(ptr, value, len / sizeof(unsigned long));
}

static bool page_same_filled(void *ptr, unsigned long *element)
{
	unsigned long *page;
	unsigned long val;
	unsigned int pos, last_pos = PAGE_SIZE / sizeof(*page) - 1;

	page = (unsigned long *)ptr;
	val = page[0];

	if (val != page[last_pos])
		return false;

	for (pos = 1; pos < last_pos; pos++) {
		if (val != page[pos])
			return false;
	}

	*element = val;

	return true;
}

static ssize_t initstate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u32 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = init_done(zram);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t disksize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", zram->disksize);
}

static ssize_t mem_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 limit;
	char *tmp;
	struct zram *zram = dev_to_zram(dev);

	limit = memparse(buf, &tmp);
	if (buf == tmp) /* no chars parsed, invalid input */
		return -EINVAL;

	down_write(&zram->init_lock);
	zram->limit_pages = PAGE_ALIGN(limit) >> PAGE_SHIFT;
	up_write(&zram->init_lock);

	return len;
}

static ssize_t mem_used_max_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int err;
	unsigned long val;
	struct zram *zram = dev_to_zram(dev);

	err = kstrtoul(buf, 10, &val);
	if (err || val != 0)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		atomic_long_set(&zram->stats.max_used_pages,
				zs_get_total_pages(zram->mem_pool));
	}
	up_read(&zram->init_lock);

	return len;
}

static ssize_t idle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	int index;

	if (!sysfs_streq(buf, "all"))
		return -EINVAL;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	for (index = 0; index < nr_pages; index++) {
		/*
		 * Do not mark ZRAM_UNDER_WB slot as ZRAM_IDLE to close race.
		 * See the comment in writeback_store.
		 */
		zram_slot_lock(zram, index);
		if (zram_allocated(zram, index) &&
				!zram_test_flag(zram, index, ZRAM_UNDER_WB))
			zram_set_flag(zram, index, ZRAM_IDLE);
		zram_slot_unlock(zram, index);
	}

	up_read(&zram->init_lock);

	return len;
}

#ifdef CONFIG_HYBRIDSWAP_ZRAM_WRITEBACK
static ssize_t writeback_limit_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->wb_limit_enable = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	bool val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->wb_limit_enable;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t writeback_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->bd_wb_limit = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->bd_wb_limit;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static void reset_bdev(struct zram *zram)
{
	struct block_device *bdev;

	if (!zram->backing_dev)
		return;

	bdev = zram->bdev;
	blkdev_put(bdev, FMODE_READ|FMODE_WRITE|FMODE_EXCL);
	/* hope filp_close flush all of IO */
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->bdev = NULL;
	zram->disk->queue->backing_dev_info->capabilities |=
				BDI_CAP_SYNCHRONOUS_IO;
	kvfree(zram->bitmap);
	zram->bitmap = NULL;
}

static ssize_t backing_dev_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct file *file;
	struct zram *zram = dev_to_zram(dev);
	char *p;
	ssize_t ret;

	down_read(&zram->init_lock);
	file = zram->backing_dev;
	if (!file) {
		memcpy(buf, "none\n", 5);
		up_read(&zram->init_lock);
		return 5;
	}

	p = file_path(file, buf, PAGE_SIZE - 1);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		goto out;
	}

	ret = strlen(p);
	memmove(buf, p, ret);
	buf[ret++] = '\n';
out:
	up_read(&zram->init_lock);
	return ret;
}

static ssize_t backing_dev_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	char *file_name;
	size_t sz;
	struct file *backing_dev = NULL;
	struct inode *inode;
	struct address_space *mapping;
	size_t bitmap_sz;
	unsigned long nr_pages, *bitmap = NULL;
	struct block_device *bdev = NULL;
	int err;
	struct zram *zram = dev_to_zram(dev);

	file_name = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!file_name)
		return -ENOMEM;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Can't setup backing device for initialized device\n");
		err = -EBUSY;
		goto out;
	}

	strlcpy(file_name, buf, PATH_MAX);
	/* ignore trailing newline */
	sz = strlen(file_name);
	if (sz > 0 && file_name[sz - 1] == '\n')
		file_name[sz - 1] = 0x00;

	backing_dev = filp_open(file_name, O_RDWR|O_LARGEFILE, 0);
	if (IS_ERR(backing_dev)) {
		err = PTR_ERR(backing_dev);
		backing_dev = NULL;
		goto out;
	}

	mapping = backing_dev->f_mapping;
	inode = mapping->host;

	/* Support only block device in this moment */
	if (!S_ISBLK(inode->i_mode)) {
		err = -ENOTBLK;
		goto out;
	}

	bdev = blkdev_get_by_dev(inode->i_rdev,
			FMODE_READ | FMODE_WRITE | FMODE_EXCL, zram);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		bdev = NULL;
		goto out;
	}

	nr_pages = i_size_read(inode) >> PAGE_SHIFT;
	bitmap_sz = BITS_TO_LONGS(nr_pages) * sizeof(long);
	bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!bitmap) {
		err = -ENOMEM;
		goto out;
	}

	reset_bdev(zram);

	zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	zram->bitmap = bitmap;
	zram->nr_pages = nr_pages;
	/*
	 * With writeback feature, zram does asynchronous IO so it's no longer
	 * synchronous device so let's remove synchronous io flag. Othewise,
	 * upper layer(e.g., swap) could wait IO completion rather than
	 * (submit and return), which will cause system sluggish.
	 * Furthermore, when the IO function returns(e.g., swap_readpage),
	 * upper layer expects IO was done so it could deallocate the page
	 * freely but in fact, IO is going on so finally could cause
	 * use-after-free when the IO is really done.
	 */
	zram->disk->queue->backing_dev_info->capabilities &=
			~BDI_CAP_SYNCHRONOUS_IO;
	up_write(&zram->init_lock);

	pr_info("setup backing device %s\n", file_name);
	kfree(file_name);

	return len;
out:
	if (bitmap)
		kvfree(bitmap);

	if (bdev)
		blkdev_put(bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);

	if (backing_dev)
		filp_close(backing_dev, NULL);

	up_write(&zram->init_lock);

	kfree(file_name);

	return err;
}

static unsigned long alloc_block_bdev(struct zram *zram)
{
	unsigned long blk_idx = 1;
retry:
	/* skip 0 bit to confuse zram.handle = 0 */
	blk_idx = find_next_zero_bit(zram->bitmap, zram->nr_pages, blk_idx);
	if (blk_idx == zram->nr_pages)
		return 0;

	if (test_and_set_bit(blk_idx, zram->bitmap))
		goto retry;

	atomic64_inc(&zram->stats.bd_count);
	return blk_idx;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	int was_set;

	was_set = test_and_clear_bit(blk_idx, zram->bitmap);
	WARN_ON_ONCE(!was_set);
	atomic64_dec(&zram->stats.bd_count);
}

static void zram_page_end_io(struct bio *bio)
{
	struct page *page = bio_first_page_all(bio);

	page_endio(page, op_is_write(bio_op(bio)),
			blk_status_to_errno(bio->bi_status));
	bio_put(bio);
}

/*
 * Returns 1 if the submission is successful.
 */
static int read_from_bdev_async(struct zram *zram, struct bio_vec *bvec,
			unsigned long entry, struct bio *parent)
{
	struct bio *bio;

	bio = bio_alloc(GFP_NOIO, 1);
	if (!bio)
		return -ENOMEM;

	bio->bi_iter.bi_sector = entry * (PAGE_SIZE >> 9);
	bio_set_dev(bio, zram->bdev);
	if (!bio_add_page(bio, bvec->bv_page, bvec->bv_len, bvec->bv_offset)) {
		bio_put(bio);
		return -EIO;
	}

	if (!parent) {
		bio->bi_opf = REQ_OP_READ;
		bio->bi_end_io = zram_page_end_io;
	} else {
		bio->bi_opf = parent->bi_opf;
		bio_chain(bio, parent);
	}

	submit_bio(bio);
	return 1;
}

#define HUGE_WRITEBACK 1
#define IDLE_WRITEBACK 2

static ssize_t writeback_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long index;
	struct bio bio;
	struct bio_vec bio_vec;
	struct page *page;
	ssize_t ret = len;
	int mode, err;
	unsigned long blk_idx = 0;

	if (sysfs_streq(buf, "idle"))
		mode = IDLE_WRITEBACK;
	else if (sysfs_streq(buf, "huge"))
		mode = HUGE_WRITEBACK;
	else
		return -EINVAL;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto release_init_lock;
	}

	if (!zram->backing_dev) {
		ret = -ENODEV;
		goto release_init_lock;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

	for (index = 0; index < nr_pages; index++) {
		struct bio_vec bvec;

		bvec.bv_page = page;
		bvec.bv_len = PAGE_SIZE;
		bvec.bv_offset = 0;

		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && !zram->bd_wb_limit) {
			spin_unlock(&zram->wb_limit_lock);
			ret = -EIO;
			break;
		}
		spin_unlock(&zram->wb_limit_lock);

		if (!blk_idx) {
			blk_idx = alloc_block_bdev(zram);
			if (!blk_idx) {
				ret = -ENOSPC;
				break;
			}
		}

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
				zram_test_flag(zram, index, ZRAM_SAME) ||
				zram_test_flag(zram, index, ZRAM_UNDER_WB))
			goto next;

		if (mode == IDLE_WRITEBACK &&
			  !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;
		if (mode == HUGE_WRITEBACK &&
			  !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;
		/*
		 * Clearing ZRAM_UNDER_WB is duty of caller.
		 * IOW, zram_free_page never clear it.
		 */
		zram_set_flag(zram, index, ZRAM_UNDER_WB);
		/* Need for hugepage writeback racing */
		zram_set_flag(zram, index, ZRAM_IDLE);
		zram_slot_unlock(zram, index);
		if (zram_bvec_read(zram, &bvec, index, 0, NULL)) {
			zram_slot_lock(zram, index);
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_slot_unlock(zram, index);
			continue;
		}

		bio_init(&bio, &bio_vec, 1);
		bio_set_dev(&bio, zram->bdev);
		bio.bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
		bio.bi_opf = REQ_OP_WRITE | REQ_SYNC;

		bio_add_page(&bio, bvec.bv_page, bvec.bv_len,
				bvec.bv_offset);
		/*
		 * XXX: A single page IO would be inefficient for write
		 * but it would be not bad as starter.
		 */
		err = submit_bio_wait(&bio);
		if (err) {
			zram_slot_lock(zram, index);
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_slot_unlock(zram, index);
			/*
			 * Return last IO error unless every IO were
			 * not suceeded.
			 */
			ret = err;
			continue;
		}

		atomic64_inc(&zram->stats.bd_writes);
		/*
		 * We released zram_slot_lock so need to check if the slot was
		 * changed. If there is freeing for the slot, we can catch it
		 * easily by zram_allocated.
		 * A subtle case is the slot is freed/reallocated/marked as
		 * ZRAM_IDLE again. To close the race, idle_store doesn't
		 * mark ZRAM_IDLE once it found the slot was ZRAM_UNDER_WB.
		 * Thus, we could close the race by checking ZRAM_IDLE bit.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
			  !zram_test_flag(zram, index, ZRAM_IDLE)) {
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			goto next;
		}

		/*
		 * Give up on this index if the free was refused, undoing
		 * only the two flags this loop set on it and skipping the
		 * ZRAM_WB install below - installing over a slot whose
		 * hybridswap bookkeeping was deliberately left in place is
		 * exactly the corruption the refusal exists to prevent.
		 * blk_idx is still set here, so the common tail returns
		 * the backing block to the pool; nothing is leaked.
		 *
		 * CONFIG_HYBRIDSWAP_CORE cannot be set in this build
		 * (HYBRIDSWAP depends on !HYBRIDSWAP_ZRAM_WRITEBACK), so
		 * this cannot actually fire today; the check is here so
		 * that stays true if that dependency ever changes.
		 */
		if (!zram_free_page(zram, index)) {
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			goto next;
		}
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_set_flag(zram, index, ZRAM_WB);
		zram_set_element(zram, index, blk_idx);
		blk_idx = 0;
		atomic64_inc(&zram->stats.pages_stored);
		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
			zram->bd_wb_limit -=  1UL << (PAGE_SHIFT - 12);
		spin_unlock(&zram->wb_limit_lock);
next:
		zram_slot_unlock(zram, index);
	}

	if (blk_idx)
		free_block_bdev(zram, blk_idx);
	__free_page(page);
release_init_lock:
	up_read(&zram->init_lock);

	return ret;
}

struct zram_work {
	struct work_struct work;
	struct zram *zram;
	unsigned long entry;
	struct bio *bio;
	struct bio_vec bvec;
};

#if PAGE_SIZE != 4096
static void zram_sync_read(struct work_struct *work)
{
	struct zram_work *zw = container_of(work, struct zram_work, work);
	struct zram *zram = zw->zram;
	unsigned long entry = zw->entry;
	struct bio *bio = zw->bio;

	read_from_bdev_async(zram, &zw->bvec, entry, bio);
}

/*
 * Block layer want one ->make_request_fn to be active at a time
 * so if we use chained IO with parent IO in same context,
 * it's a deadlock. To avoid, it, it uses worker thread context.
 */
static int read_from_bdev_sync(struct zram *zram, struct bio_vec *bvec,
				unsigned long entry, struct bio *bio)
{
	struct zram_work work;

	work.bvec = *bvec;
	work.zram = zram;
	work.entry = entry;
	work.bio = bio;

	INIT_WORK_ONSTACK(&work.work, zram_sync_read);
	queue_work(system_unbound_wq, &work.work);
	flush_work(&work.work);
	destroy_work_on_stack(&work.work);

	return 1;
}
#else
static int read_from_bdev_sync(struct zram *zram, struct bio_vec *bvec,
				unsigned long entry, struct bio *bio)
{
	WARN_ON(1);
	return -EIO;
}
#endif

static int read_from_bdev(struct zram *zram, struct bio_vec *bvec,
			unsigned long entry, struct bio *parent, bool sync)
{
	atomic64_inc(&zram->stats.bd_reads);
	if (sync)
		return read_from_bdev_sync(zram, bvec, entry, parent);
	else
		return read_from_bdev_async(zram, bvec, entry, parent);
}
#else
#ifdef CONFIG_HYBRIDSWAP_CORE
static void reset_bdev(struct zram *zram)
{
	struct block_device *bdev;

	if (!zram->backing_dev)
		return;

	bdev = zram->bdev;
	if (zram->old_block_size)
		set_blocksize(bdev, zram->old_block_size);
	blkdev_put(bdev, FMODE_READ|FMODE_WRITE|FMODE_EXCL);
	/* hope filp_close flush all of IO */
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->old_block_size = 0;
	zram->bdev = NULL;
}

void hybridswap_unbind_bdev(struct zram *zram)
{
	down_write(&zram->init_lock);
	reset_bdev(zram);
	up_write(&zram->init_lock);
}
#else
static inline void reset_bdev(struct zram *zram) {};
#endif
static int read_from_bdev(struct zram *zram, struct bio_vec *bvec,
			unsigned long entry, struct bio *parent, bool sync)
{
	return -EIO;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
#endif

#ifdef CONFIG_HYBRIDSWAP_ZRAM_MEMORY_TRACKING

static struct dentry *zram_debugfs_root;

static void zram_debugfs_create(void)
{
	zram_debugfs_root = debugfs_create_dir("zram", NULL);
}

static void zram_debugfs_destroy(void)
{
	debugfs_remove_recursive(zram_debugfs_root);
}

static void zram_accessed(struct zram *zram, u32 index)
{
	zram_clear_flag(zram, index, ZRAM_IDLE);
	zram->table[index].ac_time = ktime_get_boottime();
}

static ssize_t read_block_state(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	char *kbuf;
	ssize_t index, written = 0;
	struct zram *zram = file->private_data;
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	struct timespec64 ts;

	kbuf = kvmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		kvfree(kbuf);
		return -EINVAL;
	}

	for (index = *ppos; index < nr_pages; index++) {
		int copied;

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		ts = ktime_to_timespec64(zram->table[index].ac_time);
		copied = snprintf(kbuf + written, count,
			"%12zd %12lld.%06lu %c%c%c%c\n",
			index, (s64)ts.tv_sec,
			ts.tv_nsec / NSEC_PER_USEC,
			zram_test_flag(zram, index, ZRAM_SAME) ? 's' : '.',
			zram_test_flag(zram, index, ZRAM_WB) ? 'w' : '.',
			zram_test_flag(zram, index, ZRAM_HUGE) ? 'h' : '.',
			zram_test_flag(zram, index, ZRAM_IDLE) ? 'i' : '.');

		if (count <= copied) {
			zram_slot_unlock(zram, index);
			break;
		}
		written += copied;
		count -= copied;
next:
		zram_slot_unlock(zram, index);
		*ppos += 1;
	}

	up_read(&zram->init_lock);
	if (copy_to_user(buf, kbuf, written))
		written = -EFAULT;
	kvfree(kbuf);

	return written;
}

static const struct file_operations proc_zram_block_state_op = {
	.open = simple_open,
	.read = read_block_state,
	.llseek = default_llseek,
};

static void zram_debugfs_register(struct zram *zram)
{
	if (!zram_debugfs_root)
		return;

	zram->debugfs_dir = debugfs_create_dir(zram->disk->disk_name,
						zram_debugfs_root);
	debugfs_create_file("block_state", 0400, zram->debugfs_dir,
				zram, &proc_zram_block_state_op);
}

static void zram_debugfs_unregister(struct zram *zram)
{
	debugfs_remove_recursive(zram->debugfs_dir);
}
#else
static void zram_debugfs_create(void) {};
static void zram_debugfs_destroy(void) {};
static void zram_accessed(struct zram *zram, u32 index)
{
	zram_clear_flag(zram, index, ZRAM_IDLE);
};
static void zram_debugfs_register(struct zram *zram) {};
static void zram_debugfs_unregister(struct zram *zram) {};
#endif

/*
 * We switched to per-cpu streams and this attr is not needed anymore.
 * However, we will keep it around for some time, because:
 * a) we may revert per-cpu streams in the future
 * b) it's visible to user space and we need to follow our 2 years
 *    retirement rule; but we already have a number of 'soon to be
 *    altered' attrs, so max_comp_streams need to wait for the next
 *    layoff cycle.
 */
static ssize_t max_comp_streams_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", num_online_cpus());
}

static ssize_t max_comp_streams_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	return len;
}

static ssize_t comp_algorithm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t sz;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	sz = zcomp_available_show(zram->compressor, buf);
	up_read(&zram->init_lock);

	return sz;
}

static ssize_t comp_algorithm_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	char compressor[ARRAY_SIZE(zram->compressor)];
	size_t sz;

	strlcpy(compressor, buf, sizeof(compressor));
	/* ignore trailing newline */
	sz = strlen(compressor);
	if (sz > 0 && compressor[sz - 1] == '\n')
		compressor[sz - 1] = 0x00;

	if (!zcomp_available_algorithm(compressor))
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		up_write(&zram->init_lock);
		pr_info("Can't change algorithm for initialized device\n");
		return -EBUSY;
	}

	strcpy(zram->compressor, compressor);
	up_write(&zram->init_lock);
	return len;
}

static ssize_t compact_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	zs_compact(zram->mem_pool);
	up_read(&zram->init_lock);

	return len;
}

static ssize_t io_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8llu\n",
			(u64)atomic64_read(&zram->stats.failed_reads),
			(u64)atomic64_read(&zram->stats.failed_writes),
			(u64)atomic64_read(&zram->stats.invalid_io),
			(u64)atomic64_read(&zram->stats.notify_free));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t mm_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	struct zs_pool_stats pool_stats;
	u64 orig_size, mem_used = 0;
	long max_used;
	ssize_t ret;

	memset(&pool_stats, 0x00, sizeof(struct zs_pool_stats));

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		mem_used = zs_get_total_pages(zram->mem_pool);
		zs_pool_stats(zram->mem_pool, &pool_stats);
	}

	orig_size = atomic64_read(&zram->stats.pages_stored);
	max_used = atomic_long_read(&zram->stats.max_used_pages);

	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8lu %8ld %8llu %8lu %8llu\n",
			orig_size << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.compr_data_size),
			mem_used << PAGE_SHIFT,
			zram->limit_pages << PAGE_SHIFT,
			max_used << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.same_pages),
			atomic_long_read(&pool_stats.pages_compacted),
			(u64)atomic64_read(&zram->stats.huge_pages));
	up_read(&zram->init_lock);

	return ret;
}

#ifdef CONFIG_HYBRIDSWAP_ZRAM_WRITEBACK
#define FOUR_K(x) ((x) * (1 << (PAGE_SHIFT - 12)))
static ssize_t bd_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
		"%8llu %8llu %8llu\n",
			FOUR_K((u64)atomic64_read(&zram->stats.bd_count)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_reads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_writes)));
	up_read(&zram->init_lock);

	return ret;
}
#endif

static ssize_t debug_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int version = 1;
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"version: %d\n%8llu %8llu\n",
			version,
			(u64)atomic64_read(&zram->stats.writestall),
			(u64)atomic64_read(&zram->stats.miss_free));
	up_read(&zram->init_lock);

	return ret;
}

static DEVICE_ATTR_RO(io_stat);
static DEVICE_ATTR_RO(mm_stat);
#ifdef CONFIG_HYBRIDSWAP_ZRAM_WRITEBACK
static DEVICE_ATTR_RO(bd_stat);
#endif
static DEVICE_ATTR_RO(debug_stat);

static void zram_meta_free(struct zram *zram, u64 disksize)
{
	size_t num_pages = disksize >> PAGE_SHIFT;
	size_t index;

	/* Free all pages that are still in this zram device */
	for (index = 0; index < num_pages; index++) {
		if (!zram_wait_slot_inflight(zram, index)) {
			/*
			 * A fault-out fetch or a shrink writeback still owns
			 * this slot.  There is no refcount here to wait on,
			 * so the sweep cannot simply be resumed later: the
			 * zs_destroy_pool()/vfree() below would pull the pool
			 * and the table out from under that completer, and
			 * stopping the sweep would do the same to every slot
			 * it has not reached yet.  Abandon it instead.
			 *
			 * The table and the pool are then orphaned, not
			 * merely left allocated: the caller has already
			 * zeroed zram->disksize, so init_done() is false and
			 * the next disksize write allocates fresh ones over
			 * the top, dropping the old pool and every page left
			 * in it.  Keeping that memory would take a deferred
			 * teardown, which is out of scope here; leaking it
			 * beats the use-after-free, and the WARN_ON_ONCE above
			 * is what reports the stuck completer that got us
			 * here.
			 */
			zram_slot_unlock(zram, index);
			pr_err("meta free aborted at index %zu: busy slot\n",
					index);
			return;
		}
		/*
		 * The wait above already drained both inflight bits that
		 * hybridswap_untrack() waits on, so it cannot give up again
		 * here.  A false return would mean the tables went out from
		 * under this sweep, the same unrecoverable state as above,
		 * so stop rather than walk on.
		 */
		if (!zram_free_page(zram, index)) {
			zram_slot_unlock(zram, index);
			pr_err("meta free aborted at %zu: untracked slot\n",
					index);
			return;
		}
		zram_slot_unlock(zram, index);
	}

	zs_destroy_pool(zram->mem_pool);
	vfree(zram->table);
}

static bool zram_meta_alloc(struct zram *zram, u64 disksize)
{
	size_t num_pages;

	num_pages = disksize >> PAGE_SHIFT;
	zram->table = vzalloc(array_size(num_pages, sizeof(*zram->table)));
	if (!zram->table)
		return false;

	zram->mem_pool = zs_create_pool(zram->disk->disk_name);
	if (!zram->mem_pool) {
		vfree(zram->table);
		return false;
	}

	if (!huge_class_size)
		huge_class_size = zs_huge_class_size(zram->mem_pool);
	return true;
}

/*
 * To protect concurrent access to the same index entry,
 * caller should hold this table index entry's bit_spinlock to
 * indicate this index entry is accessing.
 *
 * Returns false if the hybridswap bookkeeping for this slot had to be
 * left in place because a completer still owns it; the slot itself is
 * not freed in that case, so nothing observes the inconsistency, but
 * the caller must not go on to reuse or re-derive the slot's state.
 */
static bool zram_free_page(struct zram *zram, size_t index)
{
	unsigned long handle;

#ifdef CONFIG_HYBRIDSWAP_CORE
	/*
	 * First, before any mutation: on a false return the slot must be
	 * left exactly as found, so the hybridswap bookkeeping is the
	 * one thing that gets to veto before the ac_time / ZRAM_IDLE /
	 * ZRAM_HUGE updates below have already half-committed to a free
	 * that is not going to happen.
	 */
	if (!hybridswap_untrack(zram, index))
		return false;
#endif

#ifdef CONFIG_HYBRIDSWAP_ZRAM_MEMORY_TRACKING
	zram->table[index].ac_time = 0;
#endif
	if (zram_test_flag(zram, index, ZRAM_IDLE))
		zram_clear_flag(zram, index, ZRAM_IDLE);

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		zram_clear_flag(zram, index, ZRAM_HUGE);
		atomic64_dec(&zram->stats.huge_pages);
	}

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		zram_clear_flag(zram, index, ZRAM_WB);
		free_block_bdev(zram, zram_get_element(zram, index));
		atomic64_dec(&zram->stats.pages_stored);
		goto out;
	}

	/*
	 * No memory is allocated for same element filled pages.
	 * Simply clear same page flag.
	 */
	if (zram_test_flag(zram, index, ZRAM_SAME)) {
		zram_clear_flag(zram, index, ZRAM_SAME);
		atomic64_dec(&zram->stats.same_pages);
		atomic64_dec(&zram->stats.pages_stored);
		goto out;
	}

	handle = zram_get_handle(zram, index);
	if (!handle)
		return true;

	zs_free(zram->mem_pool, handle);

	atomic64_sub(zram_get_obj_size(zram, index),
			&zram->stats.compr_data_size);
	atomic64_dec(&zram->stats.pages_stored);

out:
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	WARN_ON_ONCE(zram->table[index].flags &
		~(1UL << ZRAM_LOCK | 1UL << ZRAM_UNDER_WB));
	return true;
}


static int __zram_bvec_read(struct zram *zram, struct page *page, u32 index,
				struct bio *bio, bool partial_io)
{
	struct zcomp_strm *zstrm;
	unsigned long handle;
	unsigned int size;
	void *src, *dst;
	int ret;

	zram_slot_lock(zram, index);

#ifdef CONFIG_HYBRIDSWAP_CORE
	if (likely(!bio)) {
		ret = hybridswap_page_fault(zram, index);
		if (unlikely(ret)) {
			pr_err("search in hybridswap failed! err=%d, page=%u\n",
					ret, index);
			zram_slot_unlock(zram, index);
			return ret;
		}
	}
#endif

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		struct bio_vec bvec;

		zram_slot_unlock(zram, index);

		bvec.bv_page = page;
		bvec.bv_len = PAGE_SIZE;
		bvec.bv_offset = 0;
		return read_from_bdev(zram, &bvec,
				zram_get_element(zram, index),
				bio, partial_io);
	}

	handle = zram_get_handle(zram, index);
	if (!handle || zram_test_flag(zram, index, ZRAM_SAME)) {
		unsigned long value;
		void *mem;

		value = handle ? zram_get_element(zram, index) : 0;
		mem = kmap_atomic(page);
		zram_fill_page(mem, PAGE_SIZE, value);
		kunmap_atomic(mem);
		zram_slot_unlock(zram, index);
		return 0;
	}

	size = zram_get_obj_size(zram, index);

	if (size != PAGE_SIZE)
		zstrm = zcomp_stream_get(zram->comp);

	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	if (size == PAGE_SIZE) {
		dst = kmap_atomic(page);
		memcpy(dst, src, PAGE_SIZE);
		kunmap_atomic(dst);
		ret = 0;
	} else {
		dst = kmap_atomic(page);
		ret = zcomp_decompress(zstrm, src, size, dst);
		kunmap_atomic(dst);
		zcomp_stream_put(zram->comp);
	}
	zs_unmap_object(zram->mem_pool, handle);
	zram_slot_unlock(zram, index);

	/* Should NEVER happen. Return bio error if it does. */
	if (WARN_ON(ret))
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);

	return ret;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
				u32 index, int offset, struct bio *bio)
{
	int ret;
	struct page *page;

	page = bvec->bv_page;
	if (is_partial_io(bvec)) {
		/* Use a temporary buffer to decompress the page */
		page = alloc_page(GFP_NOIO|__GFP_HIGHMEM);
		if (!page)
			return -ENOMEM;
	}

	ret = __zram_bvec_read(zram, page, index, bio, is_partial_io(bvec));
	if (unlikely(ret))
		goto out;

	if (is_partial_io(bvec)) {
		void *dst = kmap_atomic(bvec->bv_page);
		void *src = kmap_atomic(page);

		memcpy(dst + bvec->bv_offset, src + offset, bvec->bv_len);
		kunmap_atomic(src);
		kunmap_atomic(dst);
	}
out:
	if (is_partial_io(bvec))
		__free_page(page);

	return ret;
}

static int __zram_bvec_write(struct zram *zram, struct bio_vec *bvec,
				u32 index, struct bio *bio)
{
	int ret = 0;
	unsigned long alloced_pages;
	unsigned long handle = 0;
	unsigned int comp_len = 0;
	void *src, *dst, *mem;
	struct zcomp_strm *zstrm;
	struct page *page = bvec->bv_page;
	unsigned long element = 0;
	enum zram_pageflags flags = 0;

	mem = kmap_atomic(page);
	if (page_same_filled(mem, &element)) {
		kunmap_atomic(mem);
		/* Free memory associated with this sector now. */
		flags = ZRAM_SAME;
		atomic64_inc(&zram->stats.same_pages);
		goto out;
	}
	kunmap_atomic(mem);

compress_again:
	zstrm = zcomp_stream_get(zram->comp);
	src = kmap_atomic(page);
	ret = zcomp_compress(zstrm, src, &comp_len);
	kunmap_atomic(src);

	if (unlikely(ret)) {
		zcomp_stream_put(zram->comp);
		pr_err("Compression failed! err=%d\n", ret);
		zs_free(zram->mem_pool, handle);
		return ret;
	}

	if (comp_len >= huge_class_size)
		comp_len = PAGE_SIZE;
	/*
	 * handle allocation has 2 paths:
	 * a) fast path is executed with preemption disabled (for
	 *  per-cpu streams) and has __GFP_DIRECT_RECLAIM bit clear,
	 *  since we can't sleep;
	 * b) slow path enables preemption and attempts to allocate
	 *  the page with __GFP_DIRECT_RECLAIM bit set. we have to
	 *  put per-cpu compression stream and, thus, to re-do
	 *  the compression once handle is allocated.
	 *
	 * if we have a 'non-null' handle here then we are coming
	 * from the slow path and handle has already been allocated.
	 */
	if (!handle)
		handle = zs_malloc(zram->mem_pool, comp_len,
				GFP_NOIO |
				__GFP_NOWARN |
				__GFP_HIGHMEM |
				__GFP_MOVABLE |
				__GFP_CMA |
				__GFP_OFFLINABLE); // NOTE: __GFP_OFFLINABLE only for QCOM 5.4 kernel
	if (!handle) {
		zcomp_stream_put(zram->comp);
		atomic64_inc(&zram->stats.writestall);
		/*
		 * The per-cpu compression stream was put above, so sleeping
		 * is allowed on this slow path - which is what the comment
		 * above describes, and the only reason the first attempt
		 * used GFP_NOIO.  It is also the only allocation in this
		 * function that may reclaim: reclaiming from the write path
		 * can recurse back into it through this same device.
		 */
		handle = zs_malloc(zram->mem_pool, comp_len,
				__GFP_KSWAPD_RECLAIM | __GFP_HIGHMEM |
				__GFP_MOVABLE | __GFP_CMA |
				__GFP_OFFLINABLE); // NOTE: __GFP_OFFLINABLE only for QCOM 5.4 kernel
		if (handle)
			goto compress_again;
		return -ENOMEM;
	}

	alloced_pages = zs_get_total_pages(zram->mem_pool);
	update_used_max(zram, alloced_pages);

	if (zram->limit_pages && alloced_pages > zram->limit_pages) {
		zcomp_stream_put(zram->comp);
		zs_free(zram->mem_pool, handle);
		return -ENOMEM;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);

	src = zstrm->buffer;
	if (comp_len == PAGE_SIZE)
		src = kmap_atomic(page);
	memcpy(dst, src, comp_len);
	if (comp_len == PAGE_SIZE)
		kunmap_atomic(src);

	zcomp_stream_put(zram->comp);
	zs_unmap_object(zram->mem_pool, handle);
	atomic64_add(comp_len, &zram->stats.compr_data_size);
out:
	/*
	 * Free memory associated with this sector
	 * before overwriting unused sectors.
	 */
	if (!zram_wait_slot_inflight(zram, index)) {
		/*
		 * A fault-out fetch or a shrink writeback still owns the
		 * slot.  Installing our handle here would overwrite the
		 * page that fetch is about to hand to the reader, and the
		 * fetch's own accounting (its stored/notify counters) would
		 * never be reconciled, so leave the slot entirely alone.
		 * The WARN_ON_ONCE above already fired.  -EAGAIN names the
		 * cause - transient, not a bad page - and surfaces as a
		 * failed write (bio_io_error() turns it into BLK_STS_IOERR)
		 * with the old contents left in the slot for whoever owns
		 * it, which is strictly better than corrupting them.
		 *
		 * Undoing our own bookkeeping is common to both refusals
		 * here, so it happens once in out_undo below.
		 */
		goto out_undo;
	}

	/*
	 * The wait above drained the very bits hybridswap_untrack()
	 * waits on, so it cannot give up here; the WARN_ON_ONCE is
	 * there to say so if that ever stops being true.  Bail rather
	 * than installing into a slot someone else still owns.
	 */
	if (WARN_ON_ONCE(!zram_free_page(zram, index)))
		goto out_undo;

	if (comp_len == PAGE_SIZE) {
		zram_set_flag(zram, index, ZRAM_HUGE);
		atomic64_inc(&zram->stats.huge_pages);
	}

	if (flags) {
		zram_set_flag(zram, index, flags);
		zram_set_element(zram, index, element);
	}  else {
		zram_set_handle(zram, index, handle);
		zram_set_obj_size(zram, index, comp_len);
	}

#ifdef CONFIG_HYBRIDSWAP_CORE
	hybridswap_record(zram, index, page->mem_cgroup);
#endif
	zram_slot_unlock(zram, index);

	/* Update stats */
	atomic64_inc(&zram->stats.pages_stored);
	return ret;

out_undo:
	/*
	 * Give back everything this write charged for work it never
	 * installed.  The object and its compr_data_size charge exist only
	 * on the compression path, which is why the handle test is the
	 * guard; the ZRAM_SAME path allocates nothing but did charge
	 * same_pages above, and zram_free_page()'s ZRAM_SAME branch -
	 * the only place that decrements it - is unreachable here
	 * precisely because the slot's ZRAM_SAME flag was never set.
	 * flags is set to ZRAM_SAME on that path and nowhere else, so it
	 * distinguishes the two exactly and the compression path cannot
	 * be charged for a same page.
	 */
	if (handle) {
		zs_free(zram->mem_pool, handle);
		atomic64_sub(comp_len, &zram->stats.compr_data_size);
	}
	if (flags)
		atomic64_dec(&zram->stats.same_pages);
	zram_slot_unlock(zram, index);
	return -EAGAIN;
}

static int zram_bvec_write(struct zram *zram, struct bio_vec *bvec,
				u32 index, int offset, struct bio *bio)
{
	int ret;
	struct page *page = NULL;
	void *src;
	struct bio_vec vec;

	vec = *bvec;
	if (is_partial_io(bvec)) {
		void *dst;
		/*
		 * This is a partial IO. We need to read the full page
		 * before to write the changes.
		 */
		page = alloc_page(GFP_NOIO|__GFP_HIGHMEM);
		if (!page)
			return -ENOMEM;

		ret = __zram_bvec_read(zram, page, index, bio, true);
		if (ret)
			goto out;

		src = kmap_atomic(bvec->bv_page);
		dst = kmap_atomic(page);
		memcpy(dst + offset, src + bvec->bv_offset, bvec->bv_len);
		kunmap_atomic(dst);
		kunmap_atomic(src);

		vec.bv_page = page;
		vec.bv_len = PAGE_SIZE;
		vec.bv_offset = 0;
	}

	ret = __zram_bvec_write(zram, &vec, index, bio);
out:
	if (is_partial_io(bvec))
		__free_page(page);
	return ret;
}

/*
 * zram_bio_discard - handler on discard request
 * @index: physical block index in PAGE_SIZE units
 * @offset: byte offset within physical block
 */
static void zram_bio_discard(struct zram *zram, u32 index,
			     int offset, struct bio *bio)
{
	size_t n = bio->bi_iter.bi_size;

	/*
	 * zram manages data in physical block size units. Because logical block
	 * size isn't identical with physical block size on some arch, we
	 * could get a discard request pointing to a specific offset within a
	 * certain physical block.  Although we can handle this request by
	 * reading that physiclal block and decompressing and partially zeroing
	 * and re-compressing and then re-storing it, this isn't reasonable
	 * because our intent with a discard request is to save memory.  So
	 * skipping this logical block is appropriate here.
	 */
	if (offset) {
		if (n <= (PAGE_SIZE - offset))
			return;

		n -= (PAGE_SIZE - offset);
		index++;
	}

	while (n >= PAGE_SIZE) {
		zram_slot_lock(zram, index);
		/*
		 * No inflight gate above this point, so a false return is
		 * reachable here: an eswap completer still owns the slot.
		 * Discarding it anyway would free the object that completer
		 * is about to hand to the reader and tear down the swap maps
		 * it is mid-way through installing, so leave the slot
		 * exactly as found and report the discard as not done.
		 * The bio still completes with success - a discard that is
		 * silently skipped is recoverable, one that corrupts a
		 * live fetch is not.
		 */
		if (zram_free_page(zram, index))
			atomic64_inc(&zram->stats.notify_free);
		zram_slot_unlock(zram, index);
		index++;
		n -= PAGE_SIZE;
	}
}

/*
 * Returns errno if it has some problem. Otherwise return 0 or 1.
 * Returns 0 if IO request was done synchronously
 * Returns 1 if IO request was successfully submitted.
 */
static int zram_bvec_rw(struct zram *zram, struct bio_vec *bvec, u32 index,
			int offset, unsigned int op, struct bio *bio)
{
	unsigned long start_time = jiffies;
	struct request_queue *q = zram->disk->queue;
	int ret;

	generic_start_io_acct(q, op, bvec->bv_len >> SECTOR_SHIFT,
			&zram->disk->part0);

	if (!op_is_write(op)) {
		atomic64_inc(&zram->stats.num_reads);
		ret = zram_bvec_read(zram, bvec, index, offset, bio);
		flush_dcache_page(bvec->bv_page);
	} else {
		atomic64_inc(&zram->stats.num_writes);
		ret = zram_bvec_write(zram, bvec, index, offset, bio);
	}

	generic_end_io_acct(q, op, &zram->disk->part0, start_time);

	zram_slot_lock(zram, index);
	zram_accessed(zram, index);
	zram_slot_unlock(zram, index);

	if (unlikely(ret < 0)) {
		if (!op_is_write(op))
			atomic64_inc(&zram->stats.failed_reads);
		else
			atomic64_inc(&zram->stats.failed_writes);
	}

	return ret;
}

static void __zram_make_request(struct zram *zram, struct bio *bio)
{
	int offset;
	u32 index;
	struct bio_vec bvec;
	struct bvec_iter iter;

	index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	offset = (bio->bi_iter.bi_sector &
		  (SECTORS_PER_PAGE - 1)) << SECTOR_SHIFT;

	switch (bio_op(bio)) {
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		zram_bio_discard(zram, index, offset, bio);
		bio_endio(bio);
		return;
	default:
		break;
	}

	bio_for_each_segment(bvec, bio, iter) {
		struct bio_vec bv = bvec;
		unsigned int unwritten = bvec.bv_len;

		do {
			bv.bv_len = min_t(unsigned int, PAGE_SIZE - offset,
							unwritten);
			if (zram_bvec_rw(zram, &bv, index, offset,
					 bio_op(bio), bio) < 0)
				goto out;

			bv.bv_offset += bv.bv_len;
			unwritten -= bv.bv_len;

			update_position(&index, &offset, &bv);
		} while (unwritten);
	}

	bio_endio(bio);
	return;

out:
	bio_io_error(bio);
}

/*
 * Handler function for all zram I/O requests.
 */
static blk_qc_t zram_make_request(struct request_queue *queue, struct bio *bio)
{
	struct zram *zram = queue->queuedata;

	if (!valid_io_request(zram, bio->bi_iter.bi_sector,
					bio->bi_iter.bi_size)) {
		atomic64_inc(&zram->stats.invalid_io);
		goto error;
	}

	__zram_make_request(zram, bio);
	return BLK_QC_T_NONE;

error:
	bio_io_error(bio);
	return BLK_QC_T_NONE;
}

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;

	atomic64_inc(&zram->stats.notify_free);
	if (!zram_slot_trylock(zram, index)) {
		atomic64_inc(&zram->stats.miss_free);
		return;
	}

#ifdef CONFIG_HYBRIDSWAP_CORE
	if (!hybridswap_delete(zram, index)) {
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.miss_free);
		return;
	}
#endif
	/*
	 * hybridswap_delete() above already refused this slot if a
	 * completer claimed it, so a false return here would mean the
	 * claim landed between the two checks.  Leave the slot as found
	 * and count it as a miss: freeing is this function's only job and
	 * there is nothing safe left for it to do instead.
	 */
	if (!zram_free_page(zram, index))
		atomic64_inc(&zram->stats.miss_free);
	zram_slot_unlock(zram, index);
}

static int zram_rw_page(struct block_device *bdev, sector_t sector,
		       struct page *page, unsigned int op)
{
	int offset, ret;
	u32 index;
	struct zram *zram;
	struct bio_vec bv;

	if (PageTransHuge(page))
		return -ENOTSUPP;
	zram = bdev->bd_disk->private_data;

	if (!valid_io_request(zram, sector, PAGE_SIZE)) {
		atomic64_inc(&zram->stats.invalid_io);
		ret = -EINVAL;
		goto out;
	}

	index = sector >> SECTORS_PER_PAGE_SHIFT;
	offset = (sector & (SECTORS_PER_PAGE - 1)) << SECTOR_SHIFT;

	bv.bv_page = page;
	bv.bv_len = PAGE_SIZE;
	bv.bv_offset = 0;

	ret = zram_bvec_rw(zram, &bv, index, offset, op, NULL);
out:
	/*
	 * If I/O fails, just return error(ie, non-zero) without
	 * calling page_endio.
	 * It causes resubmit the I/O with bio request by upper functions
	 * of rw_page(e.g., swap_readpage, __swap_writepage) and
	 * bio->bi_end_io does things to handle the error
	 * (e.g., SetPageError, set_page_dirty and extra works).
	 */
	if (unlikely(ret < 0))
		return ret;

	switch (ret) {
	case 0:
		page_endio(page, op_is_write(op), 0);
		break;
	case 1:
		ret = 0;
		break;
	default:
		WARN_ON(1);
	}
	return ret;
}

/*
 * Body of zram_reset_device().  The caller must already hold
 * zram->init_lock for write: zram_remove() needs the teardown to be one
 * critical section with its hybridswap bound check, and the rwsem is not
 * recursive, so taking it here would self-deadlock.  It must also have
 * already established that hybridswap is not bound: zram_remove() does
 * that in the same critical section, zram_reset_device() in its own.
 * That is why the check lives in the wrapper rather than here.  Use the
 * zram_reset_device() wrapper unless you already hold the lock and have
 * run the check.
 */
static void zram_reset_device_locked(struct zram *zram)
{
	struct zcomp *comp;
	u64 disksize;

	/*
	 * The contract this split exists to enforce, now checked rather
	 * than only described: both callers take init_lock for write
	 * before getting here, because the rwsem is not recursive and
	 * taking it again would self-deadlock.
	 */
	lockdep_assert_held(&zram->init_lock);

	zram->limit_pages = 0;

	if (!init_done(zram))
		return;

	comp = zram->comp;
	disksize = zram->disksize;
	zram->disksize = 0;

	set_capacity(zram->disk, 0);
	part_stat_set_all(&zram->disk->part0, 0);

	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(zram, disksize);
	memset(&zram->stats, 0, sizeof(zram->stats));
	zcomp_destroy(comp);
	reset_bdev(zram);
}

static int zram_reset_device(struct zram *zram)
{
	int ret = 0;

	down_write(&zram->init_lock);

	/*
	 * The gate zram_remove() has, and for the same reason: this runs
	 * the identical zram_meta_free() teardown, and zram_free_page()
	 * ends up in hybridswap_swap_sorted_list_del(), which destroys the
	 * swap maps of every ZRAM_WB page.  Unlike zram_remove() nothing
	 * is freed here, so hybridswap would survive the reset - but its
	 * tables and the per-memcg hybs->zram pointers would still
	 * describe slots whose metadata table and zsmalloc pool this call
	 * has just released, and which the next disksize write allocates
	 * fresh ones over.  Refusing is the only way to keep the two
	 * views of the device consistent.
	 */
#ifdef CONFIG_HYBRIDSWAP_CORE
	if (hybridswap_zram_bound(zram)) {
		pr_err("Cannot reset %s: still bound to hybridswap\n",
				zram->disk->disk_name);
		ret = -EBUSY;
	}
#endif

	if (!ret)
		zram_reset_device_locked(zram);
	up_write(&zram->init_lock);

	return ret;
}

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 disksize;
	struct zcomp *comp;
	struct zram *zram = dev_to_zram(dev);
	int err;

	disksize = memparse(buf, NULL);
	if (!disksize)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Cannot change disksize for initialized device\n");
		err = -EBUSY;
		goto out_unlock;
	}

	/*
	 * zram_remove() has committed to freeing this device and has
	 * already torn the old one down, so re-arming disksize here would
	 * hand the hybridswap bind path a non-zero disksize to latch onto
	 * a struct zram that is about to be kfree()d.  Refuse before
	 * zram_meta_alloc() so nothing is allocated.
	 */
	if (zram->dying) {
		pr_info("Cannot set disksize for a device being removed\n");
		err = -EBUSY;
		goto out_unlock;
	}

	disksize = PAGE_ALIGN(disksize);
	if (!zram_meta_alloc(zram, disksize)) {
		err = -ENOMEM;
		goto out_unlock;
	}

	comp = zcomp_create(zram->compressor);
	if (IS_ERR(comp)) {
		pr_err("Cannot initialise %s compressing backend\n",
				zram->compressor);
		err = PTR_ERR(comp);
		goto out_free_meta;
	}

	zram->comp = comp;
	zram->disksize = disksize;
	set_capacity(zram->disk, zram->disksize >> SECTOR_SHIFT);

	revalidate_disk(zram->disk);
	up_write(&zram->init_lock);

	return len;

out_free_meta:
	zram_meta_free(zram, disksize);
out_unlock:
	up_write(&zram->init_lock);
	return err;
}

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned short do_reset;
	struct zram *zram;
	struct block_device *bdev;

	ret = kstrtou16(buf, 10, &do_reset);
	if (ret)
		return ret;

	if (!do_reset)
		return -EINVAL;

	zram = dev_to_zram(dev);
	bdev = bdget_disk(zram->disk, 0);
	if (!bdev)
		return -ENOMEM;

	mutex_lock(&bdev->bd_mutex);
	/* Do not reset an active device or claimed device */
	if (bdev->bd_openers || zram->claim) {
		mutex_unlock(&bdev->bd_mutex);
		bdput(bdev);
		return -EBUSY;
	}

	/* From now on, anyone can't open /dev/zram[0-9] */
	zram->claim = true;
	mutex_unlock(&bdev->bd_mutex);

	/* Make sure all the pending I/O are finished */
	fsync_bdev(bdev);
	/*
	 * May refuse (-EBUSY, hybridswap still bound) before touching
	 * anything.  The claim and the bdev reference are released on
	 * that path exactly as on success, and the error is returned
	 * below through the usual "ret ? ret : count" so userspace sees
	 * it rather than a success it did not get.
	 */
	ret = zram_reset_device(zram);
	revalidate_disk(zram->disk);
	bdput(bdev);

	mutex_lock(&bdev->bd_mutex);
	zram->claim = false;
	mutex_unlock(&bdev->bd_mutex);

	return ret ? ret : len;
}

static int zram_open(struct block_device *bdev, fmode_t mode)
{
	int ret = 0;
	struct zram *zram;

	WARN_ON(!mutex_is_locked(&bdev->bd_mutex));

	zram = bdev->bd_disk->private_data;
	/* zram was claimed to reset so open request fails */
	if (zram->claim)
		ret = -EBUSY;

	return ret;
}

static const struct block_device_operations zram_devops = {
	.open = zram_open,
	.swap_slot_free_notify = zram_slot_free_notify,
	.rw_page = zram_rw_page,
	.owner = THIS_MODULE
};

static DEVICE_ATTR_WO(compact);
static DEVICE_ATTR_RW(disksize);
static DEVICE_ATTR_RO(initstate);
static DEVICE_ATTR_WO(reset);
static DEVICE_ATTR_WO(mem_limit);
static DEVICE_ATTR_WO(mem_used_max);
static DEVICE_ATTR_WO(idle);
static DEVICE_ATTR_RW(max_comp_streams);
static DEVICE_ATTR_RW(comp_algorithm);
#ifdef CONFIG_HYBRIDSWAP_ZRAM_WRITEBACK
static DEVICE_ATTR_RW(backing_dev);
static DEVICE_ATTR_WO(writeback);
static DEVICE_ATTR_RW(writeback_limit);
static DEVICE_ATTR_RW(writeback_limit_enable);
#endif
#ifdef CONFIG_HYBRIDSWAP
static DEVICE_ATTR_RO(hybridswap_vmstat);
static DEVICE_ATTR_RW(hybridswap_loglevel);
static DEVICE_ATTR_RW(hybridswap_enable);
#endif
#ifdef CONFIG_HYBRIDSWAP_SWAPD
static DEVICE_ATTR_RW(hybridswap_swapd_pause);
#endif
#ifdef CONFIG_HYBRIDSWAP_CORE
static DEVICE_ATTR_RW(hybridswap_core_enable);
static DEVICE_ATTR_RW(hybridswap_loop_device);
static DEVICE_ATTR_RW(hybridswap_dev_life);
static DEVICE_ATTR_RW(hybridswap_quota_day);
static DEVICE_ATTR_RO(hybridswap_report);
static DEVICE_ATTR_RO(hybridswap_stat_snap);
static DEVICE_ATTR_RO(hybridswap_meminfo);
static DEVICE_ATTR_RW(hybridswap_zram_increase);
#endif

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_compact.attr,
	&dev_attr_mem_limit.attr,
	&dev_attr_mem_used_max.attr,
	&dev_attr_idle.attr,
	&dev_attr_max_comp_streams.attr,
	&dev_attr_comp_algorithm.attr,
#ifdef CONFIG_HYBRIDSWAP_ZRAM_WRITEBACK
	&dev_attr_backing_dev.attr,
	&dev_attr_writeback.attr,
	&dev_attr_writeback_limit.attr,
	&dev_attr_writeback_limit_enable.attr,
#endif
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
#ifdef CONFIG_HYBRIDSWAP_ZRAM_WRITEBACK
	&dev_attr_bd_stat.attr,
#endif
	&dev_attr_debug_stat.attr,
#ifdef CONFIG_HYBRIDSWAP
	&dev_attr_hybridswap_vmstat.attr,
	&dev_attr_hybridswap_loglevel.attr,
	&dev_attr_hybridswap_enable.attr,
#endif
#ifdef CONFIG_HYBRIDSWAP_SWAPD
	&dev_attr_hybridswap_swapd_pause.attr,
#endif
#ifdef CONFIG_HYBRIDSWAP_CORE
	&dev_attr_hybridswap_core_enable.attr,
	&dev_attr_hybridswap_report.attr,
	&dev_attr_hybridswap_meminfo.attr,
	&dev_attr_hybridswap_stat_snap.attr,
	&dev_attr_hybridswap_loop_device.attr,
	&dev_attr_hybridswap_dev_life.attr,
	&dev_attr_hybridswap_quota_day.attr,
	&dev_attr_hybridswap_zram_increase.attr,
#endif
	NULL,
};

static const struct attribute_group zram_disk_attr_group = {
	.attrs = zram_disk_attrs,
};

static const struct attribute_group *zram_disk_attr_groups[] = {
	&zram_disk_attr_group,
	NULL,
};

/*
 * Allocate and initialize new zram device. the function returns
 * '>= 0' device_id upon success, and negative value otherwise.
 */
static int zram_add(void)
{
	struct zram *zram;
	struct request_queue *queue;
	int ret, device_id;

	zram = kzalloc(sizeof(struct zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;

	ret = idr_alloc(&zram_index_idr, zram, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_dev;
	device_id = ret;

	init_rwsem(&zram->init_lock);
#ifdef CONFIG_HYBRIDSWAP_ZRAM_WRITEBACK
	spin_lock_init(&zram->wb_limit_lock);
#endif
	queue = blk_alloc_queue(GFP_KERNEL);
	if (!queue) {
		pr_err("Error allocating disk queue for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_idr;
	}

	blk_queue_make_request(queue, zram_make_request);

	/* gendisk structure */
	zram->disk = alloc_disk(1);
	if (!zram->disk) {
		pr_err("Error allocating disk structure for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_queue;
	}

	zram->disk->major = zram_major;
	zram->disk->first_minor = device_id;
	zram->disk->fops = &zram_devops;
	zram->disk->queue = queue;
	zram->disk->queue->queuedata = zram;
	zram->disk->private_data = zram;
	snprintf(zram->disk->disk_name, 16, "zram%d", device_id);

	/* Actual capacity set using syfs (/sys/block/zram<id>/disksize */
	set_capacity(zram->disk, 0);
	/* zram devices sort of resembles non-rotational disks */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, zram->disk->queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, zram->disk->queue);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(zram->disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(zram->disk->queue,
					ZRAM_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(zram->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(zram->disk->queue, PAGE_SIZE);
	zram->disk->queue->limits.discard_granularity = PAGE_SIZE;
	blk_queue_max_discard_sectors(zram->disk->queue, UINT_MAX);
	blk_queue_flag_set(QUEUE_FLAG_DISCARD, zram->disk->queue);

	/*
	 * zram_bio_discard() will clear all logical blocks if logical block
	 * size is identical with physical block size(PAGE_SIZE). But if it is
	 * different, we will skip discarding some parts of logical blocks in
	 * the part of the request range which isn't aligned to physical block
	 * size.  So we can't ensure that all discarded logical blocks are
	 * zeroed.
	 */
	if (ZRAM_LOGICAL_BLOCK_SIZE == PAGE_SIZE)
		blk_queue_max_write_zeroes_sectors(zram->disk->queue, UINT_MAX);

	zram->disk->queue->backing_dev_info->capabilities |=
			(BDI_CAP_STABLE_WRITES | BDI_CAP_SYNCHRONOUS_IO);
	device_add_disk(NULL, zram->disk, zram_disk_attr_groups);

	strlcpy(zram->compressor, default_compressor, sizeof(zram->compressor));

	zram_debugfs_register(zram);
	pr_info("Added device: %s\n", zram->disk->disk_name);
	return device_id;

out_free_queue:
	blk_cleanup_queue(queue);
out_free_idr:
	idr_remove(&zram_index_idr, device_id);
out_free_dev:
	kfree(zram);
	return ret;
}

static int zram_remove(struct zram *zram)
{
	struct block_device *bdev;

	bdev = bdget_disk(zram->disk, 0);
	if (!bdev)
		return -ENOMEM;

	mutex_lock(&bdev->bd_mutex);
	if (bdev->bd_openers || zram->claim) {
		mutex_unlock(&bdev->bd_mutex);
		bdput(bdev);
		return -EBUSY;
	}

	zram->claim = true;
	mutex_unlock(&bdev->bd_mutex);

	/*
	 * Make sure all the pending I/O are finished.  Deliberately
	 * before down_write(): fsync_bdev() sleeps in an unbounded
	 * filemap_write_and_wait() on the backing device's inode, and
	 * init_lock is not recursive and has no owner-death requeue, so
	 * holding it across that sleep blocks every other init_lock user
	 * - disksize_store(), reset_store(), hybridswap_loop_device_show()
	 * and the hybridswap bind path - for as long as the writeback
	 * takes.  Nothing here needs the lock: zram->claim, set above
	 * under bd_mutex, is what keeps anyone else off the device, and
	 * the bios this waits for never took init_lock in the first
	 * place.
	 */
	fsync_bdev(bdev);

	zram_debugfs_unregister(zram);

	/*
	 * One write region for the bound check AND the teardown, so they
	 * are a single decision.  hybridswap_loop_device_store() takes
	 * this same lock before hybridswap_enable_lock and publishes
	 * global_settings.zram and the loop-device binding under it, so a
	 * check taken outside the lock would race a bind that lands
	 * between it and the teardown below, and the bind would hand
	 * hybridswap a table and a device that this call is about to
	 * free.
	 *
	 * Lock order: hot_remove_store()'s zram_index_mutex -> init_lock
	 * -> hybridswap_enable_lock.  The last edge is the order the bind
	 * path already documents (init_lock -> hybridswap_enable_lock), so
	 * it is consistent rather than new, and nothing reachable from
	 * here takes zram_index_mutex, so the outer edge cannot invert.
	 *
	 * The lock is dropped before del_gendisk() on purpose:
	 * del_gendisk() waits for in-flight kernfs writers, and a
	 * hybridswap_loop_device_store() already inside the store could be
	 * one of them, blocked on this very rwsem.  The zram->dying flag
	 * set below is what covers that window instead.
	 */
	down_write(&zram->init_lock);

	/*
	 * Refuse while hybridswap is still bound to THIS device.  Its
	 * global_settings.zram, the per-memcg hybs->zram, the tables
	 * published through zram->infos and swapd's own swapd_zram all
	 * survive swapoff, and only hybridswap teardown clears them, so
	 * the kfree() below would leave hybridswap dereferencing freed
	 * slab - it would take zram->init_lock from
	 * hybridswap_unbind_bdev(), the swapd kthreads would keep reading
	 * swapd_zram on their next round, and every eswap request still
	 * holding a zram pointer would fault.
	 *
	 * This is a refusal, not a cleanup: the only things done so far
	 * are the claim and the debugfs node, and the branch undoes both,
	 * so the caller keeps a usable device and can retry.  Note that
	 * the core-side bindings are only released when the core is torn
	 * down - the sysfs enable knobs clear the enable flag but leave
	 * the loop device bound and zram->infos published - while the
	 * swapd binding goes away with hybridswap_disable().  So a core-
	 * bound device stays -EBUSY until teardown, and a swapd-only one
	 * until the enable knob is cleared.  That is the intended trade:
	 * leaking one device beats handing hybridswap a freed struct
	 * zram.  Module exit is the backstop: zram_exit() runs
	 * hybridswap_exit(), whose swapd_exit() and
	 * hybridswap_exit_core() clear all of them before
	 * destroy_devices() gets here.
	 */
#ifdef CONFIG_HYBRIDSWAP_CORE
	if (hybridswap_zram_bound(zram)) {
		pr_err("Cannot remove %s: still bound to hybridswap\n",
				zram->disk->disk_name);
		up_write(&zram->init_lock);
		/* Undo the claim and the debugfs node, let the caller retry */
		zram_debugfs_register(zram);
		mutex_lock(&bdev->bd_mutex);
		zram->claim = false;
		mutex_unlock(&bdev->bd_mutex);
		bdput(bdev);

		return -EBUSY;
	}
#endif

	zram_reset_device_locked(zram);
	/*
	 * From here the device is doomed, so refuse the sysfs writers
	 * that can still run before del_gendisk() retires the nodes: they
	 * all check this under init_lock.  Without it, a disksize_store()
	 * landing in the unlock-to-kfree() gap would re-arm
	 * zram->disksize and allocate a fresh table, and a
	 * hybridswap_loop_device_store() would then find a non-zero
	 * disksize, bind, and publish global_settings.zram - a pointer
	 * the kfree() below would leave dangling.
	 */
	zram->dying = true;
	up_write(&zram->init_lock);
	bdput(bdev);

	pr_info("Removed device: %s\n", zram->disk->disk_name);

	del_gendisk(zram->disk);
	blk_cleanup_queue(zram->disk->queue);
	put_disk(zram->disk);

	kfree(zram);
	return 0;
}

/* zram-control sysfs attributes */

/*
 * NOTE: hot_add attribute is not the usual read-only sysfs attribute. In a
 * sense that reading from this file does alter the state of your system -- it
 * creates a new un-initialized zram device and returns back this device's
 * device_id (or an error code if it fails to create a new device).
 */
static ssize_t hot_add_show(struct class *class,
			struct class_attribute *attr,
			char *buf)
{
	int ret;

	mutex_lock(&zram_index_mutex);
	ret = zram_add();
	mutex_unlock(&zram_index_mutex);

	if (ret < 0)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}
static struct class_attribute class_attr_hot_add =
	__ATTR(hot_add, 0400, hot_add_show, NULL);

static ssize_t hot_remove_store(struct class *class,
			struct class_attribute *attr,
			const char *buf,
			size_t count)
{
	struct zram *zram;
	int ret, dev_id;

	/* dev_id is gendisk->first_minor, which is `int' */
	ret = kstrtoint(buf, 10, &dev_id);
	if (ret)
		return ret;
	if (dev_id < 0)
		return -EINVAL;

	mutex_lock(&zram_index_mutex);

	zram = idr_find(&zram_index_idr, dev_id);
	if (zram) {
		ret = zram_remove(zram);
		if (!ret)
			idr_remove(&zram_index_idr, dev_id);
	} else {
		ret = -ENODEV;
	}

	mutex_unlock(&zram_index_mutex);
	return ret ? ret : count;
}
static CLASS_ATTR_WO(hot_remove);

static struct attribute *zram_control_class_attrs[] = {
	&class_attr_hot_add.attr,
	&class_attr_hot_remove.attr,
	NULL,
};
ATTRIBUTE_GROUPS(zram_control_class);

static struct class zram_control_class = {
	.name		= "zram-control",
	.owner		= THIS_MODULE,
	.class_groups	= zram_control_class_groups,
};

/*
 * Tear down every registered zram.  zram_remove()'s refusal is propagated
 * rather than swallowed: this cannot abort - the remaining devices still
 * have to go - so the return is inspected to decide whether
 * unregister_blkdev() is still safe.
 */
static void destroy_devices(void)
{
	bool unregister_major = true;
	void *ptr;
	int id;

	class_unregister(&zram_control_class);
	/*
	 * Walk the idr by hand rather than through idr_for_each(): the
	 * removal can now be refused, and idr_for_each() would stop at the
	 * first refusal and leave every device after it registered.  The id
	 * is also what names the survivor below.
	 *
	 * The message is deliberately short - a split string literal is not
	 * greppable, which defeats the point of naming the device.  The
	 * consequence of the refusal, that the survivor's gendisk still
	 * points fops at zram module text that is about to be freed, is
	 * spelled out in the comment below instead.
	 */
	for (id = 0; (ptr = idr_get_next(&zram_index_idr, &id)) != NULL; id++) {
		if (zram_remove(ptr)) {
			unregister_major = false;
			pr_err("zram%d: removal refused, device kept\n", id);
		}
	}
	zram_debugfs_destroy();
	idr_destroy(&zram_index_idr);
	/*
	 * A device that refused to be removed is still registered, with
	 * zram->disk->fops still pointing into this module's text and its
	 * major/minor still carrying this driver's name.  Name that
	 * consequence, then keep the major: unregistering it would hand the
	 * live gendisk's major/minor to the next register_blkdev() caller,
	 * which is strictly worse than a leaked major number, and holding
	 * the major is the only thing that still makes the survivor's
	 * device node name this driver.  The WARN_ON_ONCE is the marker an
	 * operator greps for when the resulting use-after-free fires.
	 */
	if (unregister_major)
		unregister_blkdev(zram_major, "zram");
	else
		WARN_ON_ONCE(1);
	cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
}

static int __init zram_init(void)
{
	int ret;

	BUILD_BUG_ON(__NR_ZRAM_PAGEFLAGS > BITS_PER_LONG);

	ret = cpuhp_setup_state_multi(CPUHP_ZCOMP_PREPARE, "block/zram:prepare",
				      zcomp_cpu_up_prepare, zcomp_cpu_dead);
	if (ret < 0)
		return ret;

	ret = class_register(&zram_control_class);
	if (ret) {
		pr_err("Unable to register zram-control class\n");
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return ret;
	}

	zram_debugfs_create();
	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_err("Unable to get major number\n");
		class_unregister(&zram_control_class);
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return -EBUSY;
	}

	while (num_devices != 0) {
		mutex_lock(&zram_index_mutex);
		ret = zram_add();
		mutex_unlock(&zram_index_mutex);
		if (ret < 0)
			goto out_error;
		num_devices--;
	}

#ifdef CONFIG_HYBRIDSWAP
	ret = hybridswap_pre_init();
	if (ret)
		goto out_error;
#endif
	return 0;

out_error:
	destroy_devices();
	return ret;
}

static void __exit zram_exit(void)
{
	/*
	 * hybridswap first, destroy_devices() second, and the order is
	 * load-bearing in both directions.
	 *
	 * hybridswap_exit() must see every device still alive.  Its
	 * swapd_exit() stops the kthreads that dereference swapd_zram, and
	 * its hybridswap_exit_core() reads global_settings.zram, walks the
	 * memcg list, detaches zram->infos and unbinds the loop device
	 * through hybridswap_unbind_bdev(), which takes zram->init_lock.
	 * After destroy_devices() those would all be reads and lock
	 * acquisitions on a struct zram that kfree() has returned to
	 * kmem_cache - and zram_reset_device_locked() would already have
	 * run reset_bdev(), so the unbind would be a second close of a
	 * bdev that is gone.  The slab cache it destroys last is the one
	 * every MEMCGRP_ITEM_DATA(memcg) object is carved from, so the
	 * per-memcg walk has to have run before that too.
	 *
	 * destroy_devices() needs hybridswap to have finished, not to be
	 * alive: it wants exactly the pointers cleared.  zram_remove()'s
	 * hybridswap_zram_bound() check reads global_settings.zram,
	 * zram->infos and swapd_zram, all NULL by then, so the -EBUSY
	 * refusal does not fire and the device is torn down as normal; it
	 * only touches the still-valid hybridswap_enable_lock mutex, and
	 * the check itself still lives.  zram_free_page()'s
	 * hybridswap_untrack() and zram_slot_free_notify()'s
	 * hybridswap_delete() both return at their
	 * hybridswap_core_enabled() test, which hybridswap_exit_core() has
	 * already cleared, so no path reaches the freed tables or the
	 * destroyed slab cache.  The hybridswap symbols themselves are
	 * still mapped - this is module_exit, the text is not unmapped
	 * until it returns.
	 *
	 * hybridswap_pre_init() is called from zram_init() and unwinds
	 * itself on every failure path (it drops the cftypes it added,
	 * frees the duplicated tables and destroys hybridswap_cache), so
	 * there is no matching pre-deinit to call here: reaching this
	 * function already implies pre_init() succeeded.
	 */
#if defined(CONFIG_HYBRIDSWAP) && defined(CONFIG_MODULES)
	/*
	 * Gated on CONFIG_MODULES because that is the only build in which
	 * module_exit() can mean anything.  module.h states outright that
	 * "if the driver is statically compiled into the kernel, module_exit()
	 * has no effect", and a kernel carrying no module core has no
	 * exitcall walker either, so in a modules-disabled build this call
	 * is unreachable in every configuration rather than merely unused.
	 * Compiling it there would be a teardown that no sequence of events
	 * can run.
	 */
	hybridswap_exit();
#endif
	destroy_devices();
}

module_init(zram_init);
module_exit(zram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of pre-created zram devices");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_DESCRIPTION("Compressed RAM Block Device");
MODULE_ALIAS("zram");

#ifndef _ZRAM_DRV_INTERNAL_H_
#define _ZRAM_DRV_INTERNAL_H_
#ifdef BIT
#undef BIT
#define BIT(nr)		(1lu << (nr))
#endif

#ifdef CONFIG_HYBRIDSWAP_CORE
struct zram;
void hybridswap_unbind_bdev(struct zram *zram);
void hybridswap_unbind_bdev_locked(struct zram *zram);
#else
static inline void hybridswap_unbind_bdev(struct zram *zram) { }
static inline void hybridswap_unbind_bdev_locked(struct zram *zram) { }
#endif

#define zram_slot_lock(zram, index) (bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags))

#define zram_slot_unlock(zram, index) (bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags))

#define init_done(zram)  (!!(zram->disksize))

#define dev_to_zram(dev) ((struct zram *)dev_to_disk(dev)->private_data)

#define zram_get_handle(zram, index) (zram->table[index].handle)

#define zram_set_handle(zram, index, handle_val) (zram->table[index].handle = handle_val)

#define zram_test_flag(zram, index,  flag) (zram->table[index].flags & BIT(flag))

#define zram_set_flag(zram, index, flag) (zram->table[index].flags |= BIT(flag))

#define zram_clear_flag(zram, index, flag) (zram->table[index].flags &= ~BIT(flag))

#define zram_set_element(zram, index, elem) (zram->table[index].element = elem)

#define zram_get_obj_size(zram, index) (zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1))

#define zram_set_obj_size(zram, index, size) do {\
	unsigned long flags = zram->table[index].flags >> ZRAM_FLAG_SHIFT; \
	zram->table[index].flags = (flags << ZRAM_FLAG_SHIFT) | size; \
} while(0)

#endif

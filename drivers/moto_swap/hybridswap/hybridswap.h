// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#ifndef HYBRIDSWAP_H
#define HYBRIDSWAP_H
extern int __init hybridswap_pre_init(void);
extern void __exit hybridswap_exit(void);
extern ssize_t hybridswap_vmstat_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_loglevel_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t hybridswap_loglevel_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t hybridswap_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf);
#ifdef CONFIG_HYBRIDSWAP_CORE
extern void hybridswap_record(struct zram *zram, u32 index, struct mem_cgroup *memcg);
extern void hybridswap_untrack(struct zram *zram, u32 index);
/*
 * Declared here rather than in hybridswap_internal.h: zram_drv.c includes
 * this header and cannot include the internal one (it defines a file scope
 * hybridswap_loglevel attribute that the internal header declares as a
 * function), and hybridswap_internal.h's non-CORE stub is unreachable
 * because the only definition lives in the CORE-only hybridswap_eswap.o.
 */
extern bool hybridswap_zram_bound(struct zram *zram);
extern int hybridswap_page_fault(struct zram *zram, u32 index);
extern bool hybridswap_delete(struct zram *zram, u32 index);

extern ssize_t hybridswap_report_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_stat_snap_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_meminfo_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_core_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t hybridswap_core_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_loop_device_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t hybridswap_loop_device_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_dev_life_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t hybridswap_dev_life_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_quota_day_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t hybridswap_quota_day_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t hybridswap_zram_increase_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t hybridswap_zram_increase_show(struct device *dev,
		struct device_attribute *attr, char *buf);
#endif

#ifdef CONFIG_HYBRIDSWAP_SWAPD
extern ssize_t hybridswap_swapd_pause_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t hybridswap_swapd_pause_show(struct device *dev,
		struct device_attribute *attr, char *buf);
/*
 * The device swapd is bound to, or NULL once swapd_exit() has run.
 * An accessor because swapd_zram is file-static in hybridswap_swapd.c
 * and hybridswap_zram_bound() lives in hybridswap_eswap.c, which is
 * built for CONFIG_HYBRIDSWAP_CORE whether or not swapd is - hence the
 * stub in the #else.  Declared here rather than in
 * hybridswap_internal.h because hybridswap_swapd.c does not include
 * that one, and the definition needs a visible prototype.
 */
extern struct zram *hybridswap_swapd_zram(void);
#else
static inline struct zram *hybridswap_swapd_zram(void) { return NULL; }
#endif
static inline bool current_is_mswapd(void)
{
#ifdef CONFIG_HYBRIDSWAP_SWAPD
	return (strncmp(current->comm, "mswapd:", sizeof("mswapd:") - 1) == 0);
#else
	return false;
#endif
}
#endif /* HYBRIDSWAP_H */

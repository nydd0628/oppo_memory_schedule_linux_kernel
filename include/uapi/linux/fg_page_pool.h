/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_FG_PAGE_POOL_H
#define _UAPI_LINUX_FG_PAGE_POOL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FG_POOL_ABI_VERSION 1
#define FG_POOL_MAX_MEMBERS 32

enum fg_pool_mode {
	FG_POOL_OFF,
	FG_POOL_SHARED,
	FG_POOL_FOREGROUND,
	FG_POOL_FOREGROUND_FAST,
};

/* Accepted for V7 callers. V8 uses a fixed refill target. */
#define FG_POOL_CONFIG_ELASTIC	(1U << 0)
#define FG_POOL_CONFIG_RECLAIM_REFILL	(1U << 1)
#define FG_POOL_CONFIG_HYBRID_REFILL	(1U << 2)

struct fg_pool_config {
	__u32 version;
	__u32 mode;
	__s32 nid;
	__u32 zone;
	__u32 target_pages;
	__u32 batch_pages; /* Deprecated, ignored by V8. */
	__u32 interval_ms;
	__u32 cooldown_ms;
	__u32 flags;
	__u32 startup_pages; /* Zero starts steady refill immediately. */
	__u32 reserved[2];
};

struct fg_pool_members {
	__u32 version;
	__u32 count;
	__aligned_u64 generation;
	__u32 lease_ms;
	__u32 reserved;
	__s32 pidfds[FG_POOL_MAX_MEMBERS];
};

struct fg_pool_status {
	struct fg_pool_config config;
	__u32 pages;
	__u32 high_watermark;
	__aligned_u64 generation;
	__aligned_u64 refilled;
	__aligned_u64 attempts;
	__aligned_u64 hit;
	__aligned_u64 empty;
	__aligned_u64 denied;
	__aligned_u64 constrained;
	__aligned_u64 watermark_stops;
	__aligned_u64 shrink_returned;
	__aligned_u64 oom_returned;
	__aligned_u64 drained;
	__aligned_u64 isolation_returned;
	__aligned_u64 stale_returned;
	__aligned_u64 bad_pages;
};

struct fg_pool_refill_stats {
	__aligned_u64 buddy;
	__aligned_u64 kswapd;
};

#define FG_POOL_SET_CONFIG	_IOW(0xf6, 0, struct fg_pool_config)
#define FG_POOL_REPLACE_MEMBERS	_IOW(0xf6, 1, struct fg_pool_members)
#define FG_POOL_GET_STATUS	_IOR(0xf6, 2, struct fg_pool_status)
#define FG_POOL_DISABLE		_IO(0xf6, 3)
#define FG_POOL_GET_REFILL_STATS	_IOR(0xf6, 4, struct fg_pool_refill_stats)
/* Returns EAGAIN until the startup target is stocked. */
#define FG_POOL_START_STEADY	_IO(0xf6, 5)

#endif

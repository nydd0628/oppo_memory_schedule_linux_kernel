/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_FG_PAGE_POOL_H
#define _MM_FG_PAGE_POOL_H

#include <uapi/linux/fg_page_pool.h>

#define FG_POOL_MAX_BATCH 64

enum fg_pool_return_reason {
	FG_POOL_SHRINK,
	FG_POOL_OOM,
	FG_POOL_DRAIN,
	FG_POOL_ISOLATION,
};

struct zone;
struct list_head;

/* The controller serializes configuration and status snapshots. */
void fg_pool_enable(struct zone *zone, const struct fg_pool_config *cfg);
void fg_pool_disable(void);
int fg_pool_start_steady(void);
void fg_pool_refill(void);
unsigned int fg_pool_free_kswapd_pages(struct list_head *pages);
unsigned long fg_pool_release(unsigned long nr, enum fg_pool_return_reason why);
unsigned long fg_pool_shrinkable(void);
void fg_pool_drain_isolated(void);
void fg_pool_snapshot(struct fg_pool_status *status);
void fg_pool_refill_snapshot(struct fg_pool_refill_stats *stats);

/* The caller holds rcu_read_lock() through the actual page removal. */
bool fg_pool_current_eligible(void);

#endif

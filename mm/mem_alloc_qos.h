/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MEM_ALLOC_QOS_H
#define _MM_MEM_ALLOC_QOS_H

#include <linux/gfp.h>
#include <linux/jump_label.h>

enum mem_qos_domain {
	QOS_DRIVER,
	QOS_ANON,
};

enum mem_qos_role {
	QOS_TOP,
	QOS_FGS,
	QOS_ROLES,
};

enum mem_qos_reclaim_source {
	QOS_RECLAIM_NONE,
	QOS_RECLAIM_KSWAPD,
	QOS_RECLAIM_DIRECT,
	QOS_RECLAIM_MEMCG,
	QOS_RECLAIM_SOURCES,
};

#define QOS_DRIVER_GFP	(GFP_KERNEL | __GFP_ZERO)
#define QOS_ANON_GFP	(GFP_HIGHUSER_MOVABLE | __GFP_ZERO | __GFP_CMA | __GFP_COMP)

enum mem_qos_path {
	QOS_FAST,
	QOS_SLOW,
	QOS_POOL,
	QOS_REJECTED,
	QOS_CANCELLED,
	QOS_PERMITTED,
	QOS_FGS_BYPASS,
	QOS_NR_PATHS,
};

struct mem_qos_call {
	u64 id;
	u64 parent;
	int srcu_index;
	int tid;
	u8 role;
	u8 domain;
	bool force;
	bool gate;
	bool permitted;
};

#ifdef CONFIG_MEM_ALLOC_QOS

DECLARE_STATIC_KEY_FALSE(mem_qos_anon_key);
DECLARE_STATIC_KEY_FALSE(mem_qos_reclaim_key);

struct page *mem_qos_alloc_pages(gfp_t gfp, enum mem_qos_domain domain,
				 bool force, u64 parent);
bool mem_qos_begin(struct mem_qos_call *call, enum mem_qos_domain domain,
		   bool force, u64 parent);
void mem_qos_end(struct mem_qos_call *call, enum mem_qos_path path, bool success);
struct page *mem_qos_request(struct mem_qos_call *call, gfp_t gfp,
			     struct zone *preferred_zone);
unsigned int mem_qos_capture_reclaimed(struct list_head *pages,
				       enum mem_qos_reclaim_source source);
bool mem_qos_reclaimed_eligible(struct page *page);
void mem_qos_prepare_reclaimed(struct page *page, gfp_t gfp);
unsigned int free_unref_page_list_qos(struct list_head *pages,
				     enum mem_qos_reclaim_source source);

#endif
#endif

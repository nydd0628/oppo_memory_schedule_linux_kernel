/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MEM_ALLOC_QOS_H
#define _MM_MEM_ALLOC_QOS_H

#include <linux/gfp.h>
#include <linux/jump_label.h>

enum mem_qos_domain {
	QOS_DRIVER,
	QOS_ANON,
};

#define QOS_DRIVER_GFP	(GFP_KERNEL | __GFP_ZERO)
#define QOS_ANON_GFP	(GFP_HIGHUSER_MOVABLE | __GFP_ZERO | __GFP_CMA | __GFP_COMP)

enum mem_qos_path {
	QOS_FAST,
	QOS_SLOW,
	QOS_POOL,
	QOS_REJECTED,
	QOS_CANCELLED,
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
};

#ifdef CONFIG_MEM_ALLOC_QOS

DECLARE_STATIC_KEY_FALSE(mem_qos_anon_key);

struct page *mem_qos_alloc_pages(gfp_t gfp, enum mem_qos_domain domain,
				 bool force, u64 parent);
bool mem_qos_begin(struct mem_qos_call *call, enum mem_qos_domain domain,
		   bool force, u64 parent);
void mem_qos_end(struct mem_qos_call *call, enum mem_qos_path path, bool success);
struct page *mem_qos_slowpath(struct mem_qos_call *call, gfp_t gfp,
			      struct zone *preferred_zone);

#endif
#endif

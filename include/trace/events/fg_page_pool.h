/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fg_page_pool

#if !defined(_TRACE_FG_PAGE_POOL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FG_PAGE_POOL_H

#include <linux/tracepoint.h>

TRACE_EVENT(fg_pool_hit,

	TP_PROTO(int tgid, int nid, unsigned long pfn, unsigned int mode),

	TP_ARGS(tgid, nid, pfn, mode),

	TP_STRUCT__entry(
		__field(int, tgid)
		__field(int, nid)
		__field(unsigned long, pfn)
		__field(unsigned int, mode)
	),

	TP_fast_assign(
		__entry->tgid = tgid;
		__entry->nid = nid;
		__entry->pfn = pfn;
		__entry->mode = mode;
	),

	TP_printk("tgid=%d nid=%d pfn=%#lx mode=%u", __entry->tgid,
		  __entry->nid, __entry->pfn, __entry->mode)
);

#endif

#include <trace/define_trace.h>

/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM anon_page_admission

#if !defined(_TRACE_ANON_PAGE_ADMISSION_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_ANON_PAGE_ADMISSION_H

#include <linux/tracepoint.h>

TRACE_EVENT(anon_page_admission,
	TP_PROTO(u64 id, pid_t tid, u64 role_generation, int node,
		 u8 role, u8 pressure, u8 event),
	TP_ARGS(id, tid, role_generation, node, role, pressure, event),
	TP_STRUCT__entry(
		__field(u64, id)
		__field(pid_t, tid)
		__field(u64, role_generation)
		__field(int, node)
		__field(u8, role)
		__field(u8, pressure)
		__field(u8, event)
	),
	TP_fast_assign(
		__entry->id = id;
		__entry->tid = tid;
		__entry->role_generation = role_generation;
		__entry->node = node;
		__entry->role = role;
		__entry->pressure = pressure;
		__entry->event = event;
	),
	TP_printk("id=%llu tid=%d role_gen=%llu node=%d role=%u pressure=%u event=%u",
		  __entry->id, __entry->tid, __entry->role_generation,
		  __entry->node, __entry->role, __entry->pressure, __entry->event)
);

#endif /* _TRACE_ANON_PAGE_ADMISSION_H */

#include <trace/define_trace.h>

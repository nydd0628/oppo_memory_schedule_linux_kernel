/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mem_alloc_qos

#if !defined(_TRACE_MEM_ALLOC_QOS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MEM_ALLOC_QOS_H

#include <linux/tracepoint.h>

TRACE_EVENT(mem_qos_request,

	TP_PROTO(u64 run, u64 session, u64 id, u64 parent, u64 page,
		 int tid, int role, const char *phase, int detail,
		 u64 ordinal, unsigned int top, unsigned int fgs),

	TP_ARGS(run, session, id, parent, page, tid, role, phase, detail,
		ordinal, top, fgs),

	TP_STRUCT__entry(
		__field(u64, run)
		__field(u64, session)
		__field(u64, id)
		__field(u64, parent)
		__field(u64, page)
		__field(int, tid)
		__field(int, role)
		__string(phase, phase)
		__field(int, detail)
		__field(u64, ordinal)
		__field(unsigned int, top)
		__field(unsigned int, fgs)
	),

	TP_fast_assign(
		__entry->run = run;
		__entry->session = session;
		__entry->id = id;
		__entry->parent = parent;
		__entry->page = page;
		__entry->tid = tid;
		__entry->role = role;
		__assign_str(phase, phase);
		__entry->detail = detail;
		__entry->ordinal = ordinal;
		__entry->top = top;
		__entry->fgs = fgs;
	),

	TP_printk("run=%llu session=%llu id=%llu parent=%llu page=%llu tid=%d role=%d phase=%s detail=%d ordinal=%llu top=%u fgs=%u",
		  __entry->run, __entry->session, __entry->id, __entry->parent,
		  __entry->page, __entry->tid, __entry->role, __get_str(phase),
		  __entry->detail, __entry->ordinal, __entry->top, __entry->fgs)
);

TRACE_EVENT(mem_qos_pool,

	TP_PROTO(u64 run, u64 session, const char *phase, unsigned int f,
		 unsigned int g, unsigned int r, unsigned int count,
		 unsigned int failures, u64 duration_ns),

	TP_ARGS(run, session, phase, f, g, r, count, failures, duration_ns),

	TP_STRUCT__entry(
		__field(u64, run)
		__field(u64, session)
		__string(phase, phase)
		__field(unsigned int, f)
		__field(unsigned int, g)
		__field(unsigned int, r)
		__field(unsigned int, count)
		__field(unsigned int, failures)
		__field(u64, duration_ns)
	),

	TP_fast_assign(
		__entry->run = run;
		__entry->session = session;
		__assign_str(phase, phase);
		__entry->f = f;
		__entry->g = g;
		__entry->r = r;
		__entry->count = count;
		__entry->failures = failures;
		__entry->duration_ns = duration_ns;
	),

	TP_printk("run=%llu session=%llu phase=%s F=%u G=%u R=%u count=%u failures=%u duration_ns=%llu",
		  __entry->run, __entry->session, __get_str(phase), __entry->f,
		  __entry->g, __entry->r, __entry->count, __entry->failures,
		  __entry->duration_ns)
);

#endif /* _TRACE_MEM_ALLOC_QOS_H */

#include <trace/define_trace.h>

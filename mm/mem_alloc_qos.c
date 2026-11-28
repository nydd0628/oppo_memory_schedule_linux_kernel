// SPDX-License-Identifier: GPL-2.0
#include <linux/capability.h>
#include <linux/cpuset.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/pid.h>
#include <linux/refcount.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/workqueue.h>

#include "mem_alloc_qos.h"

#define CREATE_TRACE_POINTS
#include <trace/events/mem_alloc_qos.h>

#define QOS_ROLES	2
#define QOS_MAX_BATCH	16
#define QOS_MAX_GRANTS	8
#define QOS_MAX_PAGES	128
#define QOS_MAX_DEPTH	64

enum qos_mode { QOS_N, QOS_P, QOS_FIFO, QOS_WRR };
enum qos_lifecycle { QOS_OFF, QOS_STARTING, QOS_ACTIVE, QOS_DRAINING };
enum qos_request_state { QOS_QUEUED, QOS_GRANTED, QOS_COMPLETE, QOS_ABORTED };
enum qos_reason {
	QOS_NATIVE,
	QOS_CONTEXT,
	QOS_GFP,
	QOS_ZONE,
	QOS_EMPTY,
	QOS_FULL,
	QOS_NO_DESC,
	QOS_EXPIRED,
	QOS_SIGNAL,
	QOS_STOP,
	QOS_EXPLICIT,
	QOS_PAGE_INVALID,
	QOS_NR_REASONS,
};

struct qos_config {
	u32 mode, domain, top_weight, fgs_weight;
	u32 target, max, low, refill_batch, init_batch;
	u32 backoff_us, failure_limit, depth, budget_us, grant_batch, trace;
	u64 run_id;
	u32 tgid[QOS_ROLES];
};

struct qos_page {
	struct list_head link;
	struct page *page;
	u64 id;
};

struct qos_request {
	struct list_head link;
	struct mem_qos_call *call;
	refcount_t refs;
	wait_queue_head_t wait;
	ktime_t deadline;
	struct qos_page *page;
	u64 enqueue_seq;
	u64 grant_seq;
	enum qos_request_state state;
	enum qos_reason reason;
};

struct qos_counts {
	u64 obtained, delivered, returned, rejected;
	u64 enqueued[QOS_ROLES], granted[QOS_ROLES], claimed[QOS_ROLES];
	u64 revoked[QOS_ROLES], dual[QOS_ROLES], single[QOS_ROLES];
	u64 reason[QOS_NR_REASONS];
	u64 max_head_ns[QOS_ROLES];
	u32 peak, initialized, refill_batches, refill_empty, pauses;
	u64 init_ns;
	int start_error;
};

struct qos_session {
	spinlock_t lock;
	enum qos_lifecycle state;
	struct qos_config cfg;
	struct pid *pids[QOS_ROLES];
	u64 generation, config_generation;
	struct list_head free_pages, unused_pages;
	struct list_head queues[QOS_ROLES], granted, unused_requests;
	struct qos_page *pages;
	struct qos_request *requests;
	u32 f, g, r, retiring, queued[QOS_ROLES], live_requests, cursor;
	u32 failures;
	bool refill_pending, paused;
	u64 page_seq, enqueue_seq, grant_seq;
	atomic64_t call_seq;
	atomic64_t paths[QOS_ROLES][QOS_NR_PATHS][2];
	struct qos_counts counts;
	struct work_struct refill_work;
	struct hrtimer retry_timer;
	wait_queue_head_t test_wait;
	bool test_manual;
	int test_hold_role;
	int test_fail_after;
};

static DEFINE_MUTEX(qos_control_lock);
DEFINE_STATIC_SRCU(qos_srcu);
static struct qos_session __rcu *qos_active;
static struct qos_session q;
static struct qos_config pending;
static struct pid *pending_pids[QOS_ROLES];
static u64 pending_generation;
static const char *qos_config_error;
DEFINE_STATIC_KEY_FALSE(mem_qos_anon_key);

static void qos_request_event(const struct mem_qos_call *call, const char *phase,
			      int detail, u64 page, u64 ordinal, u32 top, u32 fgs)
{
	if (q.cfg.trace == 2)
		trace_mem_qos_request(q.cfg.run_id, q.generation, call->id, call->parent,
			      page, call->tid, call->role, phase, detail,
			      ordinal, top, fgs);
}

static void qos_event_locked(const struct mem_qos_call *call, const char *phase,
			     int detail, u64 page, u64 ordinal)
{
	qos_request_event(call, phase, detail, page, ordinal,
			  q.queued[0], q.queued[1]);
}

static void qos_pool_event(const char *phase, u32 count, u64 duration_ns)
{
	if (q.cfg.trace == 2)
		trace_mem_qos_pool(q.cfg.run_id, q.generation, phase, q.f, q.g, q.r,
			   count, q.failures, duration_ns);
}

bool mem_qos_begin(struct mem_qos_call *call, enum mem_qos_domain domain,
		   bool force, u64 parent)
{
	int index, role;

	if (!in_task() || !rcu_access_pointer(qos_active))
		return false;

	index = srcu_read_lock(&qos_srcu);
	if (!srcu_dereference(qos_active, &qos_srcu) || q.cfg.domain != domain)
		goto out;

	for (role = 0; role < QOS_ROLES; role++)
		if (task_tgid(current) == q.pids[role])
			break;
	if (role == QOS_ROLES)
		goto out;

	call->srcu_index = index;
	call->id = q.cfg.trace == 2 ? atomic64_inc_return(&q.call_seq) : 0;
	call->tid = task_pid_nr(current);
	call->parent = parent;
	call->role = role;
	call->domain = domain;
	call->force = force && domain == QOS_DRIVER && q.cfg.mode != QOS_N;
	qos_request_event(call, "enter", call->force, 0, 0, 0, 0);
	return true;
out:
	srcu_read_unlock(&qos_srcu, index);
	return false;
}

void mem_qos_end(struct mem_qos_call *call, enum mem_qos_path path, bool success)
{
	if (q.cfg.trace)
		atomic64_inc(&q.paths[call->role][path][success]);
	qos_request_event(call, "return", 2 * path + success, 0, 0, 0, 0);
	srcu_read_unlock(&qos_srcu, call->srcu_index);
}

static void qos_request_put_locked(struct qos_request *req)
{
	if (!refcount_dec_and_test(&req->refs))
		return;

	q.live_requests--;
	list_add(&req->link, &q.unused_requests);
}

static void qos_notify(struct qos_request *req)
{
	wake_up(&req->wait);
	if (q.cfg.domain == QOS_DRIVER)
		wake_up_all(&q.test_wait);

	spin_lock(&q.lock);
	qos_request_put_locked(req);
	spin_unlock(&q.lock);
}

static void qos_count_reason(const struct mem_qos_call *call, enum qos_reason why)
{
	q.counts.reason[why]++;
	qos_event_locked(call, "fallback", why, 0, 0);
}

static bool qos_page_valid(struct page *page)
{
	return page_zonenum(page) == ZONE_NORMAL && !page_is_pfmemalloc(page) &&
	       migratetype_is_mergeable(get_pageblock_migratetype(page));
}

static struct page *qos_detach_page(struct qos_page *slot)
{
	struct page *page = slot->page;

	slot->page = NULL;
	list_add(&slot->link, &q.unused_pages);
	return page;
}

static unsigned int qos_occupancy(void)
{
	return q.f + q.g + q.r + q.retiring;
}

static void qos_kick_refill_locked(void)
{
	if (q.state != QOS_ACTIVE || q.cfg.mode == QOS_N || q.paused ||
	    q.refill_pending || q.f >= q.cfg.low || qos_occupancy() >= q.cfg.target)
		return;

	q.refill_pending = true;
	queue_work(system_unbound_wq, &q.refill_work);
}

static void qos_retire_pages(struct page **pages, unsigned int nr)
{
	unsigned int i;

	if (!nr)
		return;
	for (i = 0; i < nr; i++)
		__free_page(pages[i]);
	spin_lock(&q.lock);
	q.retiring -= nr;
	q.counts.returned += nr;
	qos_kick_refill_locked();
	spin_unlock(&q.lock);
}

static void qos_record_head_age(const struct qos_request *req, ktime_t now)
{
	int role = req->call->role;
	u64 age = ktime_to_ns(ktime_sub(now,
			   ktime_sub_us(req->deadline, q.cfg.budget_us)));

	q.counts.max_head_ns[role] = max(q.counts.max_head_ns[role], age);
}

/* Cancel leaves the list reference with the notifier until after wake_up(). */
static void qos_cancel_locked(struct qos_request *req, enum qos_reason reason)
{
	struct qos_page *slot = req->page;

	if (!slot && q.queues[req->call->role].next == &req->link)
		qos_record_head_age(req, ktime_get());
	list_del_init(&req->link);
	if (slot) {
		q.g--;
		q.f++;
		list_add(&slot->link, &q.free_pages);
		req->page = NULL;
		q.counts.revoked[req->call->role]++;
	} else {
		q.queued[req->call->role]--;
	}
	req->reason = reason;
	q.counts.reason[reason]++;
	qos_event_locked(req->call, slot ? "revoke" : "cancel", reason,
			 slot ? slot->id : 0, req->grant_seq);
	WRITE_ONCE(req->state, QOS_ABORTED);
}

static struct qos_request *qos_head(int role)
{
	return list_first_entry_or_null(&q.queues[role], struct qos_request, link);
}

/* Expire both heads before selecting a slot. Live heads are ordered by enqueue time. */
static struct qos_request *qos_pick_locked(ktime_t now, bool *dual)
{
	struct qos_request *top = qos_head(0);
	struct qos_request *fgs = qos_head(1);
	struct qos_request *req;

	if (top && !ktime_before(now, top->deadline))
		return top;
	if (fgs && !ktime_before(now, fgs->deadline))
		return fgs;
	if (!top || !fgs)
		return top ?: fgs;

	*dual = true;
	if (q.cfg.mode == QOS_FIFO)
		req = top->enqueue_seq < fgs->enqueue_seq ? top : fgs;
	else
		req = q.cursor < q.cfg.top_weight ? top : fgs;

	return req;
}

/* Each pass bounds the work under q.lock, including expiries and rejected pages. */
static bool qos_dispatch_batch(bool manual)
{
	struct qos_request *notify[QOS_MAX_GRANTS];
	struct page *discard[QOS_MAX_GRANTS];
	unsigned int i, nr_notify = 0, nr_discard = 0;
	bool more;

	spin_lock(&q.lock);
	if (q.state != QOS_ACTIVE || q.test_manual != manual)
		goto idle;

	for (i = 0; i < q.cfg.grant_batch; i++) {
		struct qos_request *req;
		struct qos_page *slot;
		ktime_t now = ktime_get();
		bool dual = false;
		int role;

		req = qos_pick_locked(now, &dual);
		if (!req)
			break;
		if (!ktime_before(now, req->deadline)) {
			qos_cancel_locked(req, QOS_EXPIRED);
			notify[nr_notify++] = req;
			continue;
		}
		slot = list_first_entry_or_null(&q.free_pages, struct qos_page, link);
		if (!slot)
			break;

		if (!qos_page_valid(slot->page)) {
			list_del(&slot->link);
			q.f--;
			q.retiring++;
			discard[nr_discard++] = qos_detach_page(slot);
			q.counts.rejected++;
			continue;
		}
		now = ktime_get();
		if (!ktime_before(now, req->deadline)) {
			qos_cancel_locked(req, QOS_EXPIRED);
			notify[nr_notify++] = req;
			continue;
		}

		list_del(&slot->link);
		q.f--;
		role = req->call->role;
		qos_record_head_age(req, now);
		q.queued[role]--;
		q.g++;
		req->page = slot;
		req->grant_seq = ++q.grant_seq;
		list_move_tail(&req->link, &q.granted);

		q.counts.granted[role]++;
		if (dual) {
			q.counts.dual[role]++;
			if (q.cfg.mode == QOS_WRR) {
				q.cursor++;
				if (q.cursor == q.cfg.top_weight + q.cfg.fgs_weight)
					q.cursor = 0;
			}
		} else {
			q.counts.single[role]++;
		}
		/* The granted list and this notification each own a reference. */
		refcount_inc(&req->refs);
		WRITE_ONCE(req->state, QOS_GRANTED);
		notify[nr_notify++] = req;
		qos_event_locked(req->call, "grant", dual, slot->id, req->grant_seq);
	}
	more = i == q.cfg.grant_batch && (q.queued[0] || q.queued[1]) && q.f;
	qos_kick_refill_locked();
	spin_unlock(&q.lock);

	for (i = 0; i < nr_notify; i++)
		qos_notify(notify[i]);
	qos_retire_pages(discard, nr_discard);
	return more;

idle:
	spin_unlock(&q.lock);
	return false;
}

static void qos_dispatch(void)
{
	while (qos_dispatch_batch(false))
		cond_resched();
}

static void qos_drop_list_ref_locked(struct qos_request *req)
{
	list_del_init(&req->link);
	qos_request_put_locked(req);
}

static struct page *qos_wait(struct qos_request *req)
{
	struct qos_page *slot;
	struct page *page = NULL;
	ktime_t remaining = ktime_sub(req->deadline, ktime_get());
	enum qos_reason reason = QOS_EXPIRED;
	bool driver = req->call->domain == QOS_DRIVER;
	int wait_ret = 0;

	if (ktime_to_ns(remaining) > 0)
		wait_ret = wait_event_interruptible_hrtimeout(req->wait,
				READ_ONCE(req->state) != QOS_QUEUED, remaining);
	if (req->call->force && READ_ONCE(req->state) == QOS_GRANTED)
		wait_event(q.test_wait, READ_ONCE(req->state) != QOS_GRANTED ||
			   READ_ONCE(q.test_hold_role) != req->call->role ||
			   READ_ONCE(q.state) != QOS_ACTIVE);
	qos_request_event(req->call, "resume", wait_ret, 0, 0, 0, 0);

	spin_lock(&q.lock);
	if (req->state == QOS_QUEUED ||
	    (req->state == QOS_GRANTED && q.state != QOS_ACTIVE)) {
		if (q.state != QOS_ACTIVE)
			reason = QOS_STOP;
		else if (wait_ret == -ERESTARTSYS)
			reason = QOS_SIGNAL;
		qos_cancel_locked(req, reason);
		/* This thread performed the cancellation and needs no wakeup. */
		qos_request_put_locked(req);
	}

	if (req->state == QOS_GRANTED) {
		slot = req->page;
		req->page = NULL;
		q.g--;
		qos_drop_list_ref_locked(req);
		page = qos_detach_page(slot);
		if (qos_page_valid(page)) {
			q.counts.delivered++;
			q.counts.claimed[req->call->role]++;
			qos_event_locked(req->call, "claim", 0, slot->id, req->grant_seq);
			WRITE_ONCE(req->state, QOS_COMPLETE);
		} else {
			q.retiring++;
			q.counts.rejected++;
			q.counts.revoked[req->call->role]++;
			q.counts.reason[QOS_PAGE_INVALID]++;
			qos_event_locked(req->call, "revoke", QOS_PAGE_INVALID,
					 slot->id, req->grant_seq);
			WRITE_ONCE(req->state, QOS_ABORTED);
			reason = QOS_PAGE_INVALID;
		}
	} else {
		reason = req->reason;
	}
	qos_request_put_locked(req);
	qos_kick_refill_locked();
	spin_unlock(&q.lock);

	if (page && reason == QOS_PAGE_INVALID) {
		qos_retire_pages(&page, 1);
		return NULL;
	}
	if (reason == QOS_EXPLICIT && driver)
		return ERR_PTR(-ECANCELED);
	return page;
}

struct page *mem_qos_slowpath(struct mem_qos_call *call, gfp_t gfp,
			      struct zone *preferred_zone)
{
	struct qos_request *req;
	struct qos_page *slot;
	struct page *page;
	enum qos_reason reason;
	gfp_t expected = call->domain == QOS_DRIVER ? QOS_DRIVER_GFP : QOS_ANON_GFP;

	if (!call->id && q.cfg.mode != QOS_N)
		call->id = atomic64_inc_return(&q.call_seq);
	spin_lock(&q.lock);
	if (q.state != QOS_ACTIVE) {
		reason = QOS_STOP;
		goto fallback;
	}
	if (q.cfg.mode == QOS_N) {
		reason = QOS_NATIVE;
		goto fallback;
	}
	if (!(gfp & __GFP_DIRECT_RECLAIM) ||
	    (current->flags & (PF_MEMALLOC | PF_MEMALLOC_NOFS | PF_MEMALLOC_NOIO |
			       PF_MEMALLOC_PIN | PF_EXITING)) ||
	    tsk_is_oom_victim(current)) {
		reason = QOS_CONTEXT;
		goto fallback;
	}
	if (gfp != expected) {
		reason = QOS_GFP;
		goto fallback;
	}
	if (!preferred_zone || !cpuset_zone_allowed(&NODE_DATA(0)->node_zones[ZONE_NORMAL],
						    gfp | __GFP_HARDWALL)) {
		reason = QOS_ZONE;
		goto fallback;
	}

	if (q.cfg.mode == QOS_P) {
		slot = list_first_entry_or_null(&q.free_pages, struct qos_page, link);
		if (!slot) {
			reason = QOS_EMPTY;
			goto fallback;
		}

		list_del(&slot->link);
		q.f--;
		page = qos_detach_page(slot);
		if (qos_page_valid(page)) {
			q.counts.delivered++;
			q.counts.claimed[call->role]++;
			qos_event_locked(call, "take", 0, slot->id, 0);
			qos_kick_refill_locked();
			spin_unlock(&q.lock);
			return page;
		}
		q.retiring++;
		q.counts.rejected++;
		qos_count_reason(call, QOS_PAGE_INVALID);
		spin_unlock(&q.lock);
		qos_retire_pages(&page, 1);
		return NULL;
	}

	if (q.queued[call->role] == q.cfg.depth) {
		reason = QOS_FULL;
		goto fallback;
	}
	req = list_first_entry_or_null(&q.unused_requests, struct qos_request, link);
	if (!req) {
		reason = QOS_NO_DESC;
		goto fallback;
	}
	q.live_requests++;
	refcount_set(&req->refs, 2); /* Caller and queue. */
	req->call = call;
	req->page = NULL;
	req->grant_seq = 0;
	req->enqueue_seq = ++q.enqueue_seq;
	req->deadline = ktime_add_us(ktime_get(), q.cfg.budget_us);
	WRITE_ONCE(req->state, QOS_QUEUED);
	list_move_tail(&req->link, &q.queues[call->role]);
	q.queued[call->role]++;
	q.counts.enqueued[call->role]++;
	qos_event_locked(call, "enqueue", 0, 0, req->enqueue_seq);
	qos_kick_refill_locked();
	spin_unlock(&q.lock);

	qos_dispatch();
	return qos_wait(req);

fallback:
	qos_count_reason(call, reason);
	qos_kick_refill_locked();
	spin_unlock(&q.lock);
	return NULL;
}

static bool qos_inject_failure(void)
{
	bool fail = false;

	spin_lock(&q.lock);
	if (q.cfg.domain == QOS_DRIVER && q.test_fail_after >= 0) {
		fail = !q.test_fail_after;
		if (!fail)
			WRITE_ONCE(q.test_fail_after, q.test_fail_after - 1);
	}
	spin_unlock(&q.lock);
	return fail;
}

/*
 * The normal allocator returns fully prepared pages. Preserve the domain's
 * placement, zeroing and KASAN flags, but never use CMA, reclaim or reserves.
 */
static bool qos_fill_batch(bool init)
{
	struct page *pages[QOS_MAX_BATCH];
	unsigned int i, nr, allocated, got = 0, nr_discard = 0;
	u64 started;
	gfp_t gfp;

	spin_lock(&q.lock);
	if (q.state != (init ? QOS_STARTING : QOS_ACTIVE) ||
	    qos_occupancy() >= q.cfg.target) {
		spin_unlock(&q.lock);
		return false;
	}
	nr = min(init ? q.cfg.init_batch : q.cfg.refill_batch,
		 min(q.cfg.target, q.cfg.max) - qos_occupancy());
	q.r += nr;
	q.counts.peak = max(q.counts.peak, qos_occupancy());
	qos_pool_event(init ? "init_reserve" : "refill_reserve", nr, 0);
	spin_unlock(&q.lock);

	started = ktime_get_ns();
	gfp = q.cfg.domain == QOS_DRIVER ? QOS_DRIVER_GFP : QOS_ANON_GFP;
	gfp &= ~(__GFP_RECLAIM | __GFP_CMA);
	gfp |= __GFP_NOMEMALLOC | __GFP_NOWARN;
	for (i = 0; i < nr; i++) {
		if (qos_inject_failure())
			break;
		pages[i] = alloc_pages_node(0, gfp, 0);
		if (!pages[i])
			break;
	}

	spin_lock(&q.lock);
	allocated = i;
	q.r -= nr;
	q.counts.obtained += allocated;
	for (i = 0; i < allocated; i++) {
		struct qos_page *slot;
		struct page *page = pages[i];

		if (q.state != (init ? QOS_STARTING : QOS_ACTIVE) ||
		    !qos_page_valid(page)) {
			q.retiring++;
			q.counts.rejected++;
			pages[nr_discard++] = page;
			continue;
		}

		slot = list_first_entry(&q.unused_pages, struct qos_page, link);
		slot->page = page;
		slot->id = ++q.page_seq;
		list_move_tail(&slot->link, &q.free_pages);
		q.f++;
		got++;
	}
	if (init) {
		q.counts.initialized += got;
	} else {
		q.counts.refill_batches++;
		if (got)
			q.failures = 0;
		else {
			q.counts.refill_empty++;
			if (++q.failures >= q.cfg.failure_limit) {
				q.paused = true;
				q.counts.pauses++;
			}
		}
	}
	qos_pool_event(init ? "init_batch" : "refill_batch", got,
		       ktime_get_ns() - started);
	spin_unlock(&q.lock);
	qos_retire_pages(pages, nr_discard);

	return init ? got == nr : got != 0;
}

static void qos_refill_work(struct work_struct *work)
{
	bool filled;

	do {
		filled = qos_fill_batch(false);
		qos_dispatch();
		cond_resched();

		spin_lock(&q.lock);
		if (q.state != QOS_ACTIVE || q.paused ||
		    qos_occupancy() >= q.cfg.target)
			break;
		if (!filled) {
			hrtimer_start(&q.retry_timer, ns_to_ktime(q.cfg.backoff_us * NSEC_PER_USEC),
				      HRTIMER_MODE_REL);
			spin_unlock(&q.lock);
			return;
		}
		spin_unlock(&q.lock);
	} while (true);

	q.refill_pending = false;
	spin_unlock(&q.lock);
}

static enum hrtimer_restart qos_retry(struct hrtimer *timer)
{
	queue_work(system_unbound_wq, &q.refill_work);
	return HRTIMER_NORESTART;
}

static void qos_free_storage(void)
{
	struct qos_page *slot;

	while (!list_empty(&q.free_pages)) {
		slot = list_first_entry(&q.free_pages, struct qos_page, link);
		list_del(&slot->link);
		q.f--;
		__free_page(slot->page);
		q.counts.returned++;
	}
	kfree(q.requests);
	kfree(q.pages);
	q.requests = NULL;
	q.pages = NULL;
}

static bool qos_pid_live(struct pid *pid)
{
	struct task_struct *task = get_pid_task(pid, PIDTYPE_TGID);

	if (!task)
		return false;
	put_task_struct(task);
	return true;
}

static int qos_start(void)
{
	u64 started = ktime_get_ns();
	unsigned int role, i, nr_desc;
	int ret = -ENOMEM;

	if (q.state != QOS_OFF)
		return -EBUSY;
	for (role = 0; role < QOS_ROLES; role++)
		if (!pending_pids[role] || !qos_pid_live(pending_pids[role]))
			return -ESRCH;

	q.cfg = pending;
	if (q.cfg.domain == QOS_ANON) {
		q.test_manual = false;
		q.test_hold_role = -1;
	}
	q.config_generation = pending_generation;
	q.generation++;
	q.state = QOS_STARTING;
	q.cursor = 0;
	q.f = q.g = q.r = q.retiring = q.live_requests = 0;
	q.failures = 0;
	q.paused = q.refill_pending = false;
	q.page_seq = q.enqueue_seq = q.grant_seq = 0;
	memset(&q.counts, 0, sizeof(q.counts));
	memset(q.paths, 0, sizeof(q.paths));
	atomic64_set(&q.call_seq, 0);

	INIT_LIST_HEAD(&q.free_pages);
	INIT_LIST_HEAD(&q.unused_pages);
	INIT_LIST_HEAD(&q.granted);
	INIT_LIST_HEAD(&q.unused_requests);
	for (role = 0; role < QOS_ROLES; role++) {
		q.queued[role] = 0;
		INIT_LIST_HEAD(&q.queues[role]);
	}

	if (q.cfg.mode != QOS_N) {
		q.pages = kcalloc(q.cfg.max, sizeof(*q.pages), GFP_KERNEL);
		if (!q.pages)
			goto failed;
		for (i = 0; i < q.cfg.max; i++)
			list_add(&q.pages[i].link, &q.unused_pages);

		if (q.cfg.mode >= QOS_FIFO) {
			nr_desc = QOS_ROLES * q.cfg.depth + q.cfg.max;
			q.requests = kcalloc(nr_desc, sizeof(*q.requests), GFP_KERNEL);
			if (!q.requests)
				goto failed;
			for (i = 0; i < nr_desc; i++) {
				init_waitqueue_head(&q.requests[i].wait);
				list_add(&q.requests[i].link, &q.unused_requests);
			}
		}
		while (q.f < q.cfg.target) {
			if (!qos_fill_batch(true))
				goto failed;
			cond_resched();
		}
	}

	for (role = 0; role < QOS_ROLES; role++)
		q.pids[role] = get_pid(pending_pids[role]);

	q.counts.init_ns = ktime_get_ns() - started;
	q.state = QOS_ACTIVE;
	qos_pool_event("start", q.f, q.counts.init_ns);
	rcu_assign_pointer(qos_active, &q);
	if (q.cfg.domain == QOS_ANON)
		static_branch_enable(&mem_qos_anon_key);
	return 0;

failed:
	qos_free_storage();
	q.state = QOS_OFF;
	q.counts.init_ns = ktime_get_ns() - started;
	q.counts.start_error = ret;
	qos_pool_event("start_failed", q.counts.initialized, q.counts.init_ns);
	return ret;
}

static void qos_drain(void)
{
	struct qos_request *req;

	for (;;) {
		spin_lock(&q.lock);
		req = qos_head(0) ?: qos_head(1);
		if (!req)
			req = list_first_entry_or_null(&q.granted, struct qos_request, link);
		if (req)
			qos_cancel_locked(req, QOS_STOP);
		spin_unlock(&q.lock);
		if (!req)
			break;
		qos_notify(req);
		cond_resched();
	}
}

static void qos_stop(void)
{
	unsigned int role;

	if (q.state == QOS_OFF)
		return;

	spin_lock(&q.lock);
	WRITE_ONCE(q.state, QOS_DRAINING);
	rcu_assign_pointer(qos_active, NULL);
	spin_unlock(&q.lock);
	if (q.cfg.domain == QOS_ANON)
		static_branch_disable(&mem_qos_anon_key);
	hrtimer_cancel(&q.retry_timer);
	cancel_work_sync(&q.refill_work);
	qos_drain();
	synchronize_srcu(&qos_srcu);

	WARN_ON_ONCE(q.g || q.r || q.retiring || q.live_requests || q.queued[0] || q.queued[1]);
	qos_free_storage();
	WARN_ON_ONCE(q.counts.obtained != q.counts.delivered + q.counts.returned);
	for (role = 0; role < QOS_ROLES; role++) {
		put_pid(q.pids[role]);
		q.pids[role] = NULL;
	}

	q.refill_pending = q.paused = false;
	q.test_manual = false;
	q.test_hold_role = -1;
	q.state = QOS_OFF;
	qos_pool_event("stop", 0, 0);
}

static const char * const qos_modes[] = { "N", "P", "F", "W" };
static const char * const qos_domains[] = { "driver", "anon" };
static const char * const qos_states[] = { "OFF", "STARTING", "ACTIVE", "DRAINING" };
static const char * const qos_paths[] = { "fast", "slow", "pool", "rejected", "cancelled" };
static const char * const qos_reasons[] = {
	"native", "context", "gfp", "zone", "empty", "full", "no_desc", "expired",
	"signal", "stop", "explicit", "page_invalid",
};

static void qos_print_config(struct seq_file *m, const char *prefix,
			     const struct qos_config *cfg, u64 generation)
{
	seq_printf(m, "%s_config mode=%s domain=%s weights=%u/%u target=%u max=%u low=%u refill_batch=%u init_batch=%u backoff_us=%u failure_limit=%u depth=%u budget_us=%u grant_batch=%u trace=%u run_id=%llu tgid=%u/%u config_generation=%llu\n",
		   prefix, qos_modes[cfg->mode], qos_domains[cfg->domain], cfg->top_weight,
		   cfg->fgs_weight, cfg->target, cfg->max, cfg->low, cfg->refill_batch,
		   cfg->init_batch, cfg->backoff_us, cfg->failure_limit, cfg->depth,
		   cfg->budget_us, cfg->grant_batch, cfg->trace, cfg->run_id,
		   cfg->tgid[0], cfg->tgid[1], generation);
}

static int qos_status_show(struct seq_file *m, void *unused)
{
	struct qos_counts counts;
	u32 f, g, r, retiring, queued[QOS_ROLES], live, cursor, failures;
	bool paused, refill_pending;
	unsigned int role, i;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	mutex_lock(&qos_control_lock);
	spin_lock(&q.lock);
	counts = q.counts;
	f = q.f;
	g = q.g;
	r = q.r;
	retiring = q.retiring;
	queued[0] = q.queued[0];
	queued[1] = q.queued[1];
	live = q.live_requests;
	cursor = q.cursor;
	failures = q.failures;
	paused = q.paused;
	refill_pending = q.refill_pending;
	spin_unlock(&q.lock);

	seq_printf(m, "version=%s page_size=%lu state=%s session=%llu\n",
		   init_uts_ns.name.release, PAGE_SIZE, qos_states[q.state], q.generation);
	qos_print_config(m, "staged", &pending, pending_generation);
	seq_printf(m, "config_error=%s\n", qos_config_error ?: "none");
	qos_print_config(m, "session", &q.cfg, q.config_generation);
	seq_printf(m, "pool F=%u G=%u R=%u retiring=%u peak=%u obtained=%llu delivered=%llu returned=%llu rejected=%llu balanced=%u\n",
		   f, g, r, retiring, counts.peak, counts.obtained, counts.delivered,
		   counts.returned, counts.rejected,
		   counts.obtained == f + g + retiring + counts.delivered + counts.returned);
	seq_printf(m, "init initialized=%u init_ns=%llu start_error=%d\n",
		   counts.initialized, counts.init_ns, counts.start_error);
	seq_printf(m, "refill batches=%u empty=%u failures=%u pauses=%u paused=%u pending=%u\n",
		   counts.refill_batches, counts.refill_empty, failures,
		   counts.pauses, paused, refill_pending);
	seq_printf(m, "queue top=%u fgs=%u live=%u cursor=%u\n",
		   queued[0], queued[1], live, cursor);
	seq_printf(m, "test manual=%u hold_role=%d fail_after=%d\n",
		   q.test_manual, q.test_hold_role, READ_ONCE(q.test_fail_after));
	for (role = 0; role < QOS_ROLES; role++) {
		seq_printf(m, "role=%u enqueued=%llu grants=%llu claimed=%llu revoked=%llu dual=%llu single=%llu max_head_ns=%llu",
			   role, counts.enqueued[role], counts.granted[role],
			   counts.claimed[role], counts.revoked[role], counts.dual[role],
			   counts.single[role], counts.max_head_ns[role]);
		for (i = 0; i < QOS_NR_PATHS; i++)
			seq_printf(m, " %s_ok=%lld %s_fail=%lld", qos_paths[i],
				   atomic64_read(&q.paths[role][i][1]), qos_paths[i],
				   atomic64_read(&q.paths[role][i][0]));
		seq_putc(m, '\n');
	}
	seq_puts(m, "reasons");
	for (i = 0; i < QOS_NR_REASONS; i++)
		seq_printf(m, " %s=%llu", qos_reasons[i], counts.reason[i]);
	seq_putc(m, '\n');
	mutex_unlock(&qos_control_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qos_status);

static const char *qos_invalid_config(const struct qos_config *cfg)
{
	if (cfg->mode >= ARRAY_SIZE(qos_modes))
		return "mode";
	if (cfg->domain >= ARRAY_SIZE(qos_domains))
		return "domain";
	if (cfg->trace > 2)
		return "trace";
	if (!cfg->run_id)
		return "run_id";
	if (!cfg->tgid[0] || !cfg->tgid[1] ||
	    cfg->tgid[0] > PID_MAX_LIMIT || cfg->tgid[1] > PID_MAX_LIMIT ||
	    cfg->tgid[0] == cfg->tgid[1])
		return "role_tgids";

	if (cfg->mode == QOS_WRR) {
		if (!cfg->top_weight || cfg->top_weight > 4 || cfg->fgs_weight != 1)
			return "weights";
	} else if (cfg->top_weight || cfg->fgs_weight) {
		return "weights";
	}

	if (cfg->mode == QOS_N) {
		if (cfg->target || cfg->max || cfg->low || cfg->refill_batch ||
		    cfg->init_batch || cfg->backoff_us || cfg->failure_limit ||
		    cfg->depth || cfg->budget_us || cfg->grant_batch)
			return "native_mode_options";
		return NULL;
	}

	if (!cfg->target || cfg->target > cfg->max || cfg->max > QOS_MAX_PAGES)
		return "target_or_max";
	if (!cfg->low || cfg->low >= cfg->target)
		return "low";
	if (!cfg->refill_batch || cfg->refill_batch > QOS_MAX_BATCH)
		return "refill_batch";
	if (!cfg->init_batch || cfg->init_batch > QOS_MAX_BATCH)
		return "init_batch";
	if (cfg->backoff_us < 500 || cfg->backoff_us > 8000)
		return "backoff_us";
	if (!cfg->failure_limit || cfg->failure_limit > 8)
		return "failure_limit";

	if (cfg->mode == QOS_P) {
		if (cfg->depth || cfg->budget_us || cfg->grant_batch)
			return "pool_only_queue_options";
		return NULL;
	}

	if (!cfg->depth || cfg->depth > QOS_MAX_DEPTH)
		return "depth";
	if (cfg->budget_us < 100 ||
	    cfg->budget_us > (cfg->domain == QOS_DRIVER ? 1000000 : 1000))
		return "budget_us";
	if (!cfg->grant_batch || cfg->grant_batch > QOS_MAX_GRANTS)
		return "grant_batch";
	return NULL;
}

static int qos_configure(char **argv, int argc)
{
	struct qos_config cfg;
	const struct {
		const char *name;
		u32 *value;
	} fields[] = {
		{ "mode", &cfg.mode }, { "domain", &cfg.domain },
		{ "weight_top", &cfg.top_weight }, { "weight_fgs", &cfg.fgs_weight },
		{ "target", &cfg.target }, { "max", &cfg.max }, { "low", &cfg.low },
		{ "refill_batch", &cfg.refill_batch }, { "init_batch", &cfg.init_batch },
		{ "backoff_us", &cfg.backoff_us }, { "failure_limit", &cfg.failure_limit },
		{ "depth", &cfg.depth }, { "budget_us", &cfg.budget_us },
		{ "grant_batch", &cfg.grant_batch }, { "trace", &cfg.trace },
	};
	struct pid *pids[QOS_ROLES] = {};
	unsigned int i;

	if (q.state != QOS_OFF) {
		qos_config_error = "busy";
		return -EBUSY;
	}
	qos_config_error = "argument_count";
	if (argc != ARRAY_SIZE(fields) + 3)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		if (kstrtou32(argv[i], 10, fields[i].value)) {
			qos_config_error = fields[i].name;
			return -EINVAL;
		}
	}
	qos_config_error = "run_id";
	if (kstrtou64(argv[i++], 10, &cfg.run_id))
		return -EINVAL;
	qos_config_error = "role_tgids";
	if (kstrtou32(argv[i++], 10, &cfg.tgid[0]) ||
	    kstrtou32(argv[i], 10, &cfg.tgid[1]))
		return -EINVAL;
	qos_config_error = qos_invalid_config(&cfg);
	if (qos_config_error)
		return -EINVAL;

	for (i = 0; i < QOS_ROLES; i++) {
		pids[i] = find_get_pid(cfg.tgid[i]);
		if (!pids[i] || !qos_pid_live(pids[i])) {
			qos_config_error = i ? "fgs_tgid" : "top_tgid";
			goto out;
		}
	}
	for (i = 0; i < QOS_ROLES; i++) {
		cfg.tgid[i] = pid_nr(pids[i]);
		put_pid(pending_pids[i]);
		pending_pids[i] = pids[i];
	}
	pending = cfg;
	pending_generation++;
	return 0;

out:
	for (i = 0; i < QOS_ROLES; i++)
		put_pid(pids[i]);
	return -ESRCH;
}

static int qos_recover(void)
{
	int ret = 0;

	spin_lock(&q.lock);
	if (q.state != QOS_ACTIVE || q.cfg.mode == QOS_N) {
		ret = -EINVAL;
	} else {
		q.paused = false;
		q.failures = 0;
		qos_pool_event("resume", 0, 0);
		qos_kick_refill_locked();
	}
	spin_unlock(&q.lock);
	return ret;
}

static struct qos_request *qos_find_request(struct list_head *head, u64 id)
{
	struct qos_request *req;

	list_for_each_entry(req, head, link)
		if (req->call->id == id)
			return req;
	return NULL;
}

static int qos_cancel(char **argv)
{
	struct qos_request *req;
	u64 generation, id;

	if (kstrtou64(argv[0], 10, &generation) || kstrtou64(argv[1], 10, &id))
		return -EINVAL;
	if (q.state != QOS_ACTIVE || q.generation != generation)
		return -ESTALE;

	spin_lock(&q.lock);
	req = qos_find_request(&q.queues[0], id) ?:
	      qos_find_request(&q.queues[1], id) ?:
	      qos_find_request(&q.granted, id);
	if (req)
		qos_cancel_locked(req, QOS_EXPLICIT);
	spin_unlock(&q.lock);
	if (!req)
		return -ENOENT;

	qos_notify(req);
	qos_dispatch();
	return 0;
}

/* Fault injection and manual service are confined to the driver domain. */
static int qos_test_control(char **argv, int argc)
{
	const char *op = argv[0];
	int val;

	if ((q.state == QOS_OFF ? pending.domain : q.cfg.domain) != QOS_DRIVER)
		return -EOPNOTSUPP;
	if (argc == 1 && !strcmp(op, "step") && q.test_manual) {
		qos_dispatch_batch(true);
		return 0;
	}
	if (argc != 2 || kstrtoint(argv[1], 10, &val))
		return -EINVAL;

	spin_lock(&q.lock);
	if (!strcmp(op, "manual") && (val == 0 || val == 1)) {
		q.test_manual = val;
	} else if (!strcmp(op, "hold") && val >= -1 && val < QOS_ROLES) {
		WRITE_ONCE(q.test_hold_role, val);
	} else if (!strcmp(op, "fail_after") && val >= -1 && val <= QOS_MAX_PAGES) {
		q.test_fail_after = val;
	} else {
		spin_unlock(&q.lock);
		return -EINVAL;
	}
	spin_unlock(&q.lock);
	wake_up_all(&q.test_wait);
	if (!q.test_manual)
		qos_dispatch();
	return 0;
}

static char **qos_split_user(const char __user *buf, size_t len, int *argc)
{
	char *str, **argv;

	if (!len || len > 256)
		return ERR_PTR(-EINVAL);
	str = memdup_user_nul(buf, len);
	if (IS_ERR(str))
		return ERR_CAST(str);
	if (memchr(str, '\0', len)) {
		kfree(str);
		return ERR_PTR(-EINVAL);
	}
	argv = argv_split(GFP_KERNEL, str, argc);
	kfree(str);
	return argv ?: ERR_PTR(-ENOMEM);
}

static ssize_t qos_control_write(struct file *file, const char __user *buf,
				 size_t len, loff_t *ppos)
{
	char **argv;
	int argc, ret = -EINVAL;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	argv = qos_split_user(buf, len, &argc);
	if (IS_ERR(argv))
		return PTR_ERR(argv);

	mutex_lock(&qos_control_lock);
	if (argc) {
		if (!strcmp(argv[0], "config"))
			ret = qos_configure(argv + 1, argc - 1);
		else if (argc == 1 && !strcmp(argv[0], "start"))
			ret = qos_start();
		else if (argc == 1 && !strcmp(argv[0], "stop")) {
			qos_stop();
			ret = 0;
		} else if (argc == 1 && !strcmp(argv[0], "recover"))
			ret = qos_recover();
		else if (argc == 3 && !strcmp(argv[0], "cancel"))
			ret = qos_cancel(argv + 1);
		else if (argc >= 2 && !strcmp(argv[0], "test"))
			ret = qos_test_control(argv + 1, argc - 1);
	}
	mutex_unlock(&qos_control_lock);
	argv_free(argv);
	return ret ? ret : len;
}

static ssize_t qos_alloc_write(struct file *file, const char __user *buf,
			       size_t len, loff_t *ppos)
{
	struct page *page;
	char **argv;
	bool force;
	u64 parent;
	u32 hold_us;
	int argc, ret = -EINVAL;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	argv = qos_split_user(buf, len, &argc);
	if (IS_ERR(argv))
		return PTR_ERR(argv);

	if (argc != 3 || kstrtou64(argv[1], 10, &parent) ||
	    kstrtou32(argv[2], 10, &hold_us) || hold_us > USEC_PER_SEC)
		goto out;

	force = !strcmp(argv[0], "force");
	if (!force && strcmp(argv[0], "auto"))
		goto out;
	page = mem_qos_alloc_pages(QOS_DRIVER_GFP, QOS_DRIVER, force, parent);
	if (IS_ERR_OR_NULL(page)) {
		ret = page ? PTR_ERR(page) : -ENOMEM;
		goto out;
	}

	if (memchr_inv(page_address(page), 0, PAGE_SIZE)) {
		ret = -EUCLEAN;
	} else {
		memset(page_address(page), 0x5a, PAGE_SIZE);
		if (hold_us)
			fsleep(hold_us);
		ret = 0;
	}
	__free_page(page);
out:
	argv_free(argv);
	return ret ? ret : len;
}

static const struct file_operations qos_control_fops = {
	.write = qos_control_write,
	.llseek = no_llseek,
};

static const struct file_operations qos_alloc_fops = {
	.write = qos_alloc_write,
	.llseek = no_llseek,
};

static int __init qos_init(void)
{
	struct dentry *dir;

	spin_lock_init(&q.lock);
	init_waitqueue_head(&q.test_wait);
	INIT_WORK(&q.refill_work, qos_refill_work);
	hrtimer_init(&q.retry_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	q.retry_timer.function = qos_retry;
	q.test_hold_role = q.test_fail_after = -1;

	dir = debugfs_create_dir("mem_alloc_qos", NULL);
	if (IS_ERR(dir))
		return PTR_ERR(dir);
	debugfs_create_file("status", 0400, dir, NULL, &qos_status_fops);
	debugfs_create_file("control", 0200, dir, NULL, &qos_control_fops);
	debugfs_create_file("alloc", 0200, dir, NULL, &qos_alloc_fops);
	return 0;
}
late_initcall(qos_init);

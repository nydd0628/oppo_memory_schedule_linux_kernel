// SPDX-License-Identifier: GPL-2.0-only
#include <linux/anon_page_admission.h>
#include <linux/capability.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/hashtable.h>
#include <linux/hrtimer.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/percpu.h>
#include <linux/pid.h>
#include <linux/psi.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/rt.h>
#include <linux/sched/signal.h>
#include <linux/sched/wake_q.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/userfaultfd_k.h>
#include <linux/vmstat.h>
#include <linux/workqueue.h>

#define CREATE_TRACE_POINTS
#include <trace/events/anon_page_admission.h>

#define ADMISSION_REQUESTS	256
#define ADMISSION_ROLES		1024
#define ADMISSION_MAX_WEIGHT	16
#define ADMISSION_MAX_INFLIGHT	64
#define ADMISSION_MAX_LEASE_MS	60000
#define ADMISSION_HIST_BUCKETS	5

enum admission_role {
	ADMISSION_UNCLASSIFIED,
	ADMISSION_TOP,
	ADMISSION_FGS,
	ADMISSION_ROLE_COUNT,
};

enum admission_pressure {
	ADMISSION_NORMAL,
	ADMISSION_WARNING,
	ADMISSION_TIGHT,
	ADMISSION_RECOVERY,
	ADMISSION_EMERGENCY,
	ADMISSION_PRESSURE_COUNT,
};

enum admission_state {
	ADMISSION_WAITING,
	ADMISSION_GRANTED,
	ADMISSION_IN_ALLOC,
	ADMISSION_IN_BYPASS,
	ADMISSION_DONE,
	ADMISSION_BYPASS,
};

enum admission_event {
	ADMISSION_ENQUEUE,
	ADMISSION_GRANT,
	ADMISSION_AGED_GRANT,
	ADMISSION_TIMEOUT,
	ADMISSION_CANCEL,
	ADMISSION_PASS,
	ADMISSION_ENTER,
	ADMISSION_SUCCESS,
	ADMISSION_FAILURE,
	ADMISSION_EVENT_COUNT,
};

enum admission_emergency {
	ADMISSION_NO_EMERGENCY,
	ADMISSION_NO_DOMAIN,
	ADMISSION_TIMEOUT_BURST,
	ADMISSION_DISABLED,
};

/* All fields are int so a sysctl update can validate and publish one vector. */
struct admission_policy {
	int warning_pages;
	int tight_pages;
	int recovery_pages;
	int sample_ms;
	int dwell_ms;
	int warning_top;
	int warning_fgs;
	int tight_top;
	int tight_fgs;
	int warning_limit;
	int tight_limit;
	int aging_us;
	int deadline_us;
	int queue_limit;
	int timeout_burst;
};

struct admission_class {
	u64 generation;
	enum admission_role role;
};

struct admission_identity {
	struct hlist_node hash;
	struct list_head expired;
	struct pid *tgid;
	u64 expires;
	struct admission_class class;
};

struct admission_request {
	struct hlist_node hash;
	struct list_head list;
	struct task_struct *task;
	struct mm_struct *mm;
	unsigned long address;
	u64 id;
	u64 epoch;
	u64 role_generation;
	u64 arrival;
	u64 aging;
	u64 deadline;
	u64 entered;
	enum admission_role role;
	enum admission_state state;
	bool retry_pending;
};

struct admission_counts {
	u64 events[ADMISSION_EVENT_COUNT];
	u64 wait[ADMISSION_HIST_BUCKETS];
	u64 allocation[ADMISSION_HIST_BUCKETS];
	u64 total[ADMISSION_HIST_BUCKETS];
};

DEFINE_STATIC_KEY_FALSE(anon_admission_key);
bool anon_admission_pressure_active;

static DEFINE_MUTEX(admission_control_lock);
static DEFINE_SPINLOCK(admission_lock);
static DEFINE_HASHTABLE(admission_identities, 8);
static DEFINE_HASHTABLE(admission_requests, 8);
static DEFINE_PER_CPU(struct admission_counts[ADMISSION_ROLE_COUNT],
		      admission_counts);
static LIST_HEAD(admission_free);
static LIST_HEAD(admission_top);
static LIST_HEAD(admission_fgs);
static struct admission_request admission_pool[ADMISSION_REQUESTS];

static struct admission_policy admission_policy = {
	.warning_pages = 1024,
	.tight_pages = 256,
	.recovery_pages = 2048,
	.sample_ms = 20,
	.dwell_ms = 60,
	.warning_top = 2,
	.warning_fgs = 1,
	.tight_top = 3,
	.tight_fgs = 1,
	.warning_limit = 4,
	.tight_limit = 2,
	.aging_us = 1000,
	.deadline_us = 2000,
	.queue_limit = 128,
	.timeout_burst = 8,
};

#ifdef CONFIG_CPUSETS
/* A nonzero pair selects the experiment classifier exclusively. */
static unsigned int experiment_uids[2];
static u64 experiment_generation;
#endif

static bool admission_enabled;
static bool admission_sampled;
static unsigned int admission_roles;
static unsigned int admission_active_requests;
static unsigned int admission_queued;
static unsigned int admission_inflight;
static unsigned int admission_by_role[ADMISSION_ROLE_COUNT];
static unsigned int admission_cursor[ADMISSION_PRESSURE_COUNT];
static unsigned int admission_low_samples;
static unsigned int admission_timeouts;
static enum admission_pressure admission_pressure;
static enum admission_emergency admission_emergency;
static u64 admission_epoch;
static u64 admission_policy_version;
static u64 admission_role_generation;
static u64 admission_request_id;
static u64 admission_since;
static u64 admission_healthy_since;
static u64 admission_valid_until;
static long admission_low;
static long admission_high;
static unsigned long admission_scan;
static unsigned long admission_stall;
static unsigned long admission_psi;

static void admission_sample(struct work_struct *work);
static DECLARE_DELAYED_WORK(admission_work, admission_sample);

static bool admission_open(u64 now)
{
	return anon_admission_pressure_active && now < admission_valid_until;
}

static struct list_head *admission_queue(enum admission_role role)
{
	return role == ADMISSION_TOP ? &admission_top : &admission_fgs;
}

static struct admission_identity *admission_identity(struct pid *tgid)
{
	struct admission_identity *identity;

	hash_for_each_possible(admission_identities, identity, hash,
			       (unsigned long)tgid)
		if (identity->tgid == tgid)
			return identity;
	return NULL;
}

/* Called under admission_lock on enqueue, grant and fault retry. */
static struct admission_class admission_classify(struct task_struct *task, u64 now)
{
	struct admission_identity *identity;

#ifdef CONFIG_CPUSETS
	if (experiment_uids[0]) {
		uid_t uid = __kuid_val(task_uid(task));
		enum admission_role role;
		struct cgroup *group, *parent;
		char name[sizeof("foreground")];
		const char *expected;

		if (uid == experiment_uids[0]) {
			role = ADMISSION_TOP;
			expected = "top-app";
		} else if (uid == experiment_uids[1]) {
			role = ADMISSION_FGS;
			expected = "foreground";
		} else {
			return (struct admission_class){};
		}

		rcu_read_lock();
		group = task_cgroup(task, cpuset_cgrp_id);
		parent = cgroup_parent(group);
		if (!parent || cgroup_parent(parent) ||
		    cgroup_name(group, name, sizeof(name)) >= sizeof(name) ||
		    strcmp(name, expected))
			role = ADMISSION_UNCLASSIFIED;
		rcu_read_unlock();
		return (struct admission_class){ .generation = experiment_generation,
						.role = role };
	}
#endif
	identity = admission_identity(task_tgid(task));
	if (identity && now < identity->expires)
		return identity->class;
	return (struct admission_class){};
}

static struct admission_request *admission_request(struct task_struct *task)
{
	struct admission_request *request;

	hash_for_each_possible(admission_requests, request, hash,
			       (unsigned long)task)
		if (request->task == task)
			return request;
	return NULL;
}

static void admission_event(struct admission_request *request,
			    enum admission_event event)
{
	this_cpu_inc(admission_counts[request->role].events[event]);
	trace_anon_page_admission(request->id, task_pid_nr(request->task),
				 request->role_generation, 0, request->role,
				 admission_pressure, event);
}

static unsigned int admission_hist_bucket(u64 elapsed)
{
	if (elapsed >= 2 * NSEC_PER_MSEC)
		return 4;
	if (elapsed >= NSEC_PER_MSEC)
		return 3;
	if (elapsed >= 500 * NSEC_PER_USEC)
		return 2;
	if (elapsed >= 250 * NSEC_PER_USEC)
		return 1;
	return 0;
}

static void admission_wait_time(struct admission_request *request, u64 now)
{
	unsigned int bucket = admission_hist_bucket(now - request->arrival);

	this_cpu_inc(admission_counts[request->role].wait[bucket]);
}

static void admission_release(struct admission_request *request)
{
	admission_inflight--;
	admission_by_role[request->role]--;
}

static void admission_bypass(struct admission_request *request,
			     enum admission_event event, u64 now,
			     struct wake_q_head *wake)
{
	if (request->state == ADMISSION_WAITING) {
		list_del(&request->list);
		admission_queued--;
		admission_wait_time(request, now);
		if (event == ADMISSION_TIMEOUT)
			admission_timeouts++;
	} else if (request->state == ADMISSION_GRANTED) {
		admission_release(request);
	} else {
		return;
	}

	request->state = ADMISSION_BYPASS;
	admission_event(request, event);
	wake_q_add(wake, request->task);
}

static void admission_reclassify(struct admission_request *request,
				 enum admission_role role)
{
	struct list_head *queue = admission_queue(role);
	struct admission_request *entry;

	list_del(&request->list);
	request->role = role;
	list_for_each_entry_reverse(entry, queue, list) {
		if (entry->id < request->id) {
			list_add(&request->list, &entry->list);
			return;
		}
	}
	list_add(&request->list, queue);
}

static void admission_revalidate(u64 now, struct wake_q_head *wake)
{
	struct admission_request *request;
	struct admission_class class;
	int bucket;

	hash_for_each(admission_requests, bucket, request, hash) {
		if (request->state != ADMISSION_WAITING &&
		    request->state != ADMISSION_GRANTED)
			continue;

		if (!admission_open(now) || request->epoch != admission_epoch) {
			admission_bypass(request, ADMISSION_PASS, now, wake);
			continue;
		}

		class = admission_classify(request->task, now);
		if (!class.role ||
		    (request->state == ADMISSION_GRANTED &&
		     (request->role != class.role ||
		      request->role_generation != class.generation))) {
			admission_bypass(request, ADMISSION_CANCEL, now, wake);
			continue;
		}

		if (now >= request->deadline) {
			admission_bypass(request, request->state == ADMISSION_WAITING ?
					 ADMISSION_TIMEOUT : ADMISSION_CANCEL,
					 now, wake);
			continue;
		}

		if (request->role != class.role)
			admission_reclassify(request, class.role);
		request->role_generation = class.generation;
	}
}

static struct admission_request *
admission_head(enum admission_role role, u64 now, struct wake_q_head *wake)
{
	struct list_head *queue = admission_queue(role);
	struct admission_request *request;
	struct admission_class class;

	while (!list_empty(queue)) {
		request = list_first_entry(queue, struct admission_request, list);
		class = admission_classify(request->task, now);
		if (role != class.role || request->role_generation != class.generation) {
			admission_bypass(request, ADMISSION_CANCEL, now, wake);
			continue;
		}
		if (now < request->deadline)
			return request;
		admission_bypass(request, ADMISSION_TIMEOUT, now, wake);
	}
	return NULL;
}

static void admission_weights(unsigned int *top, unsigned int *fgs,
			      unsigned int *limit)
{
	switch (admission_pressure) {
	case ADMISSION_TIGHT:
		*top = admission_policy.tight_top;
		*fgs = admission_policy.tight_fgs;
		*limit = admission_policy.tight_limit;
		break;
	case ADMISSION_RECOVERY:
		*top = 1;
		*fgs = 1;
		*limit = min(admission_policy.warning_limit + 2,
			     ADMISSION_MAX_INFLIGHT);
		break;
	default:
		*top = admission_policy.warning_top;
		*fgs = admission_policy.warning_fgs;
		*limit = admission_policy.warning_limit;
	}
}

static struct admission_request *
admission_select(struct admission_request *top, struct admission_request *fgs,
		 unsigned int top_weight, unsigned int fgs_weight, u64 now,
		 bool *aged)
{
	unsigned int *cursor = &admission_cursor[admission_pressure];
	bool top_aged = top && now >= top->aging;
	bool fgs_aged = fgs && now >= fgs->aging;
	struct admission_request *request;
	unsigned int position, tries;

	if (!top && !fgs)
		return NULL;

	if (top_aged || fgs_aged) {
		*aged = true;
		return !fgs_aged || (top_aged && top->id < fgs->id) ? top : fgs;
	}

	for (tries = 0; tries < top_weight + fgs_weight; tries++) {
		position = *cursor;
		*cursor = (position + 1) % (top_weight + fgs_weight);
		/* Interleave the default warning round as TOP, FGS, TOP. */
		if (admission_pressure == ADMISSION_WARNING && top_weight > 1)
			request = !position || position > fgs_weight ? top : fgs;
		else
			request = position < top_weight ? top : fgs;
		if (request)
			return request;
	}
	return NULL;
}

static void admission_dispatch(u64 now, struct wake_q_head *wake)
{
	struct admission_request *request, *top, *fgs;
	unsigned int top_weight, fgs_weight, limit;

	if (!admission_open(now))
		return;

	admission_weights(&top_weight, &fgs_weight, &limit);
	while (admission_inflight < limit) {
		bool aged = false;

		top = admission_head(ADMISSION_TOP, now, wake);
		fgs = admission_head(ADMISSION_FGS, now, wake);
		request = admission_select(top, fgs, top_weight, fgs_weight,
					   now, &aged);
		if (!request)
			break;

		list_del(&request->list);
		admission_queued--;
		request->state = ADMISSION_GRANTED;
		admission_inflight++;
		admission_by_role[request->role]++;
		admission_wait_time(request, now);
		admission_event(request, aged ? ADMISSION_AGED_GRANT :
				ADMISSION_GRANT);
		wake_q_add(wake, request->task);
	}
}

static void admission_set_pressure(enum admission_pressure pressure, u64 now)
{
	bool active = pressure == ADMISSION_WARNING || pressure == ADMISSION_TIGHT ||
		      pressure == ADMISSION_RECOVERY;

	if (pressure != admission_pressure) {
		admission_pressure = pressure;
		admission_since = now;
	}

	if (!active && anon_admission_pressure_active)
		admission_epoch++;
	WRITE_ONCE(anon_admission_pressure_active, active);
}

static bool admission_fault_eligible(struct vm_fault *vmf)
{
	unsigned int flags = vmf->flags;
	unsigned int required = FAULT_FLAG_USER | FAULT_FLAG_WRITE |
				FAULT_FLAG_ALLOW_RETRY | FAULT_FLAG_KILLABLE;
	unsigned int excluded = FAULT_FLAG_RETRY_NOWAIT | FAULT_FLAG_REMOTE |
				FAULT_FLAG_INSTRUCTION | FAULT_FLAG_UNSHARE;

	return (flags & required) == required && !(flags & excluded) &&
	       vmf->vma->vm_mm == current->mm && (vmf->vma->vm_flags & VM_WRITE) &&
	       !userfaultfd_armed(vmf->vma) && !in_atomic() && !irqs_disabled() &&
	       !(current->flags & (PF_EXITING | PF_KTHREAD | PF_IO_WORKER |
				  PF_MEMALLOC | PF_WQ_WORKER)) &&
	       !current->reclaim_state && !tsk_is_oom_victim(current) &&
	       !rt_task(current) && !signal_pending(current) &&
	       anon_admission_domain_matches(vmf->vma->vm_flags);
}

static void admission_wait(struct admission_request *request)
{
	for (;;) {
		DEFINE_WAKE_Q(wake);
		ktime_t next;
		u64 now;

		spin_lock(&admission_lock);
		now = ktime_get_ns();
		if (signal_pending(current))
			admission_bypass(request, ADMISSION_CANCEL, now, &wake);
		else if (!admission_open(now))
			admission_bypass(request, ADMISSION_PASS, now, &wake);
		else if (now >= request->deadline)
			admission_bypass(request, request->state == ADMISSION_WAITING ?
					 ADMISSION_TIMEOUT : ADMISSION_CANCEL,
					 now, &wake);

		admission_dispatch(now, &wake);
		if (request->state != ADMISSION_WAITING) {
			spin_unlock(&admission_lock);
			wake_up_q(&wake);
			return;
		}

		next = ns_to_ktime(now < request->aging ? request->aging :
				  request->deadline);
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock(&admission_lock);
		wake_up_q(&wake);
		schedule_hrtimeout(&next, HRTIMER_MODE_ABS);
	}
}

bool anon_admission_wait_fault(void)
{
	struct admission_request *request;

	if (!READ_ONCE(admission_active_requests))
		return false;

	spin_lock(&admission_lock);
	request = admission_request(current);
	if (request && request->retry_pending)
		request->retry_pending = false;
	else
		request = NULL;
	spin_unlock(&admission_lock);

	/* Only the originating fault can return its record to the pool. */
	if (!request)
		return false;
	admission_wait(request);
	return true;
}

enum anon_admission_action anon_admission_prepare(struct vm_fault *vmf)
{
	enum anon_admission_action action = ANON_ADMISSION_PASS;
	struct admission_request *request;
	struct admission_class class;
	bool eligible = admission_fault_eligible(vmf);
	DEFINE_WAKE_Q(wake);
	u64 now;

	spin_lock(&admission_lock);
	now = ktime_get_ns();
	request = admission_request(current);
	if (!eligible || !admission_open(now)) {
		if (request)
			admission_bypass(request, ADMISSION_CANCEL, now, &wake);
		goto unlock;
	}

	class = admission_classify(current, now);
	if (!class.role) {
		if (request)
			admission_bypass(request, ADMISSION_CANCEL, now, &wake);
		goto unlock;
	}

	if (!request) {
		if (!fault_flag_allow_retry_first(vmf->flags))
			goto unlock;
		if (admission_queued >= admission_policy.queue_limit ||
		    list_empty(&admission_free)) {
			this_cpu_inc(admission_counts[class.role].events[ADMISSION_PASS]);
			action = class.role == ADMISSION_TOP ?
				 ANON_ADMISSION_MEASURE_TOP : ANON_ADMISSION_MEASURE_FGS;
			goto unlock;
		}

		request = list_first_entry(&admission_free,
					   struct admission_request, list);
		list_del(&request->list);
		request->task = current;
		request->mm = current->mm;
		request->address = vmf->address;
		request->id = ++admission_request_id;
		request->epoch = admission_epoch;
		request->role_generation = class.generation;
		request->arrival = now;
		request->aging = now + admission_policy.aging_us * NSEC_PER_USEC;
		request->deadline = now + admission_policy.deadline_us * NSEC_PER_USEC;
		request->role = class.role;
		request->state = ADMISSION_WAITING;
		request->retry_pending = false;
		hash_add(admission_requests, &request->hash, (unsigned long)current);
		admission_active_requests++;
		list_add_tail(&request->list, admission_queue(request->role));
		admission_queued++;
		admission_event(request, ADMISSION_ENQUEUE);
		admission_dispatch(now, &wake);
	}

	if (request->state == ADMISSION_GRANTED) {
		if (request->mm == current->mm && request->address == vmf->address &&
		    request->epoch == admission_epoch && now < request->deadline &&
		    request->role == class.role &&
		    request->role_generation == class.generation) {
			request->state = ADMISSION_IN_ALLOC;
			request->entered = now;
			admission_event(request, ADMISSION_ENTER);
			action = ANON_ADMISSION_ALLOCATE;
		} else {
			admission_bypass(request, ADMISSION_CANCEL, now, &wake);
		}
	} else if (request->state == ADMISSION_WAITING) {
		request->retry_pending = true;
		action = ANON_ADMISSION_RETRY;
	}
unlock:
	/*
	 * Once a waited request falls back to the native allocator, keep
	 * timing it without reserving a permit.
	 */
	if (request && request->state == ADMISSION_BYPASS) {
		request->state = ADMISSION_IN_BYPASS;
		request->entered = now;
		admission_event(request, ADMISSION_ENTER);
		action = ANON_ADMISSION_ALLOCATE;
	}
	admission_dispatch(now, &wake);
	spin_unlock(&admission_lock);
	wake_up_q(&wake);

	if (action == ANON_ADMISSION_RETRY) {
		assert_fault_locked(vmf);
		release_fault_lock(vmf);
		/* The architecture waits, then performs a fresh VMA/PTE lookup. */
	}

	return action;
}

void anon_admission_finish(enum anon_admission_action action, bool success)
{
	struct admission_request *request;
	enum admission_event event = success ? ADMISSION_SUCCESS : ADMISSION_FAILURE;
	DEFINE_WAKE_Q(wake);
	unsigned int bucket;
	u64 now;

	if (action != ANON_ADMISSION_ALLOCATE) {
		enum admission_role role = action == ANON_ADMISSION_MEASURE_TOP ?
					   ADMISSION_TOP : ADMISSION_FGS;

		this_cpu_inc(admission_counts[role].events[event]);
		return;
	}

	spin_lock(&admission_lock);
	request = admission_request(current);
	if (request && (request->state == ADMISSION_IN_ALLOC ||
			request->state == ADMISSION_IN_BYPASS)) {
		now = ktime_get_ns();
		if (request->state == ADMISSION_IN_ALLOC)
			admission_release(request);
		request->state = ADMISSION_DONE;
		bucket = admission_hist_bucket(now - request->entered);
		this_cpu_inc(admission_counts[request->role].allocation[bucket]);
		bucket = admission_hist_bucket(now - request->arrival);
		this_cpu_inc(admission_counts[request->role].total[bucket]);
		admission_event(request, event);
		admission_dispatch(now, &wake);
	}
	spin_unlock(&admission_lock);
	wake_up_q(&wake);
}

void anon_admission_cancel_fault(void)
{
	struct admission_request *request;
	DEFINE_WAKE_Q(wake);
	u64 now;

	if (!READ_ONCE(admission_active_requests))
		return;

	spin_lock(&admission_lock);
	request = admission_request(current);
	if (request) {
		now = ktime_get_ns();
		admission_bypass(request, ADMISSION_CANCEL, now, &wake);
		admission_dispatch(now, &wake);
	}
	spin_unlock(&admission_lock);
	wake_up_q(&wake);
}

void anon_admission_end_fault(void)
{
	struct admission_request *request;
	DEFINE_WAKE_Q(wake);
	u64 now;

	if (!READ_ONCE(admission_active_requests))
		return;

	spin_lock(&admission_lock);
	request = admission_request(current);
	if (request) {
		now = ktime_get_ns();
		admission_bypass(request, ADMISSION_CANCEL, now, &wake);
		if (WARN_ON_ONCE(request->state == ADMISSION_IN_ALLOC))
			admission_release(request);
		hash_del(&request->hash);
		list_add(&request->list, &admission_free);
		admission_active_requests--;
		admission_dispatch(now, &wake);
	}
	spin_unlock(&admission_lock);
	wake_up_q(&wake);
}

static void admission_retire_identity(struct admission_identity *identity,
				      struct list_head *expired)
{
	hash_del(&identity->hash);
	list_add_tail(&identity->expired, expired);
	admission_roles--;
}

static void admission_prune_identities(u64 now, struct list_head *expired)
{
	struct admission_identity *identity;
	struct hlist_node *next;
	int bucket;

	hash_for_each_safe(admission_identities, bucket, next, identity, hash)
		if (now >= identity->expires)
			admission_retire_identity(identity, expired);
}

static void admission_free_identities(struct list_head *expired)
{
	struct admission_identity *identity, *next;

	list_for_each_entry_safe(identity, next, expired, expired) {
		put_pid(identity->tgid);
		kfree(identity);
	}
}

static void admission_update_pressure(bool valid, long low, long high,
				      unsigned long scan, unsigned long stall,
				      u64 now)
{
	const struct admission_policy *policy = &admission_policy;
	bool dwelling = now - admission_since >= policy->dwell_ms * NSEC_PER_MSEC;
	bool reclaim = admission_sampled &&
		       (scan != admission_scan || stall != admission_stall);
	bool falling = valid && admission_sampled &&
		       admission_low - low >= max(policy->warning_pages / 4, 1);
	bool warning, tight, recovering, sustained_recovery;

	if (!valid || admission_timeouts >= policy->timeout_burst) {
		admission_emergency = valid ? ADMISSION_TIMEOUT_BURST :
					      ADMISSION_NO_DOMAIN;
		admission_set_pressure(ADMISSION_EMERGENCY, now);
		admission_since = now;
		admission_low_samples = 0;
		admission_healthy_since = 0;
		return;
	}

	if (admission_pressure == ADMISSION_EMERGENCY && !dwelling)
		return;

	admission_emergency = ADMISSION_NO_EMERGENCY;
	if (low <= policy->warning_pages)
		admission_low_samples = min(admission_low_samples + 1, 2U);
	else
		admission_low_samples = 0;

	warning = admission_low_samples == 2 ||
		  (falling && low <= policy->warning_pages + policy->tight_pages);
	tight = low <= 0 || (low <= policy->tight_pages && reclaim);
	recovering = high >= policy->recovery_pages &&
		     low >= policy->recovery_pages;
	if (!recovering)
		admission_healthy_since = 0;
	else if (!admission_healthy_since)
		admission_healthy_since = now;
	sustained_recovery = recovering &&
		now - admission_healthy_since >= policy->dwell_ms * NSEC_PER_MSEC;

	if (tight) {
		admission_set_pressure(ADMISSION_TIGHT, now);
	} else if (sustained_recovery && dwelling) {
		if (admission_pressure == ADMISSION_RECOVERY ||
		    admission_pressure == ADMISSION_EMERGENCY)
			admission_set_pressure(ADMISSION_NORMAL, now);
		else if (admission_pressure != ADMISSION_NORMAL)
			admission_set_pressure(ADMISSION_RECOVERY, now);
	} else if (warning && (admission_pressure == ADMISSION_NORMAL ||
			       admission_pressure == ADMISSION_RECOVERY ||
			       admission_pressure == ADMISSION_EMERGENCY)) {
		admission_set_pressure(ADMISSION_WARNING, now);
	} else if (admission_pressure == ADMISSION_EMERGENCY && dwelling) {
		admission_set_pressure(ADMISSION_NORMAL, now);
	} else if (admission_pressure == ADMISSION_RECOVERY && !recovering) {
		admission_set_pressure(ADMISSION_WARNING, now);
	}
}

static void admission_sample(struct work_struct *work)
{
	unsigned long vm_events[NR_VM_EVENT_ITEMS];
	unsigned long scan, stall;
	LIST_HEAD(expired);
	DEFINE_WAKE_Q(wake);
	long low, high;
	bool valid;
	u64 now;

	valid = anon_admission_zone_margin(&low, &high);
	all_vm_events(vm_events);
	scan = vm_events[PGSCAN_DIRECT] + vm_events[PGSCAN_KSWAPD];
	stall = vm_events[ALLOCSTALL_NORMAL];
#ifdef CONFIG_ZONE_DMA
	stall += vm_events[ALLOCSTALL_DMA];
#endif
#ifdef CONFIG_ZONE_DMA32
	stall += vm_events[ALLOCSTALL_DMA32];
#endif

	spin_lock(&admission_lock);
	if (!admission_enabled) {
		spin_unlock(&admission_lock);
		return;
	}

	now = ktime_get_ns();
	admission_update_pressure(valid, low, high, scan, stall, now);
	admission_sampled = valid;
	admission_low = low;
	admission_high = high;
	admission_scan = scan;
	admission_stall = stall;
	admission_timeouts = 0;
#ifdef CONFIG_PSI
	admission_psi = READ_ONCE(psi_system.avg[PSI_MEM_SOME][0]);
#endif
	admission_valid_until = now + 4ULL * admission_policy.sample_ms *
				NSEC_PER_MSEC;
	admission_prune_identities(now, &expired);
	admission_revalidate(now, &wake);
	admission_dispatch(now, &wake);
	schedule_delayed_work(&admission_work,
			      max(1UL, msecs_to_jiffies(admission_policy.sample_ms)));
	spin_unlock(&admission_lock);
	wake_up_q(&wake);
	admission_free_identities(&expired);
}

static int admission_set_enabled(struct ctl_table *table, int write,
				 void *buffer, size_t *len, loff_t *ppos)
{
	struct ctl_table copy = *table;
	DEFINE_WAKE_Q(wake);
	int enabled, err;
	u64 now;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	mutex_lock(&admission_control_lock);
	enabled = admission_enabled;
	copy.data = &enabled;
	err = proc_dointvec_minmax(&copy, write, buffer, len, ppos);
	if (err || !write || !!enabled == admission_enabled)
		goto unlock;

	spin_lock(&admission_lock);
	now = ktime_get_ns();
	admission_enabled = enabled;
	admission_since = now;
	admission_healthy_since = 0;
	admission_sampled = false;
	admission_low_samples = 0;
	admission_timeouts = 0;
	admission_emergency = enabled ? ADMISSION_NO_EMERGENCY :
					ADMISSION_DISABLED;
	admission_set_pressure(enabled ? ADMISSION_NORMAL : ADMISSION_EMERGENCY,
			       now);
	admission_revalidate(now, &wake);
	spin_unlock(&admission_lock);
	wake_up_q(&wake);

	if (enabled) {
		static_branch_enable(&anon_admission_key);
		schedule_delayed_work(&admission_work, 0);
	} else {
		static_branch_disable(&anon_admission_key);
		cancel_delayed_work_sync(&admission_work);
	}
unlock:
	mutex_unlock(&admission_control_lock);
	return err;
}

static bool admission_policy_valid(const struct admission_policy *p)
{
	return p->tight_pages >= 0 && p->warning_pages > p->tight_pages &&
	       p->recovery_pages > p->warning_pages &&
	       p->warning_pages <= INT_MAX / 2 &&
	       p->sample_ms >= 5 && p->sample_ms <= 1000 &&
	       p->dwell_ms >= p->sample_ms && p->dwell_ms <= 10000 &&
	       p->warning_top > 0 && p->warning_top <= ADMISSION_MAX_WEIGHT &&
	       p->warning_fgs > 0 && p->warning_fgs <= ADMISSION_MAX_WEIGHT &&
	       p->tight_top > 0 && p->tight_top <= ADMISSION_MAX_WEIGHT &&
	       p->tight_fgs > 0 && p->tight_fgs <= ADMISSION_MAX_WEIGHT &&
	       p->tight_limit > 0 && p->tight_limit <= p->warning_limit &&
	       p->warning_limit <= ADMISSION_MAX_INFLIGHT &&
	       p->aging_us > 0 && p->deadline_us > p->aging_us &&
	       p->deadline_us <= 10000 && p->queue_limit > 0 &&
	       p->queue_limit <= ADMISSION_REQUESTS - ADMISSION_MAX_INFLIGHT &&
	       p->timeout_burst > 0 && p->timeout_burst <= ADMISSION_REQUESTS;
}

static int admission_set_policy(struct ctl_table *table, int write,
				void *buffer, size_t *len, loff_t *ppos)
{
	struct admission_policy candidate;
	struct ctl_table copy = *table;
	DEFINE_WAKE_Q(wake);
	int err;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (write && *ppos)
		return -EINVAL;

	mutex_lock(&admission_control_lock);
	candidate = admission_policy;
	copy.data = &candidate;
	err = proc_dointvec(&copy, write, buffer, len, ppos);
	if (err || !write)
		goto unlock;
	if (!admission_policy_valid(&candidate)) {
		err = -EINVAL;
		goto unlock;
	}

	spin_lock(&admission_lock);
	admission_policy = candidate;
	admission_policy_version++;
	admission_cursor[ADMISSION_WARNING] %= candidate.warning_top +
					      candidate.warning_fgs;
	admission_cursor[ADMISSION_TIGHT] %= candidate.tight_top +
					    candidate.tight_fgs;
	admission_dispatch(ktime_get_ns(), &wake);
	spin_unlock(&admission_lock);
	wake_up_q(&wake);
	if (admission_enabled)
		mod_delayed_work(system_wq, &admission_work, 0);
unlock:
	mutex_unlock(&admission_control_lock);
	return err;
}

#ifdef CONFIG_CPUSETS
static int admission_set_experiment(struct ctl_table *table, int write,
				    void *buffer, size_t *len, loff_t *ppos)
{
	char command[32], *first, *second;
	struct ctl_table copy = *table;
	unsigned int top, fgs;
	DEFINE_WAKE_Q(wake);
	u64 now;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (write && (*ppos || !*len || *len >= sizeof(command)))
		return -EINVAL;
	spin_lock(&admission_lock);
	snprintf(command, sizeof(command), "%u %u", experiment_uids[0],
		 experiment_uids[1]);
	spin_unlock(&admission_lock);
	copy.data = command;
	err = proc_dostring(&copy, write, buffer, len, ppos);
	if (err || !write)
		return err;
	second = strim(command);
	first = strsep(&second, " \t");
	if (!second || kstrtouint(first, 10, &top) ||
	    kstrtouint(skip_spaces(second), 10, &fgs) ||
	    (!!top != !!fgs) || (top && top == fgs) ||
	    top == (uid_t)-1 || fgs == (uid_t)-1)
		return -EINVAL;

	spin_lock(&admission_lock);
	if (top != experiment_uids[0] || fgs != experiment_uids[1]) {
		experiment_uids[0] = top;
		experiment_uids[1] = fgs;
		experiment_generation = ++admission_role_generation;
		admission_epoch++;
		now = ktime_get_ns();
		admission_revalidate(now, &wake);
		admission_dispatch(now, &wake);
	}
	spin_unlock(&admission_lock);
	wake_up_q(&wake);
	return 0;
}
#endif

static int admission_publish_role(struct ctl_table *table, int write,
				  void *buffer, size_t *len, loff_t *ppos)
{
	struct admission_identity *identity, *candidate = NULL;
	struct ctl_table copy = *table;
	struct task_struct *task;
	char command[64] = {};
	char trailing;
	unsigned int fd, role, lease_ms, flags;
	LIST_HEAD(expired);
	DEFINE_WAKE_Q(wake);
	struct pid *tgid;
	int err;
	u64 now;

	if (!write || !capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (*ppos || !*len || *len >= sizeof(command))
		return -EINVAL;

	copy.data = command;
	err = proc_dostring(&copy, write, buffer, len, ppos);
	if (err)
		return err;
	if (sscanf(command, "%u %u %u %c", &fd, &role, &lease_ms, &trailing) != 3 ||
	    role >= ADMISSION_ROLE_COUNT || lease_ms > ADMISSION_MAX_LEASE_MS ||
	    (!!role != !!lease_ms))
		return -EINVAL;

	tgid = pidfd_get_pid(fd, &flags);
	if (IS_ERR(tgid))
		return PTR_ERR(tgid);
	task = get_pid_task(tgid, PIDTYPE_TGID);
	if (!task || READ_ONCE(task->flags) & (PF_KTHREAD | PF_EXITING)) {
		err = -ESRCH;
		goto put_task;
	}

	if (role) {
		candidate = kzalloc(sizeof(*candidate), GFP_KERNEL);
		if (!candidate) {
			err = -ENOMEM;
			goto put_task;
		}
	}

	spin_lock(&admission_lock);
	now = ktime_get_ns();
	admission_prune_identities(now, &expired);
	identity = admission_identity(tgid);
	if (!role) {
		if (identity)
			admission_retire_identity(identity, &expired);
	} else if (identity) {
		if (identity->class.role != role) {
			identity->class.role = role;
			identity->class.generation = ++admission_role_generation;
		}
		identity->expires = now + lease_ms * NSEC_PER_MSEC;
	} else if (admission_roles < ADMISSION_ROLES) {
		candidate->tgid = get_pid(tgid);
		candidate->class.role = role;
		candidate->class.generation = ++admission_role_generation;
		candidate->expires = now + lease_ms * NSEC_PER_MSEC;
		hash_add(admission_identities, &candidate->hash, (unsigned long)tgid);
		admission_roles++;
		candidate = NULL;
	} else {
		err = -ENOSPC;
	}

	admission_revalidate(now, &wake);
	admission_dispatch(now, &wake);
	spin_unlock(&admission_lock);
	wake_up_q(&wake);
	admission_free_identities(&expired);
	kfree(candidate);
put_task:
	if (task)
		put_task_struct(task);
	put_pid(tgid);
	return err;
}

static int admission_read_stats(struct ctl_table *table, int write,
				void *buffer, size_t *len, loff_t *ppos)
{
	static const char * const names[] = { "unclassified", "top", "fgs" };
	struct ctl_table copy = *table;
	struct admission_counts totals[ADMISSION_ROLE_COUNT] = {};
	struct seq_buf stats;
	char *data;
	unsigned int top, fgs, limit;
	int cpu, role, i, err;

	if (write || !capable(CAP_SYS_ADMIN))
		return -EPERM;
	data = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	seq_buf_init(&stats, data, PAGE_SIZE);

	spin_lock(&admission_lock);
	admission_weights(&top, &fgs, &limit);
	seq_buf_printf(&stats,
		       "node 0 pressure %u emergency %u policy %llu epoch %llu queued %u inflight %u low_pages %ld high_pages %ld ",
		       admission_pressure, admission_emergency,
		       admission_policy_version, admission_epoch, admission_queued,
		       admission_inflight, admission_low, admission_high);
	seq_buf_printf(&stats, "psi_mem_some_avg10_x100 %lu top_weight %u fgs_weight %u permit_limit %u\n",
		       (admission_psi * 100) >> FSHIFT, top, fgs,
		       anon_admission_pressure_active ? limit : 0);
	seq_buf_printf(&stats, "top_inflight %u fgs_inflight %u active_requests %u\n",
		       admission_by_role[ADMISSION_TOP],
		       admission_by_role[ADMISSION_FGS], admission_active_requests);
	spin_unlock(&admission_lock);

	for_each_possible_cpu(cpu) {
		for (role = ADMISSION_TOP; role < ADMISSION_ROLE_COUNT; role++) {
			const struct admission_counts *counts =
				&per_cpu(admission_counts, cpu)[role];

			for (i = 0; i < ADMISSION_EVENT_COUNT; i++)
				totals[role].events[i] += READ_ONCE(counts->events[i]);
			for (i = 0; i < ADMISSION_HIST_BUCKETS; i++) {
				totals[role].wait[i] += READ_ONCE(counts->wait[i]);
				totals[role].allocation[i] += READ_ONCE(counts->allocation[i]);
				totals[role].total[i] += READ_ONCE(counts->total[i]);
			}
		}
	}

	seq_buf_puts(&stats, "role enqueue grant aged timeout cancel bypass enter success failure\n");
	for (role = ADMISSION_TOP; role < ADMISSION_ROLE_COUNT; role++) {
		seq_buf_printf(&stats, "%s", names[role]);
		for (i = 0; i < ADMISSION_EVENT_COUNT; i++)
			seq_buf_printf(&stats, " %llu", totals[role].events[i]);
		seq_buf_puts(&stats, "\n  wait_us");
		for (i = 0; i < ADMISSION_HIST_BUCKETS; i++)
			seq_buf_printf(&stats, " %llu", totals[role].wait[i]);
		seq_buf_puts(&stats, "\n  allocation_us");
		for (i = 0; i < ADMISSION_HIST_BUCKETS; i++)
			seq_buf_printf(&stats, " %llu", totals[role].allocation[i]);
		seq_buf_puts(&stats, "\n  total_us");
		for (i = 0; i < ADMISSION_HIST_BUCKETS; i++)
			seq_buf_printf(&stats, " %llu", totals[role].total[i]);
		if (role != ADMISSION_FGS)
			seq_buf_putc(&stats, '\n');
	}

	copy.data = data;
	copy.maxlen = PAGE_SIZE;
	err = proc_dostring(&copy, write, buffer, len, ppos);
	kfree(data);
	return err;
}

static struct ctl_table admission_sysctls[] = {
	{
		.procname = "enabled",
		.maxlen = sizeof(int),
		.mode = 0600,
		.proc_handler = admission_set_enabled,
		.extra1 = SYSCTL_ZERO,
		.extra2 = SYSCTL_ONE,
	},
	{
		.procname = "policy",
		.maxlen = sizeof(struct admission_policy),
		.mode = 0600,
		.proc_handler = admission_set_policy,
	},
#ifdef CONFIG_CPUSETS
	{
		.procname = "experiment_uids",
		.maxlen = 32,
		.mode = 0600,
		.proc_handler = admission_set_experiment,
	},
#endif
	{
		.procname = "role",
		.maxlen = 64,
		.mode = 0200,
		.proc_handler = admission_publish_role,
	},
	{
		.procname = "stats",
		.mode = 0400,
		.proc_handler = admission_read_stats,
	},
	{}
};

static int __init anon_admission_init(void)
{
	int i;

	for (i = 0; i < ADMISSION_REQUESTS; i++)
		list_add(&admission_pool[i].list, &admission_free);
	return register_sysctl("vm/anon_page_admission", admission_sysctls) ?
		0 : -ENOMEM;
}
late_initcall(anon_admission_init);

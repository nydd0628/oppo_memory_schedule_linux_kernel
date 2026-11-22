// SPDX-License-Identifier: GPL-2.0-only
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/shrinker.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "fg_page_pool.h"

#define CREATE_TRACE_POINTS
#include <trace/events/fg_page_pool.h>

#define FG_POOL_MAX_BYTES SZ_32M
#define FG_POOL_MAX_LEASE_MS 60000

struct fg_pool_member {
	struct pid *tgid;
	struct mm_struct *mm;
};

struct fg_pool_registry {
	unsigned long expires;
	unsigned int count;
	struct fg_pool_member members[];
};

static DEFINE_MUTEX(fg_pool_control_lock);
static struct fg_pool_registry __rcu *fg_pool_registry;
static u64 fg_pool_generation;
static u32 fg_pool_interval_ms;

bool fg_pool_current_eligible(void)
{
	struct fg_pool_registry *reg = rcu_dereference(fg_pool_registry);
	unsigned int i;

	if (!reg || time_after_eq(jiffies, reg->expires))
		return false;

	for (i = 0; i < reg->count; i++) {
		if (reg->members[i].tgid == task_tgid(current) &&
		    reg->members[i].mm == current->mm)
			return true;
	}

	return false;
}

static void fg_pool_free_registry(struct fg_pool_registry *reg)
{
	unsigned int i;

	if (!reg)
		return;

	for (i = 0; i < reg->count; i++) {
		put_pid(reg->members[i].tgid);
		mmdrop(reg->members[i].mm);
	}
	kfree(reg);
}

static void fg_pool_publish(struct fg_pool_registry *reg)
{
	struct fg_pool_registry *old;

	old = rcu_replace_pointer(fg_pool_registry, reg,
				  lockdep_is_held(&fg_pool_control_lock));
	if (old) {
		synchronize_rcu();
		fg_pool_free_registry(old);
	}
}

static int fg_pool_replace_members(const struct fg_pool_members *req)
{
	struct fg_pool_registry *reg;
	unsigned int i, j, flags;
	int ret;

	if (req->version != FG_POOL_ABI_VERSION || req->reserved ||
	    req->count > FG_POOL_MAX_MEMBERS ||
	    req->lease_ms > FG_POOL_MAX_LEASE_MS ||
	    (req->count && !req->lease_ms))
		return -EINVAL;

	if (req->generation <= fg_pool_generation)
		return -ESTALE;

	if (!req->count) {
		fg_pool_publish(NULL);
		fg_pool_generation = req->generation;
		return 0;
	}

	reg = kzalloc(struct_size(reg, members, req->count), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;

	for (i = 0; i < req->count; i++) {
		struct task_struct *task;
		struct mm_struct *mm;
		struct pid *tgid;

		if (req->pidfds[i] < 0) {
			ret = -EBADF;
			goto fail;
		}

		tgid = pidfd_get_pid(req->pidfds[i], &flags);
		if (IS_ERR(tgid)) {
			ret = PTR_ERR(tgid);
			goto fail;
		}

		for (j = 0; j < i; j++) {
			if (reg->members[j].tgid == tgid) {
				put_pid(tgid);
				ret = -EINVAL;
				goto fail;
			}
		}

		task = get_pid_task(tgid, PIDTYPE_TGID);
		mm = task ? get_task_mm(task) : NULL;
		if (task)
			put_task_struct(task);
		if (!mm) {
			put_pid(tgid);
			ret = -ESRCH;
			goto fail;
		}

		mmgrab(mm);
		mmput(mm);
		reg->members[i].tgid = tgid;
		reg->members[i].mm = mm;
		reg->count++;
	}

	reg->expires = jiffies + msecs_to_jiffies(req->lease_ms);
	fg_pool_publish(reg);
	fg_pool_generation = req->generation;
	return 0;

fail:
	fg_pool_free_registry(reg);
	return ret;
}

static void fg_pool_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(fg_pool_work, fg_pool_workfn);

static void fg_pool_workfn(struct work_struct *work)
{
	u32 interval = READ_ONCE(fg_pool_interval_ms);

	if (!interval)
		return;

	fg_pool_refill();
	schedule_delayed_work(&fg_pool_work, msecs_to_jiffies(interval));
}

static void fg_pool_stop(void)
{
	WRITE_ONCE(fg_pool_interval_ms, 0);
	cancel_delayed_work_sync(&fg_pool_work);
	fg_pool_disable();
}

static int fg_pool_set_config(const struct fg_pool_config *cfg)
{
	struct zone *zone;

	if (cfg->version != FG_POOL_ABI_VERSION ||
	    memchr_inv(cfg->reserved, 0, sizeof(cfg->reserved)) ||
	    cfg->mode < FG_POOL_SHARED || cfg->mode > FG_POOL_FOREGROUND_FAST ||
	    (cfg->flags & ~(FG_POOL_CONFIG_ELASTIC |
			    FG_POOL_CONFIG_RECLAIM_REFILL |
			    FG_POOL_CONFIG_HYBRID_REFILL)) ||
	    ((cfg->flags & FG_POOL_CONFIG_RECLAIM_REFILL) &&
	     (cfg->flags & FG_POOL_CONFIG_HYBRID_REFILL)) ||
	    (cfg->flags && cfg->mode != FG_POOL_FOREGROUND_FAST) ||
	    cfg->nid < 0 || cfg->nid >= MAX_NUMNODES ||
	    cfg->zone != ZONE_NORMAL ||
	    !cfg->target_pages || cfg->target_pages > FG_POOL_MAX_BYTES / PAGE_SIZE ||
	    cfg->startup_pages >= cfg->target_pages ||
	    (cfg->startup_pages && (cfg->flags & FG_POOL_CONFIG_RECLAIM_REFILL)) ||
	    cfg->interval_ms < 10 || cfg->interval_ms > 1000 ||
	    cfg->cooldown_ms < cfg->interval_ms || cfg->cooldown_ms > 10000)
		return -EINVAL;

	if (!node_online(cfg->nid))
		return -ENODEV;

	zone = &NODE_DATA(cfg->nid)->node_zones[cfg->zone];
	if (!managed_zone(zone))
		return -ENODEV;

	if (cfg->target_pages > zone_managed_pages(zone) / 64)
		return -EINVAL;

	fg_pool_stop();
	fg_pool_enable(zone, cfg);
	if (cfg->flags & FG_POOL_CONFIG_RECLAIM_REFILL)
		return 0;

	WRITE_ONCE(fg_pool_interval_ms, cfg->interval_ms);
	schedule_delayed_work(&fg_pool_work, 0);
	return 0;
}

static long fg_pool_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *up = (void __user *)arg;
	long ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	mutex_lock(&fg_pool_control_lock);
	switch (cmd) {
	case FG_POOL_SET_CONFIG: {
		struct fg_pool_config cfg;

		ret = copy_from_user(&cfg, up, sizeof(cfg)) ? -EFAULT :
			fg_pool_set_config(&cfg);
		break;
	}
	case FG_POOL_REPLACE_MEMBERS: {
		struct fg_pool_members req;

		ret = copy_from_user(&req, up, sizeof(req)) ? -EFAULT :
			fg_pool_replace_members(&req);
		break;
	}
	case FG_POOL_GET_STATUS: {
		struct fg_pool_status status;

		fg_pool_snapshot(&status);
		status.generation = fg_pool_generation;
		ret = copy_to_user(up, &status, sizeof(status)) ? -EFAULT : 0;
		break;
	}
	case FG_POOL_GET_REFILL_STATS: {
		struct fg_pool_refill_stats stats;

		fg_pool_refill_snapshot(&stats);
		ret = copy_to_user(up, &stats, sizeof(stats)) ? -EFAULT : 0;
		break;
	}
	case FG_POOL_START_STEADY:
		ret = fg_pool_start_steady();
		if (!ret && READ_ONCE(fg_pool_interval_ms))
			mod_delayed_work(system_wq, &fg_pool_work, 0);
		break;
	case FG_POOL_DISABLE:
		fg_pool_stop();
		fg_pool_publish(NULL);
		ret = 0;
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&fg_pool_control_lock);

	return ret;
}

static unsigned long fg_pool_shrink_count(struct shrinker *shrinker,
					  struct shrink_control *sc)
{
	return fg_pool_shrinkable();
}

static unsigned long fg_pool_shrink_scan(struct shrinker *shrinker,
					 struct shrink_control *sc)
{
	unsigned long nr = fg_pool_release(sc->nr_to_scan, FG_POOL_SHRINK);

	sc->nr_scanned = nr;
	return nr ?: SHRINK_STOP;
}

static struct shrinker fg_pool_shrinker = {
	.count_objects = fg_pool_shrink_count,
	.scan_objects = fg_pool_shrink_scan,
	.batch = FG_POOL_MAX_BATCH,
	.seeks = DEFAULT_SEEKS,
};

static const struct file_operations fg_pool_fops = {
	.unlocked_ioctl = fg_pool_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = no_llseek,
};

static struct miscdevice fg_pool_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "fg_page_pool",
	.fops = &fg_pool_fops,
	.mode = 0600,
};

static int __init fg_pool_init(void)
{
	int ret;

	ret = register_shrinker(&fg_pool_shrinker, "fg_page_pool");
	if (ret)
		return ret;

	ret = misc_register(&fg_pool_device);
	if (ret)
		unregister_shrinker(&fg_pool_shrinker);

	return ret;
}
late_initcall(fg_pool_init);

// SPDX-License-Identifier: GPL-2.0-only
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <linux/workqueue.h>

#include "fg_page_pool.h"

#define CREATE_TRACE_POINTS
#include <trace/events/fg_page_pool.h>

#define FG_POOL_MAX_BYTES SZ_32M
#define FG_POOL_MAX_LEASE_MS 60000

struct fg_pool_registry {
	unsigned long expires;
	unsigned int count;
	kuid_t members[];
};

static DEFINE_MUTEX(fg_pool_control_lock);
static struct fg_pool_registry __rcu *fg_pool_registry;
static u64 fg_pool_generation;
static u32 fg_pool_interval_ms;

bool fg_pool_current_eligible(void)
{
	struct fg_pool_registry *reg = rcu_dereference(fg_pool_registry);
	kuid_t uid = current_uid();
	unsigned int i;

	if (!reg || time_after_eq(jiffies, reg->expires))
		return false;

	for (i = 0; i < reg->count; i++)
		if (uid_eq(reg->members[i], uid))
			return true;

	return false;
}

static void fg_pool_publish(struct fg_pool_registry *reg)
{
	struct fg_pool_registry *old;

	old = rcu_replace_pointer(fg_pool_registry, reg,
				  lockdep_is_held(&fg_pool_control_lock));
	if (old) {
		synchronize_rcu();
		kfree(old);
	}
}

static int fg_pool_replace_uids(const struct fg_pool_uid_members *req)
{
	struct fg_pool_registry *reg;
	unsigned int i, j;

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
		kuid_t uid = make_kuid(&init_user_ns, req->uids[i]);

		if (!uid_valid(uid))
			goto fail;
		for (j = 0; j < i; j++)
			if (uid_eq(reg->members[j], uid))
				goto fail;
		reg->members[i] = uid;
	}

	reg->count = req->count;
	reg->expires = jiffies + msecs_to_jiffies(req->lease_ms);
	fg_pool_publish(reg);
	fg_pool_generation = req->generation;
	return 0;

fail:
	kfree(reg);
	return -EINVAL;
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
	    cfg->mode < FG_POOL_FOREGROUND || cfg->mode > FG_POOL_FOREGROUND_FAST ||
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
	case FG_POOL_REPLACE_UIDS: {
		struct fg_pool_uid_members req;

		ret = copy_from_user(&req, up, sizeof(req)) ? -EFAULT :
			fg_pool_replace_uids(&req);
		break;
	}
	case FG_POOL_SET_TARGET: {
		u32 target;

		if (copy_from_user(&target, up, sizeof(target))) {
			ret = -EFAULT;
			break;
		}
		if (target > FG_POOL_MAX_BYTES / PAGE_SIZE) {
			ret = -EINVAL;
			break;
		}
		ret = fg_pool_set_target(target);
		if (!ret && target && READ_ONCE(fg_pool_interval_ms))
			mod_delayed_work(system_wq, &fg_pool_work, 0);
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
	return misc_register(&fg_pool_device);
}
late_initcall(fg_pool_init);

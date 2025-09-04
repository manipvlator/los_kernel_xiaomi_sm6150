// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/power/wakelock.c
 *
 * User space wakeup sources support.
 *
 * Copyright (C) 2012 Rafael J. Wysocki <rjw@sisk.pl>
 *
 * This code is based on the analogous interface allowing user space to
 * manipulate wakelocks on Android.
 */

#include <linux/capability.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/stat.h>

#include "power.h"

static DEFINE_MUTEX(wakelocks_lock);

struct wakelock {
	char			*name;
	struct rb_node		node;
	struct wakeup_source	*ws;
#ifdef CONFIG_PM_WAKELOCKS_GC
	struct list_head	lru;
#endif
};

static struct rb_root wakelocks_tree = RB_ROOT;

static struct wakelock *wakelock_lookup_add(const char *name, size_t len,
					    bool add_if_not_found);

#ifdef CONFIG_WAKELOCK_GOVERNOR
struct wakelock_governor_entry {
	struct list_head list;
	char *name;
	ktime_t start_time;
};

static struct delayed_work governor_work;
static struct kobject *governor_kobj;

static bool governor_enabled = true;
static u64 governor_max_time_ms = 60000;
static bool governor_debug = false;
static u64 governor_stats_released = 0;
static LIST_HEAD(governor_active_list);
static DEFINE_SPINLOCK(governor_lock);

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "%d\n", governor_enabled);
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	int ret;
	bool value;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;

	governor_enabled = value;
	return count;
}

static ssize_t max_time_ms_show(struct kobject *kobj, struct kobj_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%llu\n", governor_max_time_ms);
}

static ssize_t max_time_ms_store(struct kobject *kobj, struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	u64 value;

	ret = kstrtou64(buf, 10, &value);
	if (ret)
		return ret;

	if (value < 1000)
		return -EINVAL;

	governor_max_time_ms = value;
	return count;
}

static ssize_t debug_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sprintf(buf, "%d\n", governor_debug);
}

static ssize_t debug_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	int ret;
	bool value;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;

	governor_debug = value;
	return count;
}

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sprintf(buf, "Wakelocks force-released: %llu\n", governor_stats_released);
}

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "Wakelock Governor v1.0\n");
}

static struct kobj_attribute enabled_attr = __ATTR(enabled, 0664, enabled_show, enabled_store);
static struct kobj_attribute max_time_ms_attr = __ATTR(max_time_ms, 0664, max_time_ms_show, max_time_ms_store);
static struct kobj_attribute debug_attr = __ATTR(debug, 0664, debug_show, debug_store);
static struct kobj_attribute stats_attr = __ATTR(stats, 0444, stats_show, NULL);
static struct kobj_attribute version_attr = __ATTR(version, 0444, version_show, NULL);

static struct attribute *governor_attrs[] = {
	&enabled_attr.attr,
	&max_time_ms_attr.attr,
	&debug_attr.attr,
	&stats_attr.attr,
	&version_attr.attr,
	NULL,
};

static struct attribute_group governor_attr_group = {
	.attrs = governor_attrs,
	.name = "wakelock_governor",
};

static void governor_work_fn(struct work_struct *work)
{
	struct wakelock_governor_entry *entry, *tmp;
	ktime_t now = ktime_get();
	unsigned long flags;
	struct wakelock *wl;
	bool need_reschedule = false;

	spin_lock_irqsave(&governor_lock, flags);
	
	list_for_each_entry_safe(entry, tmp, &governor_active_list, list) {
		u64 elapsed_ms = ktime_to_ms(ktime_sub(now, entry->start_time));
		
		if (elapsed_ms > governor_max_time_ms) {
			mutex_lock(&wakelocks_lock);
			wl = wakelock_lookup_add(entry->name, strlen(entry->name), false);
			if (!IS_ERR(wl)) {
				__pm_relax(wl->ws);
				governor_stats_released++;
				
				if (governor_debug) {
					pr_info("Wakelock governor: Released %s after %llums\n",
					       entry->name, elapsed_ms);
				}
			}
			mutex_unlock(&wakelocks_lock);
			
			list_del(&entry->list);
			kfree(entry->name);
			kfree(entry);
		} else {
			need_reschedule = true;
		}
	}

	spin_unlock_irqrestore(&governor_lock, flags);
	
	if (need_reschedule && governor_enabled) {
		schedule_delayed_work(&governor_work, msecs_to_jiffies(1000));
	}
}

static int __init wakelock_governor_init(void)
{
	int ret;

	governor_kobj = kobject_create_and_add("wakelock_governor", kernel_kobj);
	if (!governor_kobj) {
		pr_err("Failed to create wakelock governor kobject\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(governor_kobj, &governor_attr_group);
	if (ret) {
		pr_err("Failed to create wakelock governor sysfs group: %d\n", ret);
		kobject_put(governor_kobj);
		return ret;
	}

	INIT_DELAYED_WORK(&governor_work, governor_work_fn);
	
	pr_info("Wakelock governor initialized with %llums limit\n", governor_max_time_ms);
	return 0;
}

late_initcall(wakelock_governor_init);
#endif /* CONFIG_WAKELOCK_GOVERNOR */

ssize_t pm_show_wakelocks(char *buf, bool show_active)
{
	struct rb_node *node;
	struct wakelock *wl;
	int len = 0;

	mutex_lock(&wakelocks_lock);

	for (node = rb_first(&wakelocks_tree); node; node = rb_next(node)) {
		wl = rb_entry(node, struct wakelock, node);
		if (wl->ws->active == show_active) {
			if (len < PAGE_SIZE - 1) {
				len += scnprintf(buf + len, PAGE_SIZE - len, "%s ", wl->name);
			}
		}
	}

	if (len < PAGE_SIZE - 1) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	} else {
		// Ensure null termination if buffer is full
		buf[PAGE_SIZE - 1] = '\0';
		len = PAGE_SIZE - 1;
	}

	mutex_unlock(&wakelocks_lock);
	return len;
}

#if CONFIG_PM_WAKELOCKS_LIMIT > 0
static unsigned int number_of_wakelocks;

static inline bool wakelocks_limit_exceeded(void)
{
	return number_of_wakelocks > CONFIG_PM_WAKELOCKS_LIMIT;
}

static inline void increment_wakelocks_number(void)
{
	number_of_wakelocks++;
}

static inline void decrement_wakelocks_number(void)
{
	number_of_wakelocks--;
}
#else /* CONFIG_PM_WAKELOCKS_LIMIT = 0 */
static inline bool wakelocks_limit_exceeded(void) { return false; }
static inline void increment_wakelocks_number(void) {}
static inline void decrement_wakelocks_number(void) {}
#endif /* CONFIG_PM_WAKELOCKS_LIMIT */

#ifdef CONFIG_PM_WAKELOCKS_GC
#define WL_GC_COUNT_MAX	100
#define WL_GC_TIME_SEC	300

static void __wakelocks_gc(struct work_struct *work);
static LIST_HEAD(wakelocks_lru_list);
static DECLARE_WORK(wakelock_work, __wakelocks_gc);
static unsigned int wakelocks_gc_count;

static inline void wakelocks_lru_add(struct wakelock *wl)
{
	list_add(&wl->lru, &wakelocks_lru_list);
}

static inline void wakelocks_lru_most_recent(struct wakelock *wl)
{
	list_move(&wl->lru, &wakelocks_lru_list);
}

static void __wakelocks_gc(struct work_struct *work)
{
	struct wakelock *wl, *aux;
	ktime_t now;
#ifdef CONFIG_WAKELOCK_GOVERNOR
	unsigned long flags;
	struct wakelock_governor_entry *gov_entry, *tmp;
#endif

	mutex_lock(&wakelocks_lock);

	now = ktime_get();
	list_for_each_entry_safe_reverse(wl, aux, &wakelocks_lru_list, lru) {
		u64 idle_time_ns;
		bool active;

		spin_lock_irq(&wl->ws->lock);
		idle_time_ns = ktime_to_ns(ktime_sub(now, wl->ws->last_time));
		active = wl->ws->active;
		spin_unlock_irq(&wl->ws->lock);

		if (idle_time_ns < ((u64)WL_GC_TIME_SEC * NSEC_PER_SEC))
			break;

		if (!active) {
#ifdef CONFIG_WAKELOCK_GOVERNOR
			spin_lock_irqsave(&governor_lock, flags);
			list_for_each_entry_safe(gov_entry, tmp, &governor_active_list, list) {
				if (strcmp(gov_entry->name, wl->name) == 0) {
					list_del(&gov_entry->list);
					kfree(gov_entry->name);
					kfree(gov_entry);
					break;
				}
			}
			spin_unlock_irqrestore(&governor_lock, flags);
#endif
			
			wakeup_source_unregister(wl->ws);
			rb_erase(&wl->node, &wakelocks_tree);
			list_del(&wl->lru);
			kfree(wl->name);
			kfree(wl);
			decrement_wakelocks_number();
		}
	}
	wakelocks_gc_count = 0;

	mutex_unlock(&wakelocks_lock);
}

static void wakelocks_gc(void)
{
	bool expedite;
	int cpu = get_cpu();

	/*
	 * If the current CPU isn't occupied, go ahead and
	 * garbage collect inactive wakelocks.
	 */
	expedite = idle_cpu(cpu);

	put_cpu();

	if (expedite)
		goto do_gc;

	/*
	 * If our CPU is busy, allow wakelocks to
	 * accumulate before attempting to garbage collect.
	 * If by the time we register WL_GC_COUNT_MAX
	 * wakelocks and the current CPU is still busy,
	 * run the garbage collecton anyway.
	 */
	if (++wakelocks_gc_count <= WL_GC_COUNT_MAX)
		return;

do_gc:
	schedule_work(&wakelock_work);
}
#else /* !CONFIG_PM_WAKELOCKS_GC */
static inline void wakelocks_lru_add(struct wakelock *wl) {}
static inline void wakelocks_lru_most_recent(struct wakelock *wl) {}
static inline void wakelocks_gc(void) {}
#endif /* !CONFIG_PM_WAKELOCKS_GC */

static struct wakelock *wakelock_lookup_add(const char *name, size_t len,
					    bool add_if_not_found)
{
	struct rb_node **node = &wakelocks_tree.rb_node;
	struct rb_node *parent = *node;
	struct wakelock *wl;

	while (*node) {
		int diff;

		parent = *node;
		wl = rb_entry(*node, struct wakelock, node);
		diff = strncmp(name, wl->name, len);
		if (diff == 0) {
			if (wl->name[len])
				diff = -1;
			else
				return wl;
		}
		if (diff < 0)
			node = &(*node)->rb_left;
		else
			node = &(*node)->rb_right;
	}
	if (!add_if_not_found)
		return ERR_PTR(-EINVAL);

	if (wakelocks_limit_exceeded())
		return ERR_PTR(-ENOSPC);

	wl = kzalloc(sizeof(*wl), GFP_KERNEL);
	if (!wl)
		return ERR_PTR(-ENOMEM);

	wl->name = kstrndup(name, len, GFP_KERNEL);
	if (!wl->name) {
		kfree(wl);
		return ERR_PTR(-ENOMEM);
	}

	wl->ws = wakeup_source_register(NULL, wl->name);
	if (!wl->ws) {
		kfree(wl->name);
		kfree(wl);
		return ERR_PTR(-ENOMEM);
	}
	wl->ws->last_time = ktime_get();

	rb_link_node(&wl->node, parent, node);
	rb_insert_color(&wl->node, &wakelocks_tree);
	wakelocks_lru_add(wl);
	increment_wakelocks_number();
	return wl;
}

int pm_wake_lock(const char *buf)
{
	const char *str = buf;
	struct wakelock *wl;
	u64 timeout_ns = 0;
	size_t len;
	int ret = 0;
#ifdef CONFIG_WAKELOCK_GOVERNOR
	unsigned long flags;
	struct wakelock_governor_entry *gov_entry;
#endif

	if (!capable(CAP_BLOCK_SUSPEND))
		return -EPERM;

	while (*str && !isspace(*str))
		str++;

	len = str - buf;
	if (!len)
		return -EINVAL;

	if (*str && *str != '\n') {
		ret = kstrtou64(skip_spaces(str), 10, &timeout_ns);
		if (ret)
			return -EINVAL;
	}

	mutex_lock(&wakelocks_lock);

	wl = wakelock_lookup_add(buf, len, true);
	if (IS_ERR(wl)) {
		ret = PTR_ERR(wl);
		goto out;
	}
	if (timeout_ns) {
		u64 timeout_ms = timeout_ns + NSEC_PER_MSEC - 1;

		do_div(timeout_ms, NSEC_PER_MSEC);
		__pm_wakeup_event(wl->ws, timeout_ms);
	} else {
		__pm_stay_awake(wl->ws);
		
#ifdef CONFIG_WAKELOCK_GOVERNOR
		if (governor_enabled) {
			gov_entry = kmalloc(sizeof(*gov_entry), GFP_KERNEL);
			if (gov_entry) {
				gov_entry->name = kstrndup(buf, len, GFP_KERNEL);
				if (!gov_entry->name) {
					kfree(gov_entry);
				} else {
					gov_entry->start_time = ktime_get();
					
					spin_lock_irqsave(&governor_lock, flags);
					list_add(&gov_entry->list, &governor_active_list);
					spin_unlock_irqrestore(&governor_lock, flags);
					
					if (!delayed_work_pending(&governor_work)) {
						schedule_delayed_work(&governor_work, 
							msecs_to_jiffies(1000));
					}
				}
			}
		}
#endif
	}

	wakelocks_lru_most_recent(wl);

 out:
	mutex_unlock(&wakelocks_lock);
	return ret;
}

int pm_wake_unlock(const char *buf)
{
	struct wakelock *wl;
	size_t len;
	int ret = 0;
#ifdef CONFIG_WAKELOCK_GOVERNOR
	unsigned long flags;
	struct wakelock_governor_entry *gov_entry, *tmp;
#endif

	if (!capable(CAP_BLOCK_SUSPEND))
		return -EPERM;

	len = strlen(buf);
	if (!len)
		return -EINVAL;

	if (buf[len-1] == '\n')
		len--;

	if (!len)
		return -EINVAL;

	mutex_lock(&wakelocks_lock);

	wl = wakelock_lookup_add(buf, len, false);
	if (IS_ERR(wl)) {
		ret = PTR_ERR(wl);
		goto out;
	}
	__pm_relax(wl->ws);
	
#ifdef CONFIG_WAKELOCK_GOVERNOR
	if (governor_enabled) {
		spin_lock_irqsave(&governor_lock, flags);
		list_for_each_entry_safe(gov_entry, tmp, &governor_active_list, list) {
			if (strcmp(gov_entry->name, buf) == 0) {
				list_del(&gov_entry->list);
				kfree(gov_entry->name);
				kfree(gov_entry);
				break;
			}
		}
		spin_unlock_irqrestore(&governor_lock, flags);
	}
#endif

	wakelocks_lru_most_recent(wl);
	wakelocks_gc();

 out:
	mutex_unlock(&wakelocks_lock);
	return ret;
}

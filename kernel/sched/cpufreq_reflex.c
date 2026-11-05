// SPDX-License-Identifier: GPL-2.0
/*
 * Reflex CPUFreq governor (based on schedutil)
 * Copyright (C) 2026 Masahito Suzuki
 *
 * schedutil, plus a measured fast path for the rising edge.
 *
 * schedutil ramps up slowly because a saturated CPU's frequency-
 * invariant utilization only shows the current capacity: the demand
 * above it is censored, and the 1.25x headroom ratchets the frequency
 * up at PELT's pace.  Reflex closes, on each CPU itself, intervals of
 * at least max(rate_limit_us, 1 tick) and measures over each one
 *
 *   D = busy * freq_scale * capacity		(PELT's units)
 *
 * from kcpustat's idle time.  An interval with any idle in it shows
 * the demand exactly; one with none is censored: the CPU is falling
 * behind and its demand is only known to be at least D.  The state per
 * CPU is H, the envelope of D, which follows a rise at once and decays
 * like PELT, and N, the running time seen lately, which decays like PELT
 * too.  The probability that the CPU is behind is
 *
 *   p = 1 after a censored interval, else alpha / (N + alpha)
 *
 * with alpha = tau_min of running as the prior that work is behind.  The
 * request is
 *
 *   util = max(1.25 * PELT, 1.25 * H)
 *   freq >= s*(p * lambda)
 *
 * where s*(l) = argmin_s (P(s) + l) / s.  s*(0) is the most efficient
 * speed.  P(s) comes from a cube law; lambda is the latency_weight
 * tunable, per mille of lambda_max, the least weight at which work
 * that is behind runs at the maximum frequency; that is the default.
 *
 * Backport of Reflex v0.4.0 to Linux 4.14: the Energy Model, CPPC,
 * uclamp and the adjust_perf path are not available here, so the speed
 * floors come from the cube law alone.
 */

#include "sched.h"

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/sched/cpufreq.h>
#include <linux/slab.h>
#include <linux/tick.h>

#include <uapi/linux/sched/types.h>

/**************************************************************
 * Version Information:
 */

#define CPUFREQ_REFLEX_PROGNAME "Reflex CPUFreq Governor"
#define CPUFREQ_REFLEX_AUTHOR   "Masahito Suzuki"

#define CPUFREQ_REFLEX_VERSION  "0.4.0"

#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)

/* s*(p * lambda) is tabulated at this many evenly spaced p */
#define RFX_FLOOR_N		65
/* latency_weight: lambda, per mille of lambda_max */
/*
 * sweet: policy0 is six CPUs at capacity 488 and tops out at 1.80 GHz;
 * policy6 tops out at 2.30 GHz. 550 puts the p=1 floor near 1.48/1.89
 * GHz, around where the device settles under load; 1000 would hold a
 * stalled six-CPU policy at fmax.
 */
#define RFX_LATENCY_WEIGHT_DEF	550
#define RFX_LATENCY_WEIGHT_LIM	100000

struct rfx_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		rate_limit_us;
	unsigned int		latency_weight;	/* lambda, per mille of lambda_max */
};

struct rfx_policy {
	struct cpufreq_policy	*policy;

	struct rfx_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;
	u64			last_freq_update_time;
	s64			freq_update_delay_ns;
	s64			sample_delay_ns;	/* tau_min */
	unsigned int		alpha_us;		/* tau_min, in us */
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;

	/* Speed floors, kHz: s*(p * lambda) at p = k / (RFX_FLOOR_N - 1) */
	unsigned int		floor[RFX_FLOOR_N];

	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;
};

struct rfx_cpu {
	struct update_util_data	update_util;
	struct rfx_policy	*rfx_policy;
	unsigned int		cpu;

	/* I/O wait boost (schedutil-compatible) */
	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	unsigned long		util;
	unsigned long		bw_min;
	unsigned long		bw_max;

	/* Measured demand: closed on this CPU only */
	u64			last_sample;	/* rq clock, ns */
	u64			last_wall;	/* kcpustat, us */
	u64			last_idle;	/* kcpustat, us */
	unsigned long		H;		/* envelope of measured demand */
	unsigned int		N;		/* running time seen lately, us */
	bool			censored;	/* last interval had no idle */
};

static DEFINE_PER_CPU(struct rfx_cpu, rfx_cpu);

/************************ Governor internals ***********************/

static void rfx_update_rate_limit_us(struct rfx_policy *rfx_pol)
{
	rfx_pol->freq_update_delay_ns =
		(s64)rfx_pol->tunables->rate_limit_us * NSEC_PER_USEC;

	/*
	 * The frequency scale only moves once a tick, and an interval of a
	 * few microseconds can be all busy by chance; shorter intervals than
	 * this tell nothing about falling behind.
	 */
	rfx_pol->sample_delay_ns = max_t(s64, rfx_pol->freq_update_delay_ns,
					 TICK_NSEC);
	rfx_pol->alpha_us = div_u64(rfx_pol->sample_delay_ns, NSEC_PER_USEC);
}

static bool rfx_should_update_freq(struct rfx_policy *rfx_pol, u64 time)
{
	s64 delta_ns;

	if (!cpufreq_can_do_remote_dvfs(rfx_pol->policy))
		return false;

	if (unlikely(READ_ONCE(rfx_pol->limits_changed))) {
		WRITE_ONCE(rfx_pol->limits_changed, false);
		rfx_pol->need_freq_update = true;
		smp_mb();
		return true;
	} else if (rfx_pol->need_freq_update) {
		return true;
	}

	delta_ns = time - rfx_pol->last_freq_update_time;

	return delta_ns >= rfx_pol->freq_update_delay_ns;
}

static bool rfx_update_next_freq(struct rfx_policy *rfx_pol, u64 time,
				 unsigned int next_freq)
{
	rfx_pol->need_freq_update = false;

	if (rfx_pol->next_freq == next_freq)
		return false;

	rfx_pol->next_freq = next_freq;
	rfx_pol->last_freq_update_time = time;

	return true;
}

static void rfx_deferred_update(struct rfx_policy *rfx_pol)
{
	if (!rfx_pol->work_in_progress) {
		rfx_pol->work_in_progress = true;
		irq_work_queue(&rfx_pol->irq_work);
	}
}

static inline unsigned long map_util_perf(unsigned long util)
{
	return util + (util >> 2);
}

static inline unsigned int rfx_map_util_freq(unsigned long util,
					     unsigned int freq,
					     unsigned long max)
{
	return (unsigned int)div64_u64((u64)freq * util, max);
}

static unsigned int rfx_capacity_ref_freq(struct cpufreq_policy *policy)
{
	if (arch_scale_freq_invariant())
		return policy->cpuinfo.max_freq;

	return policy->cur + (policy->cur >> 2);
}

/* @floor is a speed floor in kHz; resolving clamps it to the limits. */
static unsigned int rfx_get_next_freq(struct rfx_policy *rfx_pol,
				      unsigned long util, unsigned long max,
				      unsigned int floor)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int freq;

	freq = rfx_capacity_ref_freq(policy);
	freq = rfx_map_util_freq(util, freq, max);
	freq = max(freq, floor);

	if (freq == rfx_pol->cached_raw_freq && !rfx_pol->need_freq_update)
		return rfx_pol->next_freq;

	rfx_pol->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

/*
 * Get CPU utilization, with the DVFS headroom already applied and capped
 * by the CPU capacity (4.14 has no uclamp or DL bandwidth helpers).
 */
static void rfx_get_util(struct rfx_cpu *rfx_c, unsigned long boost)
{
	struct rq *rq = cpu_rq(rfx_c->cpu);
	unsigned long util, max_cap;

	max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);

	sched_avg_update(rq);
	util = boosted_cpu_util(rfx_c->cpu, NULL);

	rfx_c->bw_min = 0;
	rfx_c->bw_max = max_cap;
	rfx_c->util = min(map_util_perf(max(util, boost)), max_cap);
}

/************************ Speed floors ***********************/

/* Integer cube root, for the cube-law model; table rebuilds only. */
static u64 rfx_icbrt(u64 n)
{
	u64 lo = 0, hi = 1ULL << 21;	/* (2^21)^3 = 2^63 */

	while (lo < hi) {
		u64 mid = (lo + hi + 1) >> 1;

		if (mid * mid * mid <= n)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

/*
 * Without an Energy Model, a cube law: with P(s) = s^3 for s = f / fmax,
 * (s^3 + l) / s is least at s = cbrt(l / 2), so lambda_max = 2 and
 * s*(p * lambda) = cbrt(p * weight / 1000).
 */
static void rfx_cube_floors(struct rfx_policy *rfx_pol, unsigned int weight)
{
	unsigned int fmax = rfx_pol->policy->cpuinfo.max_freq;
	u64 den = 1000 * (RFX_FLOOR_N - 1);
	int k;

	for (k = 0; k < RFX_FLOOR_N; k++) {
		u64 num = (u64)weight * k;	/* s^3 = num / den */
		unsigned int freq = fmax;

		if (num < den)	/* < 2^16, so num << 48 fits */
			freq = ((u64)fmax *
				rfx_icbrt(div64_u64(num << 48, den))) >> 16;
		WRITE_ONCE(rfx_pol->floor[k], freq);
	}
}

static void rfx_speed_floors(struct rfx_policy *rfx_pol)
{
	unsigned int weight = READ_ONCE(rfx_pol->tunables->latency_weight);

	rfx_cube_floors(rfx_pol, weight);
}

/************************ Measured demand ***********************/

/*
 * Close the interval since the last sample, on this CPU only: kcpustat
 * of a remote CPU would be read with this CPU's clock.  An interval of
 * a nohz idle simply closes on the first callback after the wakeup.
 */
static void rfx_sample(struct rfx_cpu *rfx_c, u64 time)
{
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	u64 idle, wall, dw, di;

	if (rfx_c->cpu != smp_processor_id() ||
	    (s64)(time - rfx_c->last_sample) < rfx_pol->sample_delay_ns)
		return;

	/* io_busy = 0: a CPU waiting on I/O has nothing to run */
	idle = get_cpu_idle_time(rfx_c->cpu, &wall, 0);
	dw = wall - rfx_c->last_wall;
	if (!dw)
		return;
	di = idle > rfx_c->last_idle ? min(idle - rfx_c->last_idle, dw) : 0;

	/* D = busy * freq_scale * capacity, in PELT's units */
	rfx_c->H = max_t(unsigned long,
			 div64_u64((dw - di) *
				   arch_scale_freq_capacity(NULL, rfx_c->cpu) *
				   arch_scale_cpu_capacity(NULL, rfx_c->cpu),
				   dw << SCHED_CAPACITY_SHIFT),
			 cpufreq_pelt_decay(rfx_c->H, dw * NSEC_PER_USEC));

	/*
	 * N gains the running time and decays like PELT; the running is
	 * taken to come first, as when the CPU ends an interval in nohz idle.
	 */
	rfx_c->N = cpufreq_pelt_decay(rfx_c->N, dw * NSEC_PER_USEC) +
		   cpufreq_pelt_decay(dw - di, di * NSEC_PER_USEC);
	rfx_c->censored = !di;

	rfx_c->last_sample = time;
	rfx_c->last_wall = wall;
	rfx_c->last_idle = idle;
}

/*
 * An idle CPU needs no speed: a sibling's or remote CPU's measured state
 * counts only while it runs.  This CPU's own callback always counts: on
 * a wakeup it runs from the enqueue, while idle_cpu() still says idle,
 * and that is exactly when H must bridge the gap since the last burst.
 */
static inline bool rfx_active(struct rfx_cpu *rfx_c)
{
	return rfx_c->cpu == smp_processor_id() || !idle_cpu(rfx_c->cpu);
}

/* PELT's prediction and the measured envelope, both with headroom. */
static unsigned long rfx_util(struct rfx_cpu *rfx_c, unsigned long boost)
{
	unsigned long util;

	rfx_get_util(rfx_c, boost);
	util = max(rfx_c->util, boost);

	if (rfx_active(rfx_c))
		util = max(util, min(map_util_perf(rfx_c->H), rfx_c->bw_max));

	return util;
}

/*
 * The speed floor in kHz, s*(p * lambda), p = 1 after a censored
 * interval and alpha / (N + alpha) otherwise.
 */
static unsigned int rfx_floor(struct rfx_cpu *rfx_c, unsigned long max_cap)
{
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	unsigned int floor, k = RFX_FLOOR_N - 1;

	if (!rfx_active(rfx_c))
		return 0;

	if (!rfx_c->censored) {
		unsigned int alpha = rfx_pol->alpha_us, den = rfx_c->N + alpha;

		k = (k * alpha + den / 2) / den;
	}
	floor = READ_ONCE(rfx_pol->floor[k]);

	if (rfx_c->bw_max < max_cap)
		floor = min_t(unsigned int, floor,
			      rfx_map_util_freq(rfx_c->bw_max,
				rfx_capacity_ref_freq(rfx_pol->policy),
				max_cap));
	return floor;
}

/************************ I/O wait boost ***********************/

static bool rfx_iowait_reset(struct rfx_cpu *rfx_c, u64 time,
			     bool set_iowait_boost)
{
	s64 delta_ns = time - rfx_c->last_update;

	if (delta_ns <= TICK_NSEC)
		return false;

	rfx_c->iowait_boost = set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
	rfx_c->iowait_boost_pending = set_iowait_boost;

	return true;
}

static void rfx_iowait_boost(struct rfx_cpu *rfx_c, u64 time,
			     unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;

	if (rfx_c->iowait_boost &&
	    rfx_iowait_reset(rfx_c, time, set_iowait_boost))
		return;

	if (!set_iowait_boost)
		return;

	if (rfx_c->iowait_boost_pending)
		return;
	rfx_c->iowait_boost_pending = true;

	if (rfx_c->iowait_boost) {
		rfx_c->iowait_boost =
			min_t(unsigned int, rfx_c->iowait_boost << 1, SCHED_CAPACITY_SCALE);
		return;
	}

	rfx_c->iowait_boost = IOWAIT_BOOST_MIN;
}

static unsigned long rfx_iowait_apply(struct rfx_cpu *rfx_c, u64 time,
				      unsigned long max_cap)
{
	if (!rfx_c->iowait_boost)
		return 0;

	if (rfx_iowait_reset(rfx_c, time, false))
		return 0;

	if (!rfx_c->iowait_boost_pending) {
		rfx_c->iowait_boost >>= 1;
		if (rfx_c->iowait_boost < IOWAIT_BOOST_MIN) {
			rfx_c->iowait_boost = 0;
			return 0;
		}
	}

	rfx_c->iowait_boost_pending = false;

	return (rfx_c->iowait_boost * max_cap) >> SCHED_CAPACITY_SHIFT;
}

/************************ Core update callbacks ***********************/

static void rfx_update_single_freq(struct update_util_data *hook, u64 time,
				   unsigned int flags)
{
	struct rfx_cpu *rfx_c = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	unsigned long max_cap, boost, util;
	unsigned int next_f;

	max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);

	rfx_iowait_boost(rfx_c, time, flags);
	rfx_c->last_update = time;
	rfx_sample(rfx_c, time);

	if (!rfx_should_update_freq(rfx_pol, time))
		return;

	boost = rfx_iowait_apply(rfx_c, time, max_cap);
	util = rfx_util(rfx_c, boost);
	next_f = rfx_get_next_freq(rfx_pol, util, max_cap,
				   rfx_floor(rfx_c, max_cap));

	if (!rfx_update_next_freq(rfx_pol, time, next_f))
		return;

	if (rfx_pol->policy->fast_switch_enabled) {
		cpufreq_driver_fast_switch(rfx_pol->policy, next_f);
	} else {
		raw_spin_lock(&rfx_pol->update_lock);
		rfx_deferred_update(rfx_pol);
		raw_spin_unlock(&rfx_pol->update_lock);
	}
}

/************************ Shared policy support ***********************/

static unsigned int rfx_next_freq_shared(struct rfx_cpu *rfx_c, u64 time)
{
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned long util = 0, max_cap;
	unsigned int floor = 0, j;

	max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);

	for_each_cpu(j, policy->cpus) {
		struct rfx_cpu *j_rfx_c = &per_cpu(rfx_cpu, j);
		unsigned long j_boost;

		j_boost = rfx_iowait_apply(j_rfx_c, time, max_cap);
		util = max(util, rfx_util(j_rfx_c, j_boost));
		floor = max(floor, rfx_floor(j_rfx_c, max_cap));
	}

	return rfx_get_next_freq(rfx_pol, util, max_cap, floor);
}

static void
rfx_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct rfx_cpu *rfx_c = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	unsigned int next_f;

	raw_spin_lock(&rfx_pol->update_lock);

	rfx_iowait_boost(rfx_c, time, flags);
	rfx_c->last_update = time;
	rfx_sample(rfx_c, time);

	if (rfx_should_update_freq(rfx_pol, time)) {
		next_f = rfx_next_freq_shared(rfx_c, time);

		if (!rfx_update_next_freq(rfx_pol, time, next_f))
			goto unlock;

		if (rfx_pol->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(rfx_pol->policy, next_f);
		else
			rfx_deferred_update(rfx_pol);
	}
unlock:
	raw_spin_unlock(&rfx_pol->update_lock);
}

/************************ Kthread (slow path) ***********************/

static void rfx_work(struct kthread_work *work)
{
	struct rfx_policy *rfx_pol = container_of(work, struct rfx_policy, work);
	unsigned int freq;
	unsigned long flags;

	raw_spin_lock_irqsave(&rfx_pol->update_lock, flags);
	freq = rfx_pol->next_freq;
	rfx_pol->work_in_progress = false;
	raw_spin_unlock_irqrestore(&rfx_pol->update_lock, flags);

	mutex_lock(&rfx_pol->work_lock);
	__cpufreq_driver_target(rfx_pol->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&rfx_pol->work_lock);
}

static void rfx_irq_work(struct irq_work *irq_work)
{
	struct rfx_policy *rfx_pol;

	rfx_pol = container_of(irq_work, struct rfx_policy, irq_work);

	kthread_queue_work(&rfx_pol->worker, &rfx_pol->work);
}

/************************** sysfs interface ************************/

static struct rfx_tunables *rfx_global_tunables;
static DEFINE_MUTEX(rfx_global_tunables_lock);

static inline struct rfx_tunables *to_rfx_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct rfx_tunables, attr_set);
}

static ssize_t rfx_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->rate_limit_us);
}

static ssize_t
rfx_rate_limit_us_store(struct gov_attr_set *attr_set, const char *buf,
			size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->rate_limit_us = rate_limit_us;

	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_update_rate_limit_us(rfx_pol);

	return count;
}

static struct governor_attr rfx_rate_limit_us =
	__ATTR(rate_limit_us, 0644, rfx_rate_limit_us_show, rfx_rate_limit_us_store);

static ssize_t latency_weight_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_rfx_tunables(attr_set)->latency_weight);
}

static ssize_t latency_weight_store(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int weight;

	if (kstrtouint(buf, 10, &weight) || weight > RFX_LATENCY_WEIGHT_LIM)
		return -EINVAL;

	WRITE_ONCE(tunables->latency_weight, weight);

	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_speed_floors(rfx_pol);

	return count;
}

static struct governor_attr latency_weight = __ATTR_RW(latency_weight);

static ssize_t version_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%s\n", CPUFREQ_REFLEX_VERSION);
}
static struct governor_attr version = __ATTR_RO(version);

static struct attribute *rfx_attrs[] = {
	&version.attr,
	&rfx_rate_limit_us.attr,
	&latency_weight.attr,
	NULL
};

static void rfx_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);

	kfree(to_rfx_tunables(attr_set));
}

static struct kobj_type rfx_tunables_ktype = {
	.default_attrs = rfx_attrs,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &rfx_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor reflex_gov;

static struct rfx_policy *rfx_policy_alloc(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol;

	rfx_pol = kzalloc(sizeof(*rfx_pol), GFP_KERNEL);
	if (!rfx_pol)
		return NULL;

	rfx_pol->policy = policy;
	raw_spin_lock_init(&rfx_pol->update_lock);
	return rfx_pol;
}

static void rfx_policy_free(struct rfx_policy *rfx_pol)
{
	kfree(rfx_pol);
}

static int rfx_kthread_create(struct rfx_policy *rfx_pol)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= 0,
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		.sched_runtime	= NSEC_PER_MSEC,
		.sched_deadline = 10 * NSEC_PER_MSEC,
		.sched_period	= 10 * NSEC_PER_MSEC,
	};
	struct cpufreq_policy *policy = rfx_pol->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&rfx_pol->work, rfx_work);
	kthread_init_worker(&rfx_pol->worker);
	thread = kthread_create(kthread_worker_fn, &rfx_pol->worker,
				"rfxgov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("reflex: failed to create kthread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	rfx_pol->thread = thread;
	if (policy->dvfs_possible_from_any_cpu)
		set_cpus_allowed_ptr(thread, policy->related_cpus);
	else
		kthread_bind_mask(thread, policy->related_cpus);

	init_irq_work(&rfx_pol->irq_work, rfx_irq_work);
	mutex_init(&rfx_pol->work_lock);

	wake_up_process(thread);

	return 0;
}

static void rfx_kthread_stop(struct rfx_policy *rfx_pol)
{
	if (rfx_pol->policy->fast_switch_enabled)
		return;

	if (!rfx_pol->thread)
		return;

	kthread_flush_worker(&rfx_pol->worker);
	kthread_stop(rfx_pol->thread);
	mutex_destroy(&rfx_pol->work_lock);
}

static struct rfx_tunables *rfx_tunables_alloc(struct rfx_policy *rfx_pol)
{
	struct rfx_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &rfx_pol->tunables_hook);
		if (!have_governor_per_policy())
			rfx_global_tunables = tunables;
	}
	return tunables;
}

static void rfx_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		rfx_global_tunables = NULL;
}

static int rfx_init(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol;
	struct rfx_tunables *tunables;
	int ret = 0;

	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	rfx_pol = rfx_policy_alloc(policy);
	if (!rfx_pol) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = rfx_kthread_create(rfx_pol);
	if (ret)
		goto free_rfx_pol;

	mutex_lock(&rfx_global_tunables_lock);

	if (rfx_global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = rfx_pol;
		rfx_pol->tunables = rfx_global_tunables;

		gov_attr_set_get(&rfx_global_tunables->attr_set,
				 &rfx_pol->tunables_hook);
		goto out;
	}

	tunables = rfx_tunables_alloc(rfx_pol);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	/* Default tunable values */
	tunables->rate_limit_us = cpufreq_policy_transition_delay_us(policy);
	tunables->latency_weight = RFX_LATENCY_WEIGHT_DEF;

	policy->governor_data = rfx_pol;
	rfx_pol->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj,
				   &rfx_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   reflex_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&rfx_global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	rfx_clear_global_tunables();

stop_kthread:
	rfx_kthread_stop(rfx_pol);
	mutex_unlock(&rfx_global_tunables_lock);

free_rfx_pol:
	rfx_policy_free(rfx_pol);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("reflex: initialization failed (error %d)\n", ret);
	return ret;
}

static void rfx_exit(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	unsigned int count;

	mutex_lock(&rfx_global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &rfx_pol->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		rfx_clear_global_tunables();

	mutex_unlock(&rfx_global_tunables_lock);

	rfx_kthread_stop(rfx_pol);
	rfx_policy_free(rfx_pol);
	cpufreq_disable_fast_switch(policy);
}

static int rfx_start(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;

	rfx_update_rate_limit_us(rfx_pol);
	rfx_speed_floors(rfx_pol);
	rfx_pol->last_freq_update_time	= 0;
	rfx_pol->next_freq		= 0;
	rfx_pol->work_in_progress	= false;
	rfx_pol->limits_changed		= false;
	rfx_pol->cached_raw_freq	= 0;
	rfx_pol->need_freq_update	= false;

	if (policy_is_shared(policy))
		uu = rfx_update_shared;
	else
		uu = rfx_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		struct rfx_cpu *rfx_c = &per_cpu(rfx_cpu, cpu);

		memset(rfx_c, 0, sizeof(*rfx_c));
		rfx_c->cpu = cpu;
		rfx_c->rfx_policy = rfx_pol;
		rfx_c->last_idle = get_cpu_idle_time(cpu, &rfx_c->last_wall, 0);
	}

	/*
	 * Publish the hooks only after all per-CPU data is initialized, so a
	 * shared policy's rfx_update_shared() never reads an uninitialized
	 * sibling rfx_cpu.
	 */
	for_each_cpu(cpu, policy->cpus) {
		struct rfx_cpu *rfx_c = &per_cpu(rfx_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &rfx_c->update_util, uu);
	}

	return 0;
}

static void rfx_stop(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&rfx_pol->irq_work);
		kthread_cancel_work_sync(&rfx_pol->work);
	}
}

static void rfx_limits(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&rfx_pol->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&rfx_pol->work_lock);
	}

	/* cpuinfo.max_freq, which the cube-law floor scales, may move */
	rfx_speed_floors(rfx_pol);

	smp_wmb();

	WRITE_ONCE(rfx_pol->limits_changed, true);
}

static struct cpufreq_governor reflex_gov = {
	.name			= "reflex",
	.owner			= THIS_MODULE,
	.dynamic_switching	= true,
	.init			= rfx_init,
	.exit			= rfx_exit,
	.start			= rfx_start,
	.stop			= rfx_stop,
	.limits			= rfx_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_REFLEX
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &reflex_gov;
}
#endif

static int __init reflex_gov_init(void)
{
	pr_info("%s %s by %s\n", CPUFREQ_REFLEX_PROGNAME,
		CPUFREQ_REFLEX_VERSION, CPUFREQ_REFLEX_AUTHOR);
	return cpufreq_register_governor(&reflex_gov);
}

static void __exit reflex_gov_exit(void)
{
	cpufreq_unregister_governor(&reflex_gov);
}

module_init(reflex_gov_init);
module_exit(reflex_gov_exit);

MODULE_AUTHOR(CPUFREQ_REFLEX_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(CPUFREQ_REFLEX_PROGNAME);

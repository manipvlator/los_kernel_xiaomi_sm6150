// SPDX-License-Identifier: GPL-2.0
/*
 * Vorpal CPUFreq Governor - Perfect Gaming & Thermal Edition
 * Based on schedutil - optimized for 120fps gaming & daily use
 * Features: Activity State Machine, Smart Mode Detection, Hispeed Blending,
 *           Gaming Lock & Sustain, Thermal-Aware Cap, Burst Guard,
 *           Aggressive Idle, Game Launch Boost, Universal SoC Support,
 *           Adaptive ROM Detection, Proactive Thermal Control
 * Author: Templar Dev (Steambot12)
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/topology.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <uapi/linux/sched/types.h>
#include <asm/topology.h>
#include <linux/arch_topology.h>

extern int vm_swappiness;
extern unsigned int sysctl_sched_latency;

#define CPUFREQ_VORPAL_PROGNAME     "Vorpal CPUFreq Governor"
#define CPUFREQ_VORPAL_AUTHOR       "Templar Dev"
#define CPUFREQ_VORPAL_VERSION 		"1.0"

/* === RATE LIMITS === */

/* UI Animation protection */
#define RFX_UI_IDLE_PROTECTION_NS   (25 * NSEC_PER_MSEC)
#define RFX_LITTLE_INTERACTIVE_FLOOR_KHZ  768000
#define RFX_GAME_LAUNCH_BOOST_NS   (15000 * NSEC_PER_MSEC)

/* BIG cluster rate limits */
#define CPUFREQ_VORPAL_BIG_UP_RATE_LIMIT_US      0
#define CPUFREQ_VORPAL_BIG_DOWN_RATE_LIMIT_US    8000

/* Default: Ultra-fast 10us for instant response */
#define CPUFREQ_VORPAL_DEFAULT_RATE_LIMIT_US        10
#define CPUFREQ_VORPAL_UP_RATE_LIMIT_US             0

/* BIG: balance between gaming stability and battery */
#define CPUFREQ_VORPAL_DOWN_RATE_LIMIT_US    8000

/* LITTLE: AGGRESSIVE IDLE - Fix stuck issue */
#define CPUFREQ_VORPAL_LITTLE_UP_RATE_LIMIT_US      0
#define CPUFREQ_VORPAL_LITTLE_DOWN_RATE_LIMIT_US  4000

/* Per-state LITTLE down delays - EXPERT IDLE TUNING */
#define RFX_LITTLE_DOWN_HEAVY_US    20000
#define RFX_LITTLE_DOWN_MEDIUM_US    4000
#define RFX_LITTLE_DOWN_LIGHT_US       50

/* PRIME: Faster down after gaming for thermal - TUNED */
#define CPUFREQ_VORPAL_PRIME_UP_RATE_LIMIT_US       0
#define CPUFREQ_VORPAL_PRIME_DOWN_RATE_LIMIT_US   8000
#define CPUFREQ_VORPAL_PRIME_RATE_LIMIT_US          1

/* === HISPEED / BLEND - THERMAL AWARE === */

#define CPUFREQ_VORPAL_DEFAULT_HISPEED_WINDOW_US    30
#define CPUFREQ_VORPAL_DEFAULT_HISPEED_FILTER_SHIFT 0

/* Hispeed: Thermal vs Performance Balance - TUNED */
#define CPUFREQ_VORPAL_DEFAULT_HISPEED_BOOST_PCT   72
#define RFX_HISPEED_GAMING_PCT                     85
#define RFX_HISPEED_DAILY_PCT                      55
#define RFX_HISPEED_VIDEO_PCT                      72

#define SCHED_FLAGS_UGOV    0x10000000
#define IOWAIT_BOOST_MIN    (SCHED_CAPACITY_SCALE / 8)

/* Half-life: Fast decay for battery */
#define HISPEED_HALFLIFE_NS    (6 * NSEC_PER_MSEC)
#define HISPEED_HALFLIFE_MAX                       8

/* === BURST GUARD - GAMING OPTIMIZED === */

#define RFX_BURST_GUARD_NS    (250 * NSEC_PER_MSEC)
#define RFX_BURST_DROP_THRESHOLD                  12

/* === HEAVY SUSTAIN - THERMAL GAMING === */

#define RFX_SUSTAIN_HEAVY_ENTER_PCT   35
#define RFX_SUSTAIN_HEAVY_EXIT_PCT    20
#define RFX_SUSTAIN_HEAVY_BUSY_PCT     8
#define RFX_SUSTAIN_HEAVY_TICKS        1
#define RFX_SUSTAIN_EXIT_TICKS         8

/* TUNED: Shorter gaming lock for thermal balance */
#define RFX_GAMING_LOCK_DURATION_NS   (12000 * NSEC_PER_MSEC)
#define RFX_GAMING_TUNABLE_SUSTAIN_NS  (20000 * NSEC_PER_MSEC)

/* Adaptive Gaming — persentase from max freq hardware */
#define RFX_GAMING_MAX_PCT              90
#define RFX_BIG_GAMING_MAX_PCT          88
#define RFX_PRIME_GAMING_FLOOR_PCT      82
#define RFX_GAME_LAUNCH_FLOOR_PCT       65
#define RFX_BIG_INTERACTIVE_FLOOR_PCT   15
#define RFX_LITTLE_GAMING_CAP_PCT       85


/* === LIGHT MODE - AGGRESSIVE IDLE === */

#define RFX_LIGHT_ENTER_PCT     3
#define RFX_LIGHT_ENTER_TICKS   3
#define RFX_LIGHT_EXIT_PCT     12

/* === IDLE & DEEPSLEEP - <1% DRAIN TARGET === */

#define RFX_IDLE_STALE_NS      (30 * NSEC_PER_MSEC)
#define RFX_FORCE_IDLE_THRESHOLD_NS   (15 * NSEC_PER_USEC)
#define RFX_IDLE_HYSTERESIS_NS (25 * NSEC_PER_MSEC)

/* === FREQUENCY FLOORS & CAPS - IDLE FIX === */

/* LITTLE max cap non-gaming: 1.0GHz (save battery) */
#define RFX_LITTLE_MAX_NON_GAMING_KHZ  960000
#define RFX_FG_LIGHT_BIG_CAP_KHZ      500000

#define RFX_INTERACTIVE_UTIL_PCT      1

/* LITTLE floor: 300MHz (REAL IDLE) */
#define RFX_INTERACTIVE_FLOOR_KHZ    300000

/* BIG/PRIME floors - COOLER */
#define RFX_BIG_INTERACTIVE_FLOOR_KHZ  600000

/* === TIME-BASED DUTY CYCLE THERMAL — No arch_scale dependency === */

#define RFX_THERMAL_WINDOW_NS            (14000 * NSEC_PER_MSEC)
#define RFX_THERMAL_WINDOW_SHRINK_NS     (11000 * NSEC_PER_MSEC)
#define RFX_THERMAL_THROTTLE_BURST_NS    (800  * NSEC_PER_MSEC)
#define RFX_THERMAL_THROTTLE_CAP_PCT     86
#define RFX_BIG_THERMAL_THROTTLE_CAP_PCT    90
#define RFX_PRIME_GAMING_SUSTAIN_FLOOR_PCT  75

/* Extended interactive - shorter */
#define RFX_INTERACTIVE_DURATION_NS  (3000 * NSEC_PER_MSEC)
#define RFX_VIDEO_DETECT_THRESHOLD_NS (200 * NSEC_PER_MSEC)
#define RFX_IDLE_DEEP_CAP_KHZ_FALLBACK  300000

/* === CLUSTER THRESHOLDS === */

#define RFX_LITTLE_CAP_THRESHOLD    614
#define RFX_PRIME_CAP_THRESHOLD     1000
#define RFX_BIG_DROP_PCT            13

/* === ACTIVITY STATE MACHINE - FAST IDLE === */

#define RFX_ACT_UP_TICKS    1
#define RFX_ACT_DOWN_TICKS  1

#define RFX_ACT_IDLE_TO_LIGHT_PCT  4
#define RFX_ACT_LIGHT_TO_MED_PCT  20
#define RFX_ACT_MED_TO_HEAVY_PCT  40
#define RFX_ACT_HEAVY_TO_MED_PCT  22
#define RFX_ACT_MED_TO_LIGHT_PCT   6
#define RFX_ACT_LIGHT_TO_IDLE_PCT  3

/* LITTLE freq caps - STRICT IDLE */
#define RFX_LITTLE_LIGHT_MAX_FREQ_KHZ   400000
#define RFX_LITTLE_MED_MAX_FREQ_KHZ     800000

/* === ADAPTIVE FREQ HELPERS - UNIVERSAL SoC SUPPORT === */

static inline unsigned int rfx_adaptive_max(struct cpufreq_policy *policy,
                                             unsigned int pct)
{
    if (!policy || !policy->cpuinfo.max_freq)
        return 0;
    return (unsigned int)((u64)policy->cpuinfo.max_freq * pct / 100);
}

static inline unsigned int rfx_adaptive_floor(struct cpufreq_policy *policy,
                                               unsigned int pct)
{
    unsigned int floor;
    if (!policy || !policy->cpuinfo.max_freq)
        return 0;
    floor = (unsigned int)((u64)policy->cpuinfo.max_freq * pct / 100);
    return max(floor, policy->cpuinfo.min_freq);
}

/* === DATA STRUCTURES === */

enum rfx_cluster_type {
	RFX_CLUSTER_LITTLE = 0,
	RFX_CLUSTER_BIG    = 1,
	RFX_CLUSTER_PRIME  = 2,
};

enum rfx_activity_state {
	RFX_ACT_IDLE   = 0,
	RFX_ACT_LIGHT  = 1,
	RFX_ACT_MEDIUM = 2,
	RFX_ACT_HEAVY  = 3,
};

enum rfx_mode {
	RFX_MODE_NORMAL = 0,
	RFX_MODE_GAMING = 1,
	RFX_MODE_VIDEO  = 2,
};

static inline enum rfx_cluster_type rfx_get_cluster_type(unsigned int cpu)
{
	unsigned long cap = arch_scale_cpu_capacity(NULL, cpu);
	if (cap <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD)
		return RFX_CLUSTER_LITTLE;
	if (cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD)
		return RFX_CLUSTER_PRIME;
	return RFX_CLUSTER_BIG;
}

static inline bool rfx_is_little(unsigned int cpu)
{
	return arch_scale_cpu_capacity(NULL, cpu) <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD;
}

static inline bool rfx_is_prime(unsigned int cpu)
{
	return arch_scale_cpu_capacity(NULL, cpu) >= (unsigned long)RFX_PRIME_CAP_THRESHOLD;
}

extern void rfx_get_util_gki510(int cpu, unsigned long boost,
				unsigned long *util, unsigned long *bwmin);
extern bool rfx_dl_bw_exceeded_gki510(int cpu, unsigned long bwmin);

static inline bool rfx_driver_test_flags(unsigned int flags) { return false; }
static inline bool rfx_scx_switched_all(void) { return false; }
static inline bool rfx_cpu_uclamp_capped(unsigned int cpu) { return false; }

static inline unsigned int rfx_get_ref_freq(struct cpufreq_policy *policy)
{
	if (!policy)
		return 0;
	return policy->cpuinfo.max_freq;
}

static inline struct gov_attr_set *rfx_to_gov_attr_set(struct kobject *kobj)
{
	return container_of(kobj, struct gov_attr_set, kobj);
}

struct rfx_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		rate_limit_us;
	unsigned int		up_rate_limit_us;
	unsigned int		down_rate_limit_us;
	unsigned int		hispeed_window_us;
	unsigned int		hispeed_filter_shift;
	unsigned int		hispeed_boost_pct;
	enum rfx_cluster_type	cluster_type;
	unsigned int		gaming_mode;
};

struct rfx_policy;

static void rfx_deferred_update(struct rfx_policy *rfx_pol);
static void rfx_work(struct kthread_work *work);

struct rfx_policy {
	struct cpufreq_policy	*policy;
	struct rfx_tunables	*tunables;
	struct list_head	tunables_hook;
	raw_spinlock_t		update_lock;
	u64			last_upfreq_time;
	u64			last_downfreq_time;
	s64			freq_update_delay_ns;
	s64			up_rate_delay_ns;
	s64			down_rate_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;
	struct irq_work		irq_work;
	struct kthread_work	work;
	struct mutex		work_lock;
	struct kthread_worker	worker;
	struct task_struct	*thread;
	bool			work_in_progress;
	bool			limits_changed;
	bool			need_freq_update;
	u8			sustain_heavy_ticks;
	u8			sustain_exit_ticks;
	u8			light_enter_ticks;
	bool			in_heavy_mode;
	bool			in_light_mode;
	bool			prev_heavy_mode;
	u64			guard_end_ns;
	unsigned int		guard_freq_khz;
	u64			interactive_end_ns;
	bool			prime_gaming_floor_active;
	u64			prime_gaming_floor_end_ns;
	bool			force_idle;
	u64			force_idle_start_ns;
	u64			last_real_update_ns;
	enum rfx_mode		current_mode;
	u64			mode_switch_time_ns;
	u64			gaming_lock_end_ns;
	bool			video_pattern_detected;
	u64			video_detect_start_ns;
	u64			idle_entry_time_ns;
	bool			in_deep_idle;
	u64   		game_launch_end_ns;
	bool  			game_launching;
	/* Time-based duty cycle thermal */
	u64             thermal_duty_window_start_ns;
	u64             thermal_throttle_end_ns;
	bool            thermal_throttle_active;
	unsigned int    thermal_sustain_window_count;
	u64             thermal_duty_last_active_ns;
	u64			render_boost_end_ns;
	bool			render_urgency_active;
	/* Auto ROM detection - set at init, NOT user-settable */
	u8			rom_tweak_detected;   /* 0=stock, 1=light, 2=heavy */
	bool			rom_override_active;
};

struct rfx_cpu {
	struct update_util_data	update_util;
	struct rfx_policy	*rfx_policy;
	unsigned int		cpu;
	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;
	unsigned long		util;
	unsigned long		bwmin;
	u64			prev_idle_time;
	u64			prev_wall_time;
	unsigned int		busy_pct;
	unsigned int		filtered_busy_pct;
	bool			hispeed_active;
	u64			hispeed_start_ns;
	unsigned int		hispeed_idle_windows;
	enum rfx_activity_state	act_state;
	unsigned int		act_up_ticks;
	unsigned int		act_down_ticks;
	unsigned int		prev_util_pct;
	unsigned int		util_history[8];
	u8			util_history_idx;
	bool			is_video_pattern;
	bool			is_gaming_pattern;
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long		saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct rfx_cpu, rfx_cpu);

static inline struct rfx_tunables *to_rfx_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct rfx_tunables, attr_set);
}

/* === MODE DETECTION - SMART TASK DETECT - TUNED === */

static void rfx_detect_mode(struct rfx_policy *rfx_pol, struct rfx_cpu *rfx_c,
			     unsigned long util, unsigned long max_cap, u64 time)
{
	unsigned int util_pct = max_cap ? util * 100 / max_cap : 0;
	bool heavy_load = util_pct >= 45;
	bool medium_load = util_pct >= 18 && util_pct < 55;
	bool periodic_pattern = false;
	bool not_in_gaming;
	u64 time_in_mode;
	unsigned int variance = 0, avg = 0;

	int i;

	/* Update util history */
	rfx_c->util_history[rfx_c->util_history_idx] = util_pct;
	rfx_c->util_history_idx = (rfx_c->util_history_idx + 1) & 0x7;

	/* Detect video pattern - periodic medium load */
	if (medium_load) {
		for (i = 0; i < 8; i++)
			avg += rfx_c->util_history[i];
		avg /= 8;
		for (i = 0; i < 8; i++) {
			int diff = (int)rfx_c->util_history[i] - avg;
			variance += diff < 0 ? -diff : diff;
		}
		if (variance < 20 && avg >= 15 && avg <= 55)
			periodic_pattern = true;
	}

	/* Gaming mode tunable - user explicitly set via sysfs */
	if (rfx_pol->tunables->gaming_mode) {
		rfx_pol->current_mode       = RFX_MODE_GAMING;
		rfx_pol->in_light_mode      = false;
		rfx_pol->force_idle         = false;
		rfx_pol->sustain_exit_ticks = 0;

		if (util_pct >= 5) {
			rfx_pol->in_heavy_mode      = true;
			rfx_pol->gaming_lock_end_ns = time + RFX_GAMING_LOCK_DURATION_NS;
		} else if (rfx_pol->gaming_lock_end_ns &&
			   time < rfx_pol->gaming_lock_end_ns) {
			rfx_pol->in_heavy_mode      = true;
			rfx_pol->sustain_exit_ticks = 0;
		} else {
			/* Extend hysteresis to 6s to prevent freq bounce after lock expires */
			if (!rfx_pol->gaming_lock_end_ns ||
			    (time - rfx_pol->gaming_lock_end_ns) > (6000 * NSEC_PER_MSEC))
				rfx_pol->in_heavy_mode = false;
		}

		if (rfx_pol->policy) {
			unsigned long cap = arch_scale_cpu_capacity(NULL,
				cpumask_first(rfx_pol->policy->cpus));
			if (cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD) {
				rfx_pol->prime_gaming_floor_active = true;
				rfx_pol->prime_gaming_floor_end_ns = 0;
			}
		}
		return;
	}

	/* Gaming lock check */
	if (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns) {
		rfx_pol->current_mode = RFX_MODE_GAMING;
		return;
	}

		time_in_mode = time - rfx_pol->mode_switch_time_ns;

	if (heavy_load && rfx_c->act_state == RFX_ACT_HEAVY) {
		if (rfx_pol->current_mode != RFX_MODE_GAMING) {
			if (time_in_mode > 24 * NSEC_PER_MSEC) {
				rfx_pol->current_mode = RFX_MODE_GAMING;
				rfx_pol->mode_switch_time_ns = time;
				rfx_pol->gaming_lock_end_ns = time + RFX_GAMING_LOCK_DURATION_NS;
			}
		}
	} else if (periodic_pattern && !heavy_load) {
	not_in_gaming = (rfx_pol->current_mode != RFX_MODE_GAMING) &&
                    !(rfx_pol->gaming_lock_end_ns &&
	time < rfx_pol->gaming_lock_end_ns);
	if (not_in_gaming && rfx_pol->current_mode != RFX_MODE_VIDEO) {
	if (time_in_mode > RFX_VIDEO_DETECT_THRESHOLD_NS) {
	rfx_pol->current_mode = RFX_MODE_VIDEO;
	rfx_pol->mode_switch_time_ns = time;
	}
	}
	} else if (util_pct < 6 && rfx_pol->in_light_mode) {
		if (rfx_pol->current_mode != RFX_MODE_NORMAL) {
			if (time_in_mode > 32 * NSEC_PER_MSEC) {
				rfx_pol->current_mode = RFX_MODE_NORMAL;
				rfx_pol->mode_switch_time_ns = time;
				rfx_pol->gaming_lock_end_ns = 0;
				rfx_pol->thermal_duty_window_start_ns = 0;
				rfx_pol->thermal_throttle_active = false;
				rfx_pol->thermal_throttle_end_ns = 0;
				rfx_pol->thermal_sustain_window_count = 0;
			}
		}
	}
}

/* Get adaptive hispeed pct */
static inline unsigned int rfx_get_hispeed_pct(struct rfx_policy *rfx_pol)
{
	switch (rfx_pol->current_mode) {
	case RFX_MODE_GAMING:
		return RFX_HISPEED_GAMING_PCT;
	case RFX_MODE_VIDEO:
		return RFX_HISPEED_VIDEO_PCT;
	case RFX_MODE_NORMAL:
	default:
        return CPUFREQ_VORPAL_DEFAULT_HISPEED_BOOST_PCT;
    }
}

/* === ACTIVITY STATE UPDATE - AGGRESSIVE IDLE === */

static bool rfx_act_update(struct rfx_cpu *rfx_c, unsigned long effective_util,
			   unsigned long max_cap, u64 time,
			   unsigned int *freq_cap_khz)
{
	unsigned long idle_th, light_up_th, med_up_th;
	unsigned long heavy_dn_th, med_dn_th, light_dn_th;
	bool force_down = false;
	s64 stale_delta;
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	s64 time_since_last_update;
	bool is_little = (max_cap <= RFX_LITTLE_CAP_THRESHOLD);

	/* Enhanced idle detection */
	if (rfx_c->last_update) {
		time_since_last_update = (s64)(time - rfx_c->last_update);
		if (time_since_last_update >= (s64)RFX_IDLE_STALE_NS) {
			rfx_pol->force_idle = true;
			rfx_pol->force_idle_start_ns = time;
			rfx_pol->last_real_update_ns = time;
			if (!rfx_pol->in_deep_idle) {
				rfx_pol->idle_entry_time_ns = time;
				rfx_pol->in_deep_idle = true;
			}
		} else if (rfx_pol->force_idle) {
			if (effective_util > 0 || rfx_c->filtered_busy_pct > 0) {
				rfx_pol->force_idle = false;
				rfx_pol->in_deep_idle = false;
			} else if (time - rfx_pol->force_idle_start_ns > (8 * NSEC_PER_MSEC)) {
				rfx_pol->force_idle_start_ns = time;
			}
		}
	}

	/* Gaming/benchmark mode: bypass throttling */
	if (rfx_pol->in_heavy_mode ||
	    (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns)) {
		rfx_c->act_state      = RFX_ACT_HEAVY;
		rfx_c->act_up_ticks   = 0;
		rfx_c->act_down_ticks = 0;
		rfx_pol->force_idle = false;
		rfx_pol->in_deep_idle = false;
		rfx_pol->last_real_update_ns = time;
		*freq_cap_khz = 0;
		return false;
	}

	/* Light/Idle mode: strict battery saving */
	if (rfx_pol->in_light_mode || rfx_pol->force_idle) {
		if (is_little) {
			if (rfx_pol->force_idle) {
				*freq_cap_khz = rfx_pol->policy->cpuinfo.min_freq;
				return true;
			}
			*freq_cap_khz = RFX_LITTLE_LIGHT_MAX_FREQ_KHZ;
			return true;
		} else {
			*freq_cap_khz = RFX_FG_LIGHT_BIG_CAP_KHZ;
			return false;
		}
	}

	if (!is_little) {
		*freq_cap_khz = 0;
		return false;
	}

	if (rfx_c->last_update && !rfx_pol->force_idle) {
	stale_delta = (s64)(time - rfx_c->last_update);
	if (stale_delta >= (s64)RFX_IDLE_STALE_NS) {
	if (rfx_pol->interactive_end_ns && time < rfx_pol->interactive_end_ns) {
	*freq_cap_khz = RFX_LITTLE_INTERACTIVE_FLOOR_KHZ;
	return false;
	}
	rfx_c->act_state            = RFX_ACT_IDLE;
	rfx_c->act_up_ticks         = 0;
	rfx_c->act_down_ticks       = 0;
	rfx_c->filtered_busy_pct    = 0;
	rfx_c->hispeed_start_ns     = 0;
	rfx_c->hispeed_idle_windows = 0;
	rfx_pol->force_idle         = true;
	rfx_pol->in_deep_idle       = true;
	rfx_pol->idle_entry_time_ns = time;
	*freq_cap_khz = rfx_pol->policy->cpuinfo.min_freq;
	return true;
	}
	}

	/* Zero util: immediate drop */
	if (effective_util == 0) {
		switch (rfx_c->act_state) {
		case RFX_ACT_IDLE:
		case RFX_ACT_LIGHT:
			*freq_cap_khz = rfx_pol->policy->cpuinfo.min_freq;
			return true;
		case RFX_ACT_MEDIUM:
			*freq_cap_khz = RFX_LITTLE_MED_MAX_FREQ_KHZ;
			return true;
		case RFX_ACT_HEAVY:
			*freq_cap_khz = 0;
			return false;
		}
	}

	rfx_pol->last_real_update_ns = time;
	rfx_pol->in_deep_idle = false;

	/* Thresholds - More aggressive idle */
	idle_th      = max_cap * RFX_ACT_IDLE_TO_LIGHT_PCT / 100;
	light_up_th  = max_cap * RFX_ACT_LIGHT_TO_MED_PCT  / 100;
	med_up_th    = max_cap * RFX_ACT_MED_TO_HEAVY_PCT  / 100;
	heavy_dn_th  = max_cap * RFX_ACT_HEAVY_TO_MED_PCT  / 100;
	med_dn_th    = max_cap * RFX_ACT_MED_TO_LIGHT_PCT  / 100;
	light_dn_th  = max_cap * RFX_ACT_LIGHT_TO_IDLE_PCT / 100;

	/* State machine */
	if (effective_util > med_up_th && rfx_c->act_state != RFX_ACT_HEAVY) {
		rfx_c->act_up_ticks++;
		if (rfx_c->act_up_ticks >= RFX_ACT_UP_TICKS) {
			rfx_c->act_state      = RFX_ACT_HEAVY;
			rfx_c->act_up_ticks   = 0;
			rfx_c->act_down_ticks = 0;
		}
		*freq_cap_khz = 0;
		return false;
	}

	switch (rfx_c->act_state) {
	case RFX_ACT_IDLE:
		if (effective_util > idle_th) {
			rfx_c->act_up_ticks++;
			if (rfx_c->act_up_ticks >= RFX_ACT_UP_TICKS) {
				rfx_c->act_state    = RFX_ACT_LIGHT;
				rfx_c->act_up_ticks = 0;
			}
		}
		*freq_cap_khz = rfx_pol->policy->cpuinfo.min_freq;
		break;

	case RFX_ACT_LIGHT:
		if (effective_util > light_up_th) {
			rfx_c->act_up_ticks++;
			if (rfx_c->act_up_ticks >= RFX_ACT_UP_TICKS) {
				rfx_c->act_state    = RFX_ACT_MEDIUM;
				rfx_c->act_up_ticks = 0;
			}
		} else if (effective_util < light_dn_th) {
			rfx_c->act_down_ticks++;
			if (rfx_c->act_down_ticks >= RFX_ACT_DOWN_TICKS) {
				rfx_c->act_state      = RFX_ACT_IDLE;
				rfx_c->act_down_ticks = 0;
				force_down = true;
			}
		}
		*freq_cap_khz = RFX_LITTLE_LIGHT_MAX_FREQ_KHZ;
		break;

	case RFX_ACT_MEDIUM:
		if (effective_util > med_up_th) {
			rfx_c->act_up_ticks++;
			if (rfx_c->act_up_ticks >= RFX_ACT_UP_TICKS) {
				rfx_c->act_state    = RFX_ACT_HEAVY;
				rfx_c->act_up_ticks = 0;
			}
		} else if (effective_util < med_dn_th) {
			rfx_c->act_down_ticks++;
			if (rfx_c->act_down_ticks >= RFX_ACT_DOWN_TICKS) {
				rfx_c->act_state      = RFX_ACT_LIGHT;
				rfx_c->act_down_ticks = 0;
			}
		}
		*freq_cap_khz = RFX_LITTLE_MED_MAX_FREQ_KHZ;
		break;

	case RFX_ACT_HEAVY:
		if (effective_util < heavy_dn_th) {
			rfx_c->act_down_ticks++;
			if (rfx_c->act_down_ticks >= RFX_ACT_DOWN_TICKS) {
				rfx_c->act_state      = RFX_ACT_MEDIUM;
				rfx_c->act_down_ticks = 0;
			}
		}
		force_down    = false;
		*freq_cap_khz = RFX_LITTLE_MAX_NON_GAMING_KHZ;
		break;
	}

	return force_down;
}

/* === UTILITY FUNCTIONS === */

static unsigned int rfx_get_adaptive_shift(unsigned long util,
					   unsigned long max_cap,
					   unsigned int base_shift)
{
	unsigned int util_pct;

	if (!max_cap)
		return base_shift > 0 ? min(base_shift + 3, 12U) : 0;

	util_pct = (unsigned int)(util * 100 / max_cap);

	if (util_pct > 90)
		return 0;
	if (util_pct < 5)  /* Lower threshold */
		return base_shift > 0 ? min(base_shift + 3, 12U) : 0;
	return min(base_shift, 12U);
}

static void rfx_thermal_duty_cycle(struct rfx_policy *rfx_pol, u64 time)
{
	u64 time_since_gaming;
	u64 window_elapsed;
	u64 effective_window;

	if (!rfx_pol->in_heavy_mode ||
	    rfx_pol->current_mode != RFX_MODE_GAMING)
		return;

	if (!rfx_pol->thermal_duty_window_start_ns)
		rfx_pol->thermal_duty_window_start_ns = time;

	time_since_gaming = time - rfx_pol->mode_switch_time_ns;

	if (time_since_gaming < (3000 * NSEC_PER_MSEC)) {
	rfx_pol->thermal_throttle_active = false;
	return;
	}

	if (rfx_pol->thermal_throttle_active) {
		if (time >= rfx_pol->thermal_throttle_end_ns) {
			rfx_pol->thermal_throttle_active = false;
			rfx_pol->thermal_duty_window_start_ns = time;
			rfx_pol->thermal_sustain_window_count++;
			if (rfx_pol->thermal_sustain_window_count > 6)
	rfx_pol->thermal_sustain_window_count = 6;
			rfx_pol->thermal_duty_last_active_ns = time;
		}
		return;
	}

	effective_window = (rfx_pol->thermal_sustain_window_count >= 3)
		? RFX_THERMAL_WINDOW_SHRINK_NS
		: RFX_THERMAL_WINDOW_NS;

	window_elapsed = time - rfx_pol->thermal_duty_window_start_ns;

	if (window_elapsed >= effective_window) {
		rfx_pol->thermal_throttle_active    = true;
		rfx_pol->thermal_throttle_end_ns    = time + RFX_THERMAL_THROTTLE_BURST_NS;
	}
}

static unsigned long rfx_apply_headroom(unsigned long util,
					unsigned long max_cap,
					bool is_heavy,
					enum rfx_mode mode)
{
	unsigned int util_pct;
	unsigned long result;
	bool is_prime;
	unsigned int headroom_pct;

	if (!max_cap)
    return util;
	result   = min(util, max_cap);
	util_pct = (unsigned int)(util * 100 / max_cap);

	if (util_pct >= 98)
		return max_cap;

	is_prime = (max_cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD);

    if (mode == RFX_MODE_GAMING) {
	if (is_prime)
	headroom_pct = is_heavy ? 30 : 22;
	else
	headroom_pct = is_heavy ? 30 : 24;
	return min(util + util * headroom_pct / 100, max_cap);
	}

	/* Video mode */
	if (mode == RFX_MODE_VIDEO) {
		if (is_prime) {
			if (util_pct >= 50)
				return min(util + util * 22 / 100, max_cap);
			return min(util + util * 15 / 100, max_cap);
		}
		if (util_pct >= 65)
			return min(util + (util >> 3), max_cap);
		if (util_pct >= 40)
			return min(util + (util >> 4), max_cap);
		return min(util + (util >> 5), max_cap);
	}

	/* Normal mode: conservative */
	if (max_cap <= RFX_LITTLE_CAP_THRESHOLD) {
		if (util_pct >= 70)
			return min(util + (util >> 4), max_cap);
		if (util_pct >= 45)
			return min(util + (util >> 5), max_cap);
		return util;
	}

	/* BIG/PRIME normal */
	if (util_pct >= 75)
		return min(util + (util >> 4), max_cap);
	if (util_pct >= 50)
		return min(util + (util >> 5), max_cap);
	return min(util + (util >> 6), max_cap);
}

static bool rfx_should_update_freq(struct rfx_policy *rfx_pol, u64 time)
{
    s64 delta_ns;
    s64 effective_delay;
    bool going_up;

    if (!rfx_pol || !rfx_pol->policy)
        return false;

    if (cpumask_first(rfx_pol->policy->cpus) != smp_processor_id())
        return false;

    if (unlikely(READ_ONCE(rfx_pol->limits_changed))) {
        WRITE_ONCE(rfx_pol->limits_changed, false);
        rfx_pol->need_freq_update = true;
        smp_mb();
        return true;
    } else if (rfx_pol->need_freq_update) {
        return true;
    }

    going_up = (rfx_pol->next_freq > rfx_pol->policy->cur);

    /* Mode-aware rate limiting */
    if (rfx_pol->force_idle) {
        effective_delay = 3 * NSEC_PER_USEC;
    } else if (rfx_pol->in_heavy_mode ||
               rfx_pol->current_mode == RFX_MODE_GAMING ||
               (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns)) {
        if (going_up) {
            effective_delay = 0;
        } else if (rfx_pol->thermal_throttle_active) {
            effective_delay = 6000 * NSEC_PER_USEC;
        } else {
            effective_delay = 22000 * NSEC_PER_USEC;
        }
    } else if (rfx_pol->current_mode == RFX_MODE_VIDEO) {
        effective_delay = 25 * NSEC_PER_USEC;
    } else {
        effective_delay = rfx_pol->freq_update_delay_ns;
    }

    if (going_up)
        delta_ns = time - rfx_pol->last_upfreq_time;
    else
        delta_ns = time - rfx_pol->last_downfreq_time;

    return delta_ns >= effective_delay;
}

static bool rfx_update_next_freq(struct rfx_policy *rfx_pol, u64 time,
				 unsigned int next_freq, bool force_down)
{
	if (!rfx_pol)
		return false;

	if (rfx_pol->need_freq_update) {
		rfx_pol->need_freq_update = false;
		if (rfx_pol->next_freq == next_freq &&
		    !rfx_driver_test_flags(0))
			return false;
	} else if (rfx_pol->next_freq == next_freq &&
		   rfx_pol->last_upfreq_time == time) {
		return false;
	}

	if (next_freq < rfx_pol->next_freq) {
	if (!force_down) {
	s64 down_delta = time - rfx_pol->last_downfreq_time;
	s64 effective_down_delay = rfx_pol->down_rate_delay_ns;

		if (rfx_pol->in_heavy_mode ||
		rfx_pol->current_mode == RFX_MODE_GAMING ||
		(rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns)) {
		if (rfx_pol->thermal_throttle_active)
		effective_down_delay = 6000 * NSEC_PER_USEC;
		else
		effective_down_delay = 35000 * NSEC_PER_USEC;
		}

        if (effective_down_delay > 0 &&
            down_delta < effective_down_delay)
            return false;

        rfx_pol->last_downfreq_time = time;
		}
	} else {
		s64 up_delta = time - rfx_pol->last_upfreq_time;
		if (rfx_pol->up_rate_delay_ns > 0 &&
		    up_delta < rfx_pol->up_rate_delay_ns)
			return false;
		rfx_pol->last_upfreq_time = time;
	}

	rfx_pol->next_freq = next_freq;
	return true;
}

static unsigned int rfx_get_next_freq(struct rfx_policy *rfx_pol,
				      unsigned long util, unsigned long max,
				      unsigned int freq_cap_khz, bool is_heavy,
				      u64 time)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int freq;
	bool is_little = (max <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD);
	bool is_prime  = (max >= (unsigned long)RFX_PRIME_CAP_THRESHOLD);
	unsigned int hispeed_pct;

	if (!policy)
		return 0;

	hispeed_pct = rfx_get_hispeed_pct(rfx_pol);
	util        = rfx_apply_headroom(util, max, is_heavy, rfx_pol->current_mode);
	freq        = rfx_get_ref_freq(policy);
	freq        = (unsigned int)((u64)freq * util / max);
	freq        = clamp_t(unsigned int, freq,
			      policy->cpuinfo.min_freq, policy->cpuinfo.max_freq);

	/* LITTLE cluster: strict cap when non-gaming */
	if (is_little && !rfx_pol->in_heavy_mode &&
	!rfx_pol->tunables->gaming_mode &&
	!(rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns)) {
	unsigned int little_nongaming_cap = rfx_adaptive_max(policy, 53);
	if (freq > little_nongaming_cap)
	freq = little_nongaming_cap;
	}

	if (is_prime && freq < rfx_adaptive_floor(policy, RFX_PRIME_GAMING_FLOOR_PCT)) {
	if (is_heavy || rfx_pol->current_mode == RFX_MODE_GAMING)
	freq = rfx_adaptive_floor(policy, RFX_PRIME_GAMING_FLOOR_PCT);
	}

	if (is_prime && rfx_pol->game_launching &&
	rfx_pol->game_launch_end_ns && time < rfx_pol->game_launch_end_ns) {
	if (freq < rfx_adaptive_floor(policy, RFX_GAME_LAUNCH_FLOOR_PCT))
	freq = rfx_adaptive_floor(policy, RFX_GAME_LAUNCH_FLOOR_PCT);
	}

	/* === TIME-BASED DUTY CYCLE THERMAL === */
	if (rfx_pol->current_mode == RFX_MODE_GAMING) {
	if (!rfx_pol->tunables->gaming_mode &&
        !(rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns))
            rfx_thermal_duty_cycle(rfx_pol, time);

        if (is_prime) {
			if (rfx_pol->tunables->gaming_mode) {
			unsigned int hard_floor = rfx_adaptive_floor(policy,
		RFX_PRIME_GAMING_SUSTAIN_FLOOR_PCT);

		/* gaming_mode=1: ignore stale throttle state,
		* let ROM policy->max be the ceiling */
			if (rfx_pol->in_heavy_mode && freq < hard_floor)
			freq = hard_floor;

		/* Clamp to policy->max = ROM/device thermal policy */
			if (freq > policy->max)
			freq = policy->max;

			} else {
				unsigned int soft_cap = rfx_adaptive_max(policy, RFX_GAMING_MAX_PCT);
				unsigned int hard_floor = rfx_adaptive_floor(policy,
					RFX_PRIME_GAMING_SUSTAIN_FLOOR_PCT);

				if (rfx_pol->thermal_throttle_active) {
					soft_cap = rfx_adaptive_max(policy,
						RFX_THERMAL_THROTTLE_CAP_PCT);
				}
				if (soft_cap < hard_floor)
					soft_cap = hard_floor;
				if (freq > soft_cap)
					freq = soft_cap;
				if (freq < hard_floor && rfx_pol->in_heavy_mode)
					freq = hard_floor;
			}
		} else if (!is_little) {

				if (rfx_pol->tunables->gaming_mode) {
			/* gaming_mode=1: ROM decides ceiling via policy->max */
				if (freq > policy->max)
				freq = policy->max;
				if (rfx_pol->in_heavy_mode) {
			unsigned int big_floor = rfx_adaptive_floor(policy,
			RFX_BIG_INTERACTIVE_FLOOR_PCT);
			if (freq < big_floor)
			freq = big_floor;
				}
			} else {
				unsigned int big_cap = rfx_adaptive_max(policy,
					rfx_pol->thermal_throttle_active
					? RFX_THERMAL_THROTTLE_CAP_PCT
					: RFX_BIG_GAMING_MAX_PCT);
				if (freq > big_cap)
					freq = big_cap;
			}
		}
	}

	/* ROM Override: auto-detected at init, adjusts PRIME floor/cap.
	 * Applied AFTER thermal cap so ROM override respects thermal limits.
	 */
	if (rfx_pol->rom_override_active && is_prime &&
	!rfx_pol->tunables->gaming_mode) {
		if (rfx_pol->rom_tweak_detected == 2) {

			unsigned int rom_floor = rfx_adaptive_floor(policy, 75);
			unsigned int rom_cap   = rfx_adaptive_max(policy, 88);
			if (rfx_pol->in_heavy_mode && freq < rom_floor)
				freq = rom_floor;
			if (!rfx_pol->thermal_throttle_active && freq > rom_cap)
				freq = rom_cap;
		} else if (rfx_pol->rom_tweak_detected == 1) {

			unsigned int rom_floor = rfx_adaptive_floor(policy, 73);
			if (rfx_pol->in_heavy_mode && freq < rom_floor)
				freq = rom_floor;
		}
	}


	if (rfx_pol->render_urgency_active && rfx_pol->render_boost_end_ns &&
	    time < rfx_pol->render_boost_end_ns) {
		if (is_prime && freq < rfx_adaptive_floor(policy, 85))  /* was 75 — stronger render boost */
			freq = rfx_adaptive_floor(policy, 85);
		else if (!is_little && !is_prime && freq < rfx_adaptive_floor(policy, 78))
	freq = rfx_adaptive_floor(policy, 78);
	}

	if (rfx_pol->in_heavy_mode &&
	    rfx_pol->current_mode != RFX_MODE_GAMING &&
	    !is_little) {
		unsigned int exit_util_pct = max ?
			(unsigned int)(util * 100 / max) : 0;
		if (exit_util_pct < 20) {
			unsigned int exit_soft_cap = is_prime ?
			rfx_adaptive_floor(policy, RFX_PRIME_GAMING_FLOOR_PCT) :
			rfx_adaptive_floor(policy, RFX_BIG_INTERACTIVE_FLOOR_PCT);
			if (freq > exit_soft_cap)
				freq = exit_soft_cap;
		}
	}

	/* Extended gaming floor */
	if (is_prime && rfx_pol->prime_gaming_floor_active &&
	    rfx_pol->prime_gaming_floor_end_ns &&
	    time < rfx_pol->prime_gaming_floor_end_ns) {
		if (freq < rfx_adaptive_floor(policy, RFX_PRIME_GAMING_FLOOR_PCT))
		freq = rfx_adaptive_floor(policy, RFX_PRIME_GAMING_FLOOR_PCT);
	}

	/* Interactive floor - LOWER for cooler idle */
	if (!rfx_pol->in_heavy_mode &&
	    rfx_pol->interactive_end_ns && time < rfx_pol->interactive_end_ns) {
		struct rfx_cpu *lc = per_cpu_ptr(&rfx_cpu,
				cpumask_first(rfx_pol->policy->cpus));
		if (is_little) {
			if (lc->hispeed_start_ns && freq < RFX_INTERACTIVE_FLOOR_KHZ)
				freq = RFX_INTERACTIVE_FLOOR_KHZ;
		} else {
			if (lc->hispeed_start_ns &&
			freq < rfx_adaptive_floor(policy, RFX_BIG_INTERACTIVE_FLOOR_PCT))
			freq = rfx_adaptive_floor(policy, RFX_BIG_INTERACTIVE_FLOOR_PCT);
		}
	}

	/* Force idle: immediate min freq */
	if (rfx_pol->force_idle && !is_heavy) {
		freq = policy->cpuinfo.min_freq;
	}

	/* Freq cap */
	if (freq_cap_khz > 0) {
		if (freq > freq_cap_khz)
			freq = freq_cap_khz;
	}

	if (freq == rfx_pol->cached_raw_freq && !rfx_pol->need_freq_update)
		return rfx_pol->next_freq;

	rfx_pol->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static void rfx_get_util(struct rfx_cpu *rfx_c, unsigned long boost)
{
	rfx_get_util_gki510(rfx_c->cpu, boost, &rfx_c->util, &rfx_c->bwmin);
}

static bool rfx_big_drop_force_down(struct rfx_policy *rfx_pol,
				    unsigned int next_freq)
{
	unsigned int threshold;

	if (rfx_pol->next_freq == 0)
		return false;

	threshold = rfx_pol->next_freq * RFX_BIG_DROP_PCT / 100;
	return next_freq < threshold;
}

/* === ADAPTIVE MODE UPDATE - BENCHMARK & THERMAL - TUNED === */

static void rfx_update_adaptive_mode(struct rfx_policy *rfx_pol,
				     struct rfx_cpu *rfx_c,
				     unsigned long effective_util,
				     unsigned long max_cap,
				     bool is_big, u64 time)
{
	unsigned int util_pct;
	bool heavy_cond, light_cond;
	bool interactive_cond;
	bool is_prime = (max_cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD);
	s64 idle_time;

	util_pct = (max_cap > 0)
		 ? (unsigned int)(effective_util * 100 / max_cap) : 0;

	/* Mode detection */
	rfx_detect_mode(rfx_pol, rfx_c, effective_util, max_cap, time);

	idle_time = (s64)(time - rfx_pol->last_real_update_ns);

	/* Gaming lock active */
	if (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns) {
		rfx_pol->in_heavy_mode = true;
		rfx_pol->in_light_mode = false;
		rfx_pol->force_idle = false;
		rfx_pol->last_real_update_ns = time;
		return;
	}

			if (is_big) {
			heavy_cond = (util_pct >= RFX_SUSTAIN_HEAVY_ENTER_PCT)
				&& (rfx_c->filtered_busy_pct >= RFX_SUSTAIN_HEAVY_BUSY_PCT || rfx_c->busy_pct >= 18);

			if (!rfx_pol->in_heavy_mode) {
				if (heavy_cond) {
					rfx_pol->sustain_heavy_ticks++;
					if (rfx_pol->sustain_heavy_ticks >= RFX_SUSTAIN_HEAVY_TICKS) {
						rfx_pol->in_heavy_mode       = true;
						rfx_pol->in_light_mode       = false;
						rfx_pol->force_idle          = false;
						rfx_pol->interactive_end_ns  = 0;
						rfx_pol->sustain_heavy_ticks = 0;
						rfx_pol->sustain_exit_ticks  = 0;
						rfx_pol->light_enter_ticks   = 0;

						if (is_prime) {
							rfx_pol->prime_gaming_floor_active  = true;
							rfx_pol->prime_gaming_floor_end_ns  = 0;
						}

						rfx_pol->gaming_lock_end_ns = time + RFX_GAMING_LOCK_DURATION_NS;
					}
				} else {
					rfx_pol->sustain_heavy_ticks = 0;
				}
			} else {
				if (rfx_pol->tunables->gaming_mode) {
					rfx_pol->sustain_exit_ticks = 0;
				} else if (util_pct < RFX_SUSTAIN_HEAVY_EXIT_PCT) {
					rfx_pol->sustain_exit_ticks++;
					if (rfx_pol->sustain_exit_ticks >= RFX_SUSTAIN_EXIT_TICKS) {
						rfx_pol->in_heavy_mode       = false;
						rfx_pol->sustain_exit_ticks  = 0;
						rfx_pol->sustain_heavy_ticks = 0;
						if (is_prime && rfx_pol->prime_gaming_floor_active) {
							rfx_pol->prime_gaming_floor_end_ns =
								time + (300 * NSEC_PER_MSEC);
						}
					}
				} else {
					rfx_pol->sustain_exit_ticks = 0;
				}
			}
		}

	if (rfx_pol->in_heavy_mode) {
		rfx_pol->light_enter_ticks = 0;
		rfx_pol->force_idle = false;
		rfx_pol->last_real_update_ns = time;
		return;
	}

	/* Interactive detection - shorter */
	interactive_cond = (util_pct >= RFX_INTERACTIVE_UTIL_PCT);
	if (interactive_cond) {
	u64 interactive_dur = is_big
	? RFX_INTERACTIVE_DURATION_NS
	: (500 * NSEC_PER_MSEC);
	rfx_pol->interactive_end_ns = time + interactive_dur;
    if (rfx_pol->in_light_mode) {
        rfx_pol->in_light_mode     = false;
        rfx_pol->light_enter_ticks = 0;
        rfx_pol->force_idle = false;
	}
	rfx_pol->last_real_update_ns = time;
	}

	if (rfx_pol->interactive_end_ns && time < rfx_pol->interactive_end_ns) {
		rfx_pol->light_enter_ticks = 0;
		rfx_pol->last_real_update_ns = time;
		return;
	}

	/* Prime gaming floor cleanup */
	if (is_prime && rfx_pol->prime_gaming_floor_active) {
		if (rfx_pol->prime_gaming_floor_end_ns &&
		    time >= rfx_pol->prime_gaming_floor_end_ns) {
			rfx_pol->prime_gaming_floor_active = false;
			rfx_pol->prime_gaming_floor_end_ns = 0;
		}
	}

	if (idle_time > 40 * NSEC_PER_MSEC &&
	util_pct == 0 &&
	rfx_c->filtered_busy_pct == 0 &&
	!rfx_pol->in_light_mode &&
	(!rfx_pol->interactive_end_ns || time >= rfx_pol->interactive_end_ns)) {
	rfx_pol->force_idle = true;
	rfx_pol->force_idle_start_ns = time;
	}

	/* TUNED: Light mode entry - 3% threshold (AGGRESSIVE) */
	light_cond = (util_pct <= RFX_LIGHT_ENTER_PCT)
		  && (rfx_c->filtered_busy_pct < 2)
		  && (rfx_c->act_state <= RFX_ACT_LIGHT)
		  && (rfx_c->hispeed_start_ns == 0)
		  && !rfx_pol->force_idle
		  && (!rfx_pol->interactive_end_ns || time >= rfx_pol->interactive_end_ns);

	if (!rfx_pol->in_light_mode) {
		if (light_cond) {
			rfx_pol->light_enter_ticks++;
			if (rfx_pol->light_enter_ticks >= RFX_LIGHT_ENTER_TICKS) {
				rfx_pol->in_light_mode      = true;
				rfx_pol->light_enter_ticks  = 0;
			}
		} else {
			rfx_pol->light_enter_ticks = 0;
		}
	} else {
		if (util_pct > RFX_LIGHT_EXIT_PCT || rfx_c->hispeed_start_ns != 0
		    || rfx_c->filtered_busy_pct >= 2
		    || rfx_c->act_state >= RFX_ACT_MEDIUM
		    || rfx_pol->force_idle
		    || (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns)) {
			rfx_pol->in_light_mode     = false;
			rfx_pol->light_enter_ticks = 0;
		}
	}
}

/* === BUSY PERCENT & BLEND === */

static void rfx_update_busy_pct(struct rfx_cpu *rfx_c, unsigned int window_us,
				unsigned int base_shift, unsigned long max_cap,
				u64 time)
{
	u64 cur_idle, cur_wall;
	u64 wall_delta, idle_delta;
	unsigned int filter_shift;
	bool is_prime = (max_cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD);
	bool interactive_active;
	unsigned int idle_win_threshold;
	unsigned int step;

	filter_shift = rfx_get_adaptive_shift(rfx_c->util, max_cap, base_shift);

	cur_idle   = get_cpu_idle_time(rfx_c->cpu, &cur_wall, 1);
	wall_delta = cur_wall - rfx_c->prev_wall_time;

	if (wall_delta < (u64)window_us)
		return;

	idle_delta = (cur_idle > rfx_c->prev_idle_time)
		     ? (cur_idle - rfx_c->prev_idle_time) : 0;

	rfx_c->busy_pct = (wall_delta > idle_delta)
		? (unsigned int)(100 * (wall_delta - idle_delta) / wall_delta)
		: 0;

	rfx_c->prev_idle_time = cur_idle;
	rfx_c->prev_wall_time = cur_wall;
	rfx_c->hispeed_active = true;

	if (rfx_c->busy_pct == 0) {
		rfx_c->filtered_busy_pct = 0;
	} else if (!filter_shift) {
		rfx_c->filtered_busy_pct = rfx_c->busy_pct;
	} else if (rfx_c->busy_pct > rfx_c->filtered_busy_pct + 4) {
		rfx_c->filtered_busy_pct = rfx_c->busy_pct;
	} else {
		step = (rfx_c->filtered_busy_pct > rfx_c->busy_pct)
			? (rfx_c->filtered_busy_pct - rfx_c->busy_pct) >> filter_shift
			: 0;
		if (step < rfx_c->filtered_busy_pct)
			rfx_c->filtered_busy_pct -= step;
		else
			rfx_c->filtered_busy_pct = rfx_c->busy_pct;
	}

	if (rfx_c->filtered_busy_pct > 0) {
		rfx_c->hispeed_idle_windows = 0;
		if (!rfx_c->hispeed_start_ns) {
			rfx_c->hispeed_start_ns = time;
			rfx_c->rfx_policy->need_freq_update = true;
		}
	} else {
		interactive_active = rfx_c->rfx_policy->interactive_end_ns &&
				  time < rfx_c->rfx_policy->interactive_end_ns;

		if (!rfx_c->rfx_policy->in_heavy_mode)
			rfx_c->hispeed_idle_windows++;

		idle_win_threshold = is_prime ? 5 : 3;
		if (!interactive_active &&
		    rfx_c->hispeed_idle_windows > idle_win_threshold) {
			rfx_c->hispeed_start_ns   = 0;
			rfx_c->filtered_busy_pct = 0;
		}
	}
}

static unsigned long rfx_blend_util(struct rfx_cpu *rfx_c,
				    unsigned long pelt_util,
				    unsigned long max_cap, u64 time,
				    unsigned int hispeed_boost_pct)
{
	unsigned long hispeed_util;
	unsigned int half_lives;
	unsigned int effective_pct;
	struct rfx_policy *pol = rfx_c->rfx_policy;
	unsigned long min_blend;

	if (!rfx_c->filtered_busy_pct || !rfx_c->hispeed_start_ns) {
		if (pol->current_mode == RFX_MODE_VIDEO &&
		    pol->interactive_end_ns &&
		    time < pol->interactive_end_ns &&
		    (max_cap <= RFX_LITTLE_CAP_THRESHOLD)) {
			min_blend = max_cap * 20 / 100;
			return (pelt_util > min_blend) ? pelt_util : min_blend;
		}
		return pelt_util;
	}

	effective_pct = min(rfx_c->filtered_busy_pct, hispeed_boost_pct);
	hispeed_util  = max_cap * effective_pct / 100;

	if (hispeed_util <= pelt_util)
		return pelt_util;

	half_lives = (unsigned int)((time - rfx_c->hispeed_start_ns) / HISPEED_HALFLIFE_NS);
	if (half_lives >= HISPEED_HALFLIFE_MAX) {
		rfx_c->hispeed_start_ns = time;
		return pelt_util;
	}

	return min(pelt_util + ((hispeed_util - pelt_util) >> half_lives), max_cap);
}

/* === IO WAIT BOOSTING === */

static bool rfx_iowait_reset(struct rfx_cpu *rfx_c, u64 time,
			     bool set_iowait_boost)
{
	s64 delta_ns = time - rfx_c->last_update;

	if (delta_ns <= TICK_NSEC)
		return false;

	rfx_c->iowait_boost         = set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
	rfx_c->iowait_boost_pending = set_iowait_boost;
	return true;
}

static void rfx_iowait_boost(struct rfx_cpu *rfx_c, u64 time,
			     unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;
	unsigned long max_cap;
	unsigned int iowait_cap;

	if (rfx_c->iowait_boost) {
	if (!rfx_iowait_reset(rfx_c, time, set_iowait_boost))
	rfx_c->iowait_boost_pending = set_iowait_boost;
	return;
	}
	if (!set_iowait_boost)
	return;
	if (rfx_c->iowait_boost_pending)
	return;

	rfx_c->iowait_boost_pending = true;

	max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);
	if (rfx_c->iowait_boost >= max_cap) {
		iowait_cap = (max_cap <= RFX_LITTLE_CAP_THRESHOLD)
			? (SCHED_CAPACITY_SCALE / 6)
			: (SCHED_CAPACITY_SCALE * 3 / 4);
		rfx_c->iowait_boost = min_t(unsigned int,
					    rfx_c->iowait_boost << 1,
					    iowait_cap);
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
	return rfx_c->iowait_boost * max_cap >> SCHED_CAPACITY_SHIFT;
}

/* === NOHZ / IDLE HANDLING === */

#ifdef CONFIG_NO_HZ_COMMON
static bool rfx_check_freq_hold_or_drop(struct rfx_cpu *rfx_c,
					unsigned long max_cap,
					bool *out_force_drop)
{
	unsigned long idle_calls;
	bool idle_calls_increased;
	struct rfx_policy *pol = rfx_c->rfx_policy;
	u64 idle_duration;
	u64 now;

	if (rfx_scx_switched_all())
		return false;
	if (rfx_cpu_uclamp_capped(rfx_c->cpu))
		return false;

	if (pol->force_idle) {
		if (out_force_drop)
			*out_force_drop = true;
		return false;
	}

	if (pol->in_deep_idle && pol->idle_entry_time_ns) {
		now = ktime_get_ns();
		idle_duration = now - pol->idle_entry_time_ns;
		if (idle_duration < RFX_IDLE_HYSTERESIS_NS) {
			if (out_force_drop)
				*out_force_drop = false;
			return true;
		}
		pol->in_deep_idle = false;
		pol->idle_entry_time_ns = 0;
		if (out_force_drop)
			*out_force_drop = false;
		return false;
	}

	idle_calls           = tick_nohz_get_idle_calls_cpu(rfx_c->cpu);
	idle_calls_increased = idle_calls != rfx_c->saved_idle_calls;
	rfx_c->saved_idle_calls = idle_calls;

	if (max_cap <= RFX_LITTLE_CAP_THRESHOLD) {
		if (out_force_drop)
			*out_force_drop = idle_calls_increased;
		return false;
	}

	if (max_cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD &&
	     (pol->gaming_lock_end_ns && ktime_get_ns() < pol->gaming_lock_end_ns)) {
		if (out_force_drop)
			*out_force_drop = false;
		return true;
	}

	if (out_force_drop)
		*out_force_drop = false;
	return !idle_calls_increased;
}
#else
static inline bool rfx_check_freq_hold_or_drop(struct rfx_cpu *rfx_c,
						unsigned long max_cap,
						bool *out_force_drop)
{
	if (out_force_drop)
		*out_force_drop = false;
	return false;
}
#endif

static inline void rfx_ignore_dl_rate_limit(struct rfx_cpu *rfx_c)
{
	if (rfx_dl_bw_exceeded_gki510(rfx_c->cpu, rfx_c->bwmin))
		rfx_c->rfx_policy->need_freq_update = true;
}

static void rfx_irq_work(struct irq_work *irq_work);

static void rfx_deferred_update(struct rfx_policy *rfx_pol)
{
	if (!rfx_pol->work_in_progress) {
		rfx_pol->work_in_progress = true;
		irq_work_queue(&rfx_pol->irq_work);
	}
}

/* === UPDATE SINGLE FREQUENCY - MAIN LOGIC - TUNED === */

static void rfx_update_single_freq(struct update_util_data *hook, u64 time,
				   unsigned int flags)
{
	struct rfx_cpu      *rfx_c   = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy   *rfx_pol = rfx_c->rfx_policy;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	unsigned int         cached_freq = rfx_pol->cached_raw_freq;
	unsigned long        max_cap, boost, effective_util;
	unsigned int         next_f, freq_cap_khz = 0;
	bool                 force_down, act_force, nohz_drop = false;
	bool                 hold, is_heavy;
	unsigned int         cur_pct;
	unsigned int         gf;
	unsigned int         idle_cap;
	unsigned int         hispeed_pct;
	unsigned int         hist_snap;

	max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);

	rfx_iowait_boost(rfx_c, time, flags);

	rfx_c->last_update = time;
	rfx_ignore_dl_rate_limit(rfx_c);

	if (!rfx_should_update_freq(rfx_pol, time))
		return;

	boost          = rfx_iowait_apply(rfx_c, time, max_cap);
	rfx_get_util(rfx_c, boost);
	effective_util = max(rfx_c->util, boost);

	rfx_update_busy_pct(rfx_c, tunables->hispeed_window_us,
			    tunables->hispeed_filter_shift, max_cap, time);

	hispeed_pct = rfx_get_hispeed_pct(rfx_pol);
	effective_util = rfx_blend_util(rfx_c, effective_util, max_cap, time,
					hispeed_pct);
	hist_snap = rfx_c->util_history_idx;
	{
		bool is_big_cluster = (max_cap > RFX_LITTLE_CAP_THRESHOLD);
		rfx_update_adaptive_mode(rfx_pol, rfx_c, effective_util, max_cap, is_big_cluster, time);
	}

		if (rfx_pol->tunables->gaming_mode) {
		unsigned int h  = rfx_c->util_history_idx;
		unsigned int h1 = rfx_c->util_history[(h - 1) & 7];
		unsigned int h2 = rfx_c->util_history[(h - 2) & 7];
		unsigned int h3 = rfx_c->util_history[(h - 3) & 7];
		/* Pattern 1: gradual rising (existing) */
		bool rising = h1 > h2 && h2 > h3 && h1 > 20;
		/* Pattern 2: sudden spike from idle/low — catch swipe/gesture bursts */
		bool sudden_spike = (h1 > 30) && (h2 < 20) && (h1 > h2 + 15);
		/* Pattern 3: high-but-flat load staying heavy */
		bool sustained_heavy = (h1 >= 35) && (h2 >= 35) && (h3 >= 35);
		/* Pattern 4: burst-dip-burst (WuWa animation sequence) */
		bool wuwa_anim = (h1 > 25) && (h3 > 25) && (h2 < 18);

		if (rising || sudden_spike || sustained_heavy || wuwa_anim) {
		rfx_pol->in_heavy_mode      = true;
		rfx_pol->gaming_lock_end_ns = time +
		(sudden_spike || wuwa_anim ? (1200 * NSEC_PER_MSEC) : (800 * NSEC_PER_MSEC));
		rfx_pol->render_urgency_active = true;
		rfx_pol->render_boost_end_ns = time +
		(sudden_spike || wuwa_anim ? (600 * NSEC_PER_MSEC) : (150 * NSEC_PER_MSEC));
		}
	}

	if (!rfx_pol->tunables->gaming_mode && rfx_pol->current_mode == RFX_MODE_GAMING) {
		unsigned int h  = rfx_c->util_history_idx;
		unsigned int h1 = rfx_c->util_history[(h - 1) & 7];
		unsigned int h2 = rfx_c->util_history[(h - 2) & 7];
		if (h1 > 25 && h1 > (h2 + 10)) {
			rfx_pol->render_urgency_active = true;
			rfx_pol->render_boost_end_ns = time + (80 * NSEC_PER_MSEC);
		}
	}

	if (!rfx_pol->tunables->gaming_mode) {
		unsigned int cur_util_pct = max_cap ?
			(unsigned int)(effective_util * 100 / max_cap) : 0;
		if (rfx_c->act_state >= RFX_ACT_MEDIUM &&
		    rfx_c->prev_util_pct < 10 && cur_util_pct > 20)
			rfx_pol->interactive_end_ns = time + RFX_INTERACTIVE_DURATION_NS;
	}

	is_heavy = (rfx_c->act_state == RFX_ACT_HEAVY) || rfx_pol->in_heavy_mode ||
               (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns);

	if (rfx_pol->interactive_end_ns && time < rfx_pol->interactive_end_ns)
		act_force = false;

	next_f = rfx_get_next_freq(rfx_pol, effective_util, max_cap,
				   freq_cap_khz, is_heavy, time);

	cur_pct = (max_cap > 0)
		? (unsigned int)(effective_util * 100 / max_cap) : 0;

	if (rfx_c->prev_util_pct > RFX_BURST_DROP_THRESHOLD &&
	    rfx_c->prev_util_pct > cur_pct &&
	    (rfx_c->prev_util_pct - cur_pct) >= RFX_BURST_DROP_THRESHOLD &&
	    (rfx_c->act_state == RFX_ACT_MEDIUM ||
	     rfx_c->act_state == RFX_ACT_HEAVY ||
	     (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns))) {
		if (!rfx_pol->guard_end_ns) {
                        gf = rfx_pol->next_freq ? rfx_pol->next_freq : next_f;
                        if (max_cap > RFX_LITTLE_CAP_THRESHOLD) {
                                unsigned int big_floor = rfx_adaptive_floor(
                                        rfx_pol->policy, RFX_BIG_INTERACTIVE_FLOOR_PCT);
                                if (gf < big_floor)
                                        gf = big_floor;
                        }
                        rfx_pol->guard_end_ns   = time + RFX_BURST_GUARD_NS;
                        rfx_pol->guard_freq_khz = gf;
                }
        }
        rfx_c->prev_util_pct = cur_pct;

	if (rfx_pol->guard_end_ns && !rfx_pol->in_light_mode && !rfx_pol->force_idle) {
		if (time < rfx_pol->guard_end_ns) {
			if (next_f < rfx_pol->guard_freq_khz)
				next_f = rfx_pol->guard_freq_khz;
			act_force = false;
		} else {
			rfx_pol->guard_end_ns   = 0;
			rfx_pol->guard_freq_khz = 0;
		}
	}

	if (rfx_pol->interactive_end_ns && time >= rfx_pol->interactive_end_ns)
		rfx_pol->interactive_end_ns = 0;

	/* Light/Idle mode: aggressive battery saving */
	if ((rfx_pol->in_light_mode || rfx_pol->force_idle) && !rfx_pol->in_heavy_mode &&
	    !(rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns)) {
		idle_cap = rfx_pol->policy->cpuinfo.min_freq
			? rfx_pol->policy->cpuinfo.min_freq
			: RFX_IDLE_DEEP_CAP_KHZ_FALLBACK;

		rfx_pol->interactive_end_ns = 0;
		rfx_pol->guard_end_ns = 0;
		rfx_pol->guard_freq_khz = 0;
		if (next_f > idle_cap)
			next_f = idle_cap;
		act_force = true;
	}

	force_down = act_force || (rfx_pol->guard_end_ns == 0 &&
				   rfx_big_drop_force_down(rfx_pol, next_f));

	hold = rfx_check_freq_hold_or_drop(rfx_c, max_cap, &nohz_drop);
	force_down = force_down || nohz_drop;

	if (hold && next_f == rfx_pol->next_freq &&
	    !rfx_pol->need_freq_update && !force_down) {
		next_f = rfx_pol->next_freq;
		rfx_pol->cached_raw_freq = cached_freq;
	}

	if (max_cap <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD) {
		if (rfx_pol->force_idle) {
			rfx_pol->down_rate_delay_ns = (s64)RFX_LITTLE_DOWN_LIGHT_US * NSEC_PER_USEC;
		} else {
			switch (rfx_c->act_state) {
			case RFX_ACT_HEAVY:
				rfx_pol->down_rate_delay_ns = (s64)RFX_LITTLE_DOWN_HEAVY_US * NSEC_PER_USEC;
				break;
			case RFX_ACT_MEDIUM:
				rfx_pol->down_rate_delay_ns = (s64)RFX_LITTLE_DOWN_MEDIUM_US * NSEC_PER_USEC;
				break;
			case RFX_ACT_LIGHT:
			case RFX_ACT_IDLE:
			default:
				rfx_pol->down_rate_delay_ns = (s64)RFX_LITTLE_DOWN_LIGHT_US * NSEC_PER_USEC;
				break;
			}
		}

		if (rfx_pol->in_heavy_mode ||
		    (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns))
			rfx_pol->down_rate_delay_ns = (s64)RFX_LITTLE_DOWN_HEAVY_US * NSEC_PER_USEC;
	}

	/* Heavy mode exit handling */
	if (max_cap <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD) {
		if (rfx_pol->prev_heavy_mode && !rfx_pol->in_heavy_mode &&
		    !(rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns)) {
			if (rfx_c->act_state == RFX_ACT_HEAVY) {
				rfx_c->act_state      = RFX_ACT_MEDIUM;
				rfx_c->act_down_ticks = 0;
				rfx_c->act_up_ticks   = 0;
			}

			rfx_pol->guard_end_ns   = 0;
			rfx_pol->guard_freq_khz = 0;
			force_down = true;
		}
		rfx_pol->prev_heavy_mode = rfx_pol->in_heavy_mode;
	}

	if (max_cap <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD &&
        rfx_pol->in_heavy_mode) {
        unsigned int little_cap = rfx_adaptive_max(rfx_pol->policy, RFX_LITTLE_GAMING_CAP_PCT);
        if (next_f > little_cap)
            next_f = little_cap;
    }

	/* TUNED: Game launch boost */
	if (!rfx_pol->prev_heavy_mode && rfx_pol->in_heavy_mode &&
	    !rfx_pol->game_launching &&
		rfx_pol->current_mode == RFX_MODE_GAMING) {
		rfx_pol->game_launching     = true;
		rfx_pol->game_launch_end_ns = time + RFX_GAME_LAUNCH_BOOST_NS;
	}
	if (rfx_pol->game_launching && rfx_pol->game_launch_end_ns &&
	    time >= rfx_pol->game_launch_end_ns) {
		rfx_pol->game_launching     = false;
		rfx_pol->game_launch_end_ns = 0;
	}

	/* TUNED: Cleanup render urgency */
	if (rfx_pol->render_boost_end_ns && time >= rfx_pol->render_boost_end_ns) {
		rfx_pol->render_urgency_active = false;
		rfx_pol->render_boost_end_ns = 0;
	}

	rfx_update_next_freq(rfx_pol, time, next_f, force_down);

	if (rfx_pol->policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(rfx_pol->policy, rfx_pol->next_freq);
	else {
		raw_spin_lock(&rfx_pol->update_lock);
		rfx_deferred_update(rfx_pol);
		raw_spin_unlock(&rfx_pol->update_lock);
	}
}

/* === UPDATE SHARED FREQUENCY === */

static unsigned int rfx_next_freq_shared(struct rfx_cpu *rfx_c, u64 time,
					bool *force_down_out)
{
	struct rfx_policy   *rfx_pol  = rfx_c->rfx_policy;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned long util = 0, max_cap;
	bool any_force_down = true;
	bool any_heavy = false;
	unsigned int j, next_f;
	unsigned int freq_cap_khz = 0, j_cap;
	unsigned int j_util_pct;
	struct rfx_cpu *lead;
	unsigned int sgf;
	unsigned int idle_cap;
	unsigned int hispeed_pct;

	max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);

	for_each_cpu(j, policy->cpus) {
		struct rfx_cpu *j_rfxc = per_cpu_ptr(&rfx_cpu, j);
		unsigned long j_boost, j_util;
		bool nohz_drop = false, j_force;

		j_boost = rfx_iowait_apply(j_rfxc, time, max_cap);
		rfx_get_util(j_rfxc, j_boost);
		j_util  = max(j_rfxc->util, j_boost);

		rfx_update_busy_pct(j_rfxc, tunables->hispeed_window_us,
				    tunables->hispeed_filter_shift, max_cap, time);

		hispeed_pct = rfx_get_hispeed_pct(rfx_pol);
		j_util  = rfx_blend_util(j_rfxc, j_util, max_cap, time,
					 hispeed_pct);

		{
			bool j_is_big = (max_cap > RFX_LITTLE_CAP_THRESHOLD);
			rfx_update_adaptive_mode(rfx_pol, j_rfxc, j_util, max_cap, j_is_big, time);
		}

		j_force = rfx_act_update(j_rfxc, j_util, max_cap, time, &j_cap);
		rfx_check_freq_hold_or_drop(j_rfxc, max_cap, &nohz_drop);

		if (!j_force && !nohz_drop)
			any_force_down = false;

		if (j_rfxc->act_state == RFX_ACT_HEAVY || rfx_pol->in_heavy_mode ||
		    (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns))
			any_heavy = true;

		if (j_cap == 0)
			freq_cap_khz = 0;
		else if (freq_cap_khz == 0 || j_cap < freq_cap_khz)
			freq_cap_khz = j_cap;

		util = max(j_util, util);
	}

	next_f = rfx_get_next_freq(rfx_pol, util, max_cap, freq_cap_khz, any_heavy, time);

	j_util_pct = (max_cap > 0)
		? (unsigned int)(util * 100 / max_cap) : 0;
	lead = per_cpu_ptr(&rfx_cpu,
			   cpumask_first(policy->cpus));

	if (lead->prev_util_pct > RFX_BURST_DROP_THRESHOLD &&
	    lead->prev_util_pct > j_util_pct &&
	    (lead->prev_util_pct - j_util_pct) >= RFX_BURST_DROP_THRESHOLD &&
	    (any_heavy || rfx_pol->in_heavy_mode ||
	     (rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns))) {
		if (!rfx_pol->guard_end_ns) {
                        sgf = rfx_pol->next_freq ? rfx_pol->next_freq : next_f;
                        if (max_cap > RFX_LITTLE_CAP_THRESHOLD) {
                                unsigned int big_floor = rfx_adaptive_floor(
                                        rfx_pol->policy, RFX_BIG_INTERACTIVE_FLOOR_PCT);
                                if (sgf < big_floor)
                                        sgf = big_floor;
                        }
                        rfx_pol->guard_end_ns   = time + RFX_BURST_GUARD_NS;
                        rfx_pol->guard_freq_khz = sgf;
                }
        }
        lead->prev_util_pct = j_util_pct;

	if (rfx_pol->guard_end_ns) {
		if (time < rfx_pol->guard_end_ns) {
			if (next_f < rfx_pol->guard_freq_khz)
				next_f = rfx_pol->guard_freq_khz;
			any_force_down = false;
		} else {
			rfx_pol->guard_end_ns   = 0;
			rfx_pol->guard_freq_khz = 0;
		}
	}

	if (rfx_pol->interactive_end_ns) {
		if (time < rfx_pol->interactive_end_ns) {
			any_force_down = false;
		} else {
			rfx_pol->interactive_end_ns = 0;
		}
	}

	if ((rfx_pol->in_light_mode || rfx_pol->force_idle) && !rfx_pol->in_heavy_mode &&
	    !(rfx_pol->gaming_lock_end_ns && time < rfx_pol->gaming_lock_end_ns)) {
		idle_cap = rfx_pol->policy->cpuinfo.min_freq
			? rfx_pol->policy->cpuinfo.min_freq
			: RFX_IDLE_DEEP_CAP_KHZ_FALLBACK;

		rfx_pol->interactive_end_ns = 0;
		rfx_pol->guard_end_ns = 0;
		rfx_pol->guard_freq_khz = 0;
		if (next_f > idle_cap)
			next_f = idle_cap;
		any_force_down = true;
	}

	if (force_down_out)
		*force_down_out = any_force_down || (rfx_pol->guard_end_ns == 0 &&
						    rfx_big_drop_force_down(rfx_pol, next_f));

	return next_f;
}

static void rfx_update_shared(struct update_util_data *hook, u64 time,
			      unsigned int flags)
{
	struct rfx_cpu    *rfx_c  = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	unsigned int       next_f;
	bool               force_down = false;

	raw_spin_lock(&rfx_pol->update_lock);

	rfx_iowait_boost(rfx_c, time, flags);
	rfx_c->last_update = time;
	rfx_ignore_dl_rate_limit(rfx_c);

	if (rfx_should_update_freq(rfx_pol, time)) {
		next_f = rfx_next_freq_shared(rfx_c, time, &force_down);
		rfx_update_next_freq(rfx_pol, time, next_f, force_down);

		if (rfx_pol->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(rfx_pol->policy, rfx_pol->next_freq);
		else
			rfx_deferred_update(rfx_pol);
	}

	raw_spin_unlock(&rfx_pol->update_lock);
}

/* === WORKER THREAD === */

static void rfx_work(struct kthread_work *work)
{
	struct rfx_policy *rfx_pol = container_of(work, struct rfx_policy, work);
	unsigned int  freq;
	unsigned long flags;

	raw_spin_lock_irqsave(&rfx_pol->update_lock, flags);
	freq = rfx_pol->next_freq;
	rfx_pol->work_in_progress = false;
	raw_spin_unlock_irqrestore(&rfx_pol->update_lock, flags);

	mutex_lock(&rfx_pol->work_lock);
	cpufreq_driver_target(rfx_pol->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&rfx_pol->work_lock);
}

static void rfx_irq_work(struct irq_work *irq_work)
{
	struct rfx_policy *rfx_pol = container_of(irq_work, struct rfx_policy, irq_work);
	kthread_queue_work(&rfx_pol->worker, &rfx_pol->work);
}

/* === SYSFS ATTRIBUTES === */

static struct rfx_tunables *rfx_global_tunables;
static DEFINE_MUTEX(rfx_global_tunables_lock);

#define RFX_TUNABLE_UINT(name)		static ssize_t name##_show(struct gov_attr_set *attr_set, char *buf)	{		return sprintf(buf, "%u\n", to_rfx_tunables(attr_set)->name);	}		static ssize_t name##_store(struct gov_attr_set *attr_set,		const char *buf, size_t count)		{		struct rfx_tunables *t = to_rfx_tunables(attr_set);		unsigned int val;		if (kstrtouint(buf, 10, &val))		return -EINVAL;		t->name = val;		return count;		}		static struct governor_attr name = __ATTR_RW(name)

static ssize_t rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_rfx_tunables(attr_set)->rate_limit_us);
}
static ssize_t rate_limit_us_store(struct gov_attr_set *attr_set,
				   const char *buf, size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	tunables->rate_limit_us = val;
	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_pol->freq_update_delay_ns = val * NSEC_PER_USEC;
	return count;
}
static struct governor_attr rate_limit_us =
	__ATTR(rate_limit_us, 0644, rate_limit_us_show, rate_limit_us_store);

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_rfx_tunables(attr_set)->up_rate_limit_us);
}
static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	tunables->up_rate_limit_us = val;
	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_pol->up_rate_delay_ns = (s64)val * NSEC_PER_USEC;
	return count;
}
static struct governor_attr up_rate_limit_us =
	__ATTR(up_rate_limit_us, 0644, up_rate_limit_us_show, up_rate_limit_us_store);

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_rfx_tunables(attr_set)->down_rate_limit_us);
}
static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	tunables->down_rate_limit_us = val;
	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_pol->down_rate_delay_ns = (s64)val * NSEC_PER_USEC;
	return count;
}
static struct governor_attr down_rate_limit_us =
	__ATTR(down_rate_limit_us, 0644, down_rate_limit_us_show, down_rate_limit_us_store);

static ssize_t hispeed_boost_pct_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_rfx_tunables(attr_set)->hispeed_boost_pct);
}
static ssize_t hispeed_boost_pct_store(struct gov_attr_set *attr_set,
				       const char *buf, size_t count)
{
	struct rfx_tunables *t = to_rfx_tunables(attr_set);
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val < 1 || val > 100)
		return -EINVAL;
	t->hispeed_boost_pct = val;
	return count;
}
static struct governor_attr hispeed_boost_pct =
	__ATTR(hispeed_boost_pct, 0644, hispeed_boost_pct_show, hispeed_boost_pct_store);

static ssize_t gaming_mode_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_rfx_tunables(attr_set)->gaming_mode);
}
static ssize_t gaming_mode_store(struct gov_attr_set *attr_set,
				const char *buf, size_t count)
{
	struct rfx_tunables *t = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val > 1)
		return -EINVAL;

	t->gaming_mode = val;

	if (!val) {
	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook) {
	rfx_pol->gaming_lock_end_ns      = 0;
	rfx_pol->current_mode            = RFX_MODE_NORMAL;
	rfx_pol->in_heavy_mode           = false;
	rfx_pol->in_light_mode           = false;
	rfx_pol->force_idle              = false;
	rfx_pol->need_freq_update        = true;
	rfx_pol->mode_switch_time_ns     = ktime_get_ns();
	rfx_pol->game_launching          = false;
	rfx_pol->game_launch_end_ns      = 0;
	rfx_pol->prime_gaming_floor_active = false;
	rfx_pol->prime_gaming_floor_end_ns = 0;
	rfx_pol->render_urgency_active   = false;
	rfx_pol->render_boost_end_ns     = 0;
	}
	}
	return count;
}
static struct governor_attr gaming_mode =
	__ATTR(gaming_mode, 0644, gaming_mode_show, gaming_mode_store);

RFX_TUNABLE_UINT(hispeed_window_us);
RFX_TUNABLE_UINT(hispeed_filter_shift);

static struct attribute *rfx_little_attrs[] = {
	&hispeed_boost_pct.attr,
	&rate_limit_us.attr,
	NULL
};

static struct attribute *rfx_big_attrs[] = {
	&down_rate_limit_us.attr,
	&hispeed_boost_pct.attr,
	&hispeed_filter_shift.attr,
	&rate_limit_us.attr,
	&gaming_mode.attr,
	NULL
};

static struct attribute *rfx_prime_attrs[] = {
	&hispeed_boost_pct.attr,
	&hispeed_filter_shift.attr,
	&hispeed_window_us.attr,
	&rate_limit_us.attr,
	&up_rate_limit_us.attr,
	&gaming_mode.attr,
	NULL
};

static void rfx_tunables_free(struct kobject *kobj)
{
	kfree(to_rfx_tunables(rfx_to_gov_attr_set(kobj)));
}

static struct kobj_type rfx_little_ktype = {
	.default_attrs  = rfx_little_attrs,
	.sysfs_ops      = &governor_sysfs_ops,
	.release        = rfx_tunables_free,
};

static struct kobj_type rfx_big_ktype = {
	.default_attrs  = rfx_big_attrs,
	.sysfs_ops      = &governor_sysfs_ops,
	.release        = rfx_tunables_free,
};

static struct kobj_type rfx_prime_ktype = {
	.default_attrs  = rfx_prime_attrs,
	.sysfs_ops      = &governor_sysfs_ops,
	.release        = rfx_tunables_free,
};

static struct cpufreq_governor vorpal_gov;

/* === POLICY ALLOCATION === */

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

/* === KTHREAD WORKER === */

static int rfx_kthread_create(struct rfx_policy *rfx_pol)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size          = sizeof(struct sched_attr),
		.sched_policy  = SCHED_DEADLINE,
		.sched_flags   = 0,
		.sched_runtime = 3 * NSEC_PER_MSEC,
		.sched_deadline = 10 * NSEC_PER_MSEC,
		.sched_period  = 10 * NSEC_PER_MSEC,
	};
	struct cpufreq_policy *policy = rfx_pol->policy;
	int ret;

	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&rfx_pol->work, rfx_work);
	kthread_init_worker(&rfx_pol->worker);

	thread = kthread_create(kthread_worker_fn, &rfx_pol->worker,
				"rfx_gov/%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("vorpal: kthread create failed %ld\n", PTR_ERR(thread));
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
	kthread_flush_worker(&rfx_pol->worker);
	kthread_stop(rfx_pol->thread);
	mutex_destroy(&rfx_pol->work_lock);
}

/* === TUNABLES ALLOCATION === */

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

/* === AUTO ROM DETECTION - Detect ROM sysctl tweaks at governor init === */
static u8 rfx_detect_rom_tweak(void)
{
	int score = 0;

	/* vm.swappiness: lower = more aggressively tuned ROM */
	if (vm_swappiness <= 5)
		score += 3;
	else if (vm_swappiness <= 15)
		score += 2;
	else if (vm_swappiness <= 30)
		score += 1;

	/* sched_latency_ns: lower = ROM tuned scheduler for low latency */
	if (sysctl_sched_latency <= 1000000UL)        /* <= 1ms */
		score += 3;
	else if (sysctl_sched_latency <= 3000000UL)   /* <= 3ms */
		score += 2;
	else if (sysctl_sched_latency <= 5000000UL)   /* <= 5ms */
		score += 1;

	if (score >= 5)
		return 2;
	else if (score >= 2)
		return 1; /* light tweak */
	return 0;   /* stock ROM */
}

/* === GOVERNOR INIT === */

static int rfx_init(struct cpufreq_policy *policy)
{
	struct rfx_policy   *rfx_pol;
	struct rfx_tunables *tunables;
	unsigned long        max_cap;
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
		gov_attr_set_get(&rfx_global_tunables->attr_set, &rfx_pol->tunables_hook);

		rfx_pol->freq_update_delay_ns = (s64)rfx_global_tunables->rate_limit_us * NSEC_PER_USEC;
		rfx_pol->up_rate_delay_ns     = (s64)rfx_global_tunables->up_rate_limit_us * NSEC_PER_USEC;
		rfx_pol->down_rate_delay_ns   = (s64)rfx_global_tunables->down_rate_limit_us * NSEC_PER_USEC;
		goto out;
	}

	tunables = rfx_tunables_alloc(rfx_pol);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->hispeed_window_us    = CPUFREQ_VORPAL_DEFAULT_HISPEED_WINDOW_US;
	tunables->hispeed_filter_shift = CPUFREQ_VORPAL_DEFAULT_HISPEED_FILTER_SHIFT;
	tunables->hispeed_boost_pct    = CPUFREQ_VORPAL_DEFAULT_HISPEED_BOOST_PCT;
	tunables->gaming_mode          = 0;

	/* Auto-detect ROM tweak level at init */
	rfx_pol->rom_tweak_detected  = rfx_detect_rom_tweak();
	rfx_pol->rom_override_active = (rfx_pol->rom_tweak_detected > 0);
	if (rfx_pol->rom_override_active)
		pr_info("vorpal: ROM tweak detected (level %u), governor override active\n",
			rfx_pol->rom_tweak_detected);

	max_cap = arch_scale_cpu_capacity(NULL, cpumask_first(policy->cpus));

	if (max_cap <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD) {
		tunables->cluster_type       = RFX_CLUSTER_LITTLE;
		tunables->rate_limit_us      = CPUFREQ_VORPAL_DEFAULT_RATE_LIMIT_US;
		tunables->up_rate_limit_us   = CPUFREQ_VORPAL_LITTLE_UP_RATE_LIMIT_US;
		tunables->down_rate_limit_us = CPUFREQ_VORPAL_LITTLE_DOWN_RATE_LIMIT_US;
	} else if (max_cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD) {
		tunables->cluster_type       = RFX_CLUSTER_PRIME;
		tunables->rate_limit_us      = CPUFREQ_VORPAL_PRIME_RATE_LIMIT_US;
		tunables->up_rate_limit_us   = CPUFREQ_VORPAL_PRIME_UP_RATE_LIMIT_US;
		tunables->down_rate_limit_us = CPUFREQ_VORPAL_PRIME_DOWN_RATE_LIMIT_US;
	} else {
		tunables->cluster_type       = RFX_CLUSTER_BIG;
		tunables->rate_limit_us      = CPUFREQ_VORPAL_DEFAULT_RATE_LIMIT_US;
		tunables->up_rate_limit_us   = CPUFREQ_VORPAL_BIG_UP_RATE_LIMIT_US;
		tunables->down_rate_limit_us = CPUFREQ_VORPAL_BIG_DOWN_RATE_LIMIT_US;
	}

	policy->governor_data = rfx_pol;
	rfx_pol->tunables     = tunables;

	rfx_pol->freq_update_delay_ns = (s64)tunables->rate_limit_us * NSEC_PER_USEC;
	rfx_pol->up_rate_delay_ns     = (s64)tunables->up_rate_limit_us * NSEC_PER_USEC;
	rfx_pol->down_rate_delay_ns   = (s64)tunables->down_rate_limit_us * NSEC_PER_USEC;

	{
		struct kobj_type *ktype;
		switch (tunables->cluster_type) {
		case RFX_CLUSTER_LITTLE:
			ktype = &rfx_little_ktype; break;
		case RFX_CLUSTER_PRIME:
			ktype = &rfx_prime_ktype; break;
		case RFX_CLUSTER_BIG:
		default:
			ktype = &rfx_big_ktype; break;
		}
		ret = kobject_init_and_add(&tunables->attr_set.kobj, ktype,
					   get_governor_parent_kobj(policy),
					   "%s", vorpal_gov.name);
	}
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
	pr_err("vorpal: init failed error %d\n", ret);
	return ret;
}

/* === GOVERNOR EXIT === */

static void rfx_exit(struct cpufreq_policy *policy)
{
	struct rfx_policy   *rfx_pol  = policy->governor_data;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	unsigned int         count;

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

/* === GOVERNOR START === */

static int rfx_start(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;
	u64 now;
	int i;

	rfx_pol->freq_update_delay_ns = (s64)rfx_pol->tunables->rate_limit_us * NSEC_PER_USEC;
	rfx_pol->up_rate_delay_ns     = (s64)rfx_pol->tunables->up_rate_limit_us * NSEC_PER_USEC;
	rfx_pol->down_rate_delay_ns   = (s64)rfx_pol->tunables->down_rate_limit_us * NSEC_PER_USEC;

	now = ktime_get_ns();
	rfx_pol->last_upfreq_time    = now;
	rfx_pol->last_downfreq_time  = now;
	rfx_pol->next_freq           = policy->cur > 0 ? policy->cur : policy->cpuinfo.min_freq;
	rfx_pol->work_in_progress    = false;
	rfx_pol->limits_changed      = false;
	rfx_pol->cached_raw_freq     = 0;
	rfx_pol->need_freq_update    = false;
	rfx_pol->sustain_heavy_ticks = 0;
	rfx_pol->sustain_exit_ticks  = 0;
	rfx_pol->light_enter_ticks   = 0;
	rfx_pol->in_heavy_mode       = false;
	rfx_pol->in_light_mode       = false;
	rfx_pol->prev_heavy_mode     = false;
	rfx_pol->guard_end_ns        = 0;
	rfx_pol->guard_freq_khz      = 0;
	rfx_pol->interactive_end_ns  = 0;
	rfx_pol->prime_gaming_floor_active = false;
	rfx_pol->prime_gaming_floor_end_ns = 0;
	rfx_pol->force_idle          = false;
	rfx_pol->force_idle_start_ns = 0;
	rfx_pol->last_real_update_ns = now;
	rfx_pol->current_mode        = RFX_MODE_NORMAL;
	rfx_pol->mode_switch_time_ns = now;
	rfx_pol->gaming_lock_end_ns  = 0;
	rfx_pol->video_pattern_detected = false;
	rfx_pol->video_detect_start_ns  = 0;
	rfx_pol->idle_entry_time_ns  = 0;
	rfx_pol->in_deep_idle        = false;
	rfx_pol->game_launch_end_ns  = 0;
	rfx_pol->game_launching      = false;
	rfx_pol->thermal_duty_window_start_ns  = 0;
	rfx_pol->thermal_throttle_end_ns       = 0;
	rfx_pol->thermal_throttle_active       = false;
	rfx_pol->thermal_sustain_window_count  = 0;
	rfx_pol->thermal_duty_last_active_ns   = 0;
	rfx_pol->render_urgency_active = false;
	rfx_pol->render_boost_end_ns   = 0;

	uu = policy_is_shared(policy) ? rfx_update_shared : rfx_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		struct rfx_cpu *rfx_c = per_cpu_ptr(&rfx_cpu, cpu);
		memset(rfx_c, 0, sizeof(*rfx_c));
		rfx_c->cpu        = cpu;
		rfx_c->rfx_policy = rfx_pol;
		rfx_c->act_state  = RFX_ACT_IDLE;
		rfx_c->prev_idle_time = get_cpu_idle_time(cpu, &rfx_c->prev_wall_time, 1);
		rfx_c->util_history_idx = 0;
		for (i = 0; i < 8; i++)
			rfx_c->util_history[i] = 0;
		cpufreq_add_update_util_hook(cpu, &rfx_c->update_util, uu);
	}
	return 0;
}

/* === GOVERNOR STOP === */

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

/* === GOVERNOR LIMITS === */

static void rfx_limits(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&rfx_pol->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&rfx_pol->work_lock);
	}

	smp_wmb();
	WRITE_ONCE(rfx_pol->limits_changed, true);
}

/* === GOVERNOR STRUCTURE === */

static struct cpufreq_governor vorpal_gov = {
	.name   = "vorpal",
	.owner  = THIS_MODULE,
	.dynamic_switching = true,
	.init   = rfx_init,
	.exit   = rfx_exit,
	.start  = rfx_start,
	.stop   = rfx_stop,
	.limits = rfx_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_VORPAL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &vorpal_gov;
}
#endif

/* === MODULE INIT/EXIT === */

static int __init vorpal_gov_init(void)
{
	pr_info("%s %s by %s\n", CPUFREQ_VORPAL_PROGNAME,
		CPUFREQ_VORPAL_VERSION, CPUFREQ_VORPAL_AUTHOR);
	pr_info("Gaming<43C | Idle<1%% | ThermalProactive | RenderUrgency | AdaptiveROM\n");
	return cpufreq_register_governor(&vorpal_gov);
}

static void __exit vorpal_gov_exit(void)
{
	cpufreq_unregister_governor(&vorpal_gov);
}

module_init(vorpal_gov_init);
module_exit(vorpal_gov_exit);

MODULE_AUTHOR(CPUFREQ_VORPAL_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(CPUFREQ_VORPAL_PROGNAME);

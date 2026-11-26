// SPDX-License-Identifier: GPL-2.0
/*
 * cfs_timeshare.c - Custom Linux 7.0 x86_64 (Ubuntu Server 26.04 LTS) System
 *                   Call implementing a CFS-Inspired Virtual Runtime (vruntime)
 *                   Time-Sharing Scheduler.
 *
 * System call signature:
 *   long sys_cfs_timeshare(struct cfs_proc_arg __user *user_procs,
 *                          int count, int base_quantum_ms);
 *
 * Design overview:
 *   - Copies up to MAX_CFS_PROCS (16) process descriptors from user space.
 *   - Maps each process's nice value [-20, +19] to Linux CFS weights
 *     (High -5 -> 3121, Normal 0 -> 1024, Low +5 -> 335).
 *   - Drives preemptive time-sharing ticks from a kernel timer_list callback
 *     (softirq context) protected by a spinlock_t with IRQ-safe locking.
 *   - Preempts the active task via SIGSTOP, updates its vruntime according to
 *       delta_vruntime = (actual_slice_ms * NICE_0_LOAD) / weight,
 *     prunes exited/zombie tasks, picks the alive task with minimum vruntime,
 *     resumes it via SIGCONT, and re-arms the timer with a dynamically weighted
 *     time slice clamped to safe bounds.
 *   - Stops the timer cleanly via timer_delete_sync() and logs a final summary
 *     table in dmesg once all registered processes have terminated.
 */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/timer.h>
#include <linux/uaccess.h>

#if defined(__has_include)
#if __has_include(<linux/cfs_shared.h>)
#include <linux/cfs_shared.h>
#elif __has_include("../../include/cfs_shared.h")
#include "../../include/cfs_shared.h"
#endif
#endif

/* Fallback definitions if built standalone without copying cfs_shared.h */
#ifndef CFS_SHARED_H
#define MAX_CFS_PROCS          16
#define CFS_NICE_MIN           (-20)
#define CFS_NICE_MAX           (19)
#define CFS_NICE_0_LOAD        1024U
#define CFS_MIN_QUANTUM_MS     10
#define CFS_MAX_QUANTUM_MS     2000
#define CFS_MIN_SLICE_MS       20
#define CFS_MAX_SLICE_MS       1000

struct cfs_proc_arg {
	pid_t pid;
	int nice_val;
};
#endif

#define CFS_LOG_PREFIX "[CFS_SYS] "

/*
 * Standard Linux CFS priority-to-weight mapping table (nice -20 to +19).
 * Each nice increment decreases weight by approximately a factor of 1.25.
 *   nice -5 -> 3121 (High)
 *   nice  0 -> 1024 (Normal / NICE_0_LOAD)
 *   nice +5 ->  335 (Low)
 */
static const u32 cfs_prio_to_weight[40] = {
 /* -20 */ 88761, 71755, 56483, 46273, 36291,
 /* -15 */ 29154, 23254, 18705, 14949, 11916,
 /* -10 */  9548,  7620,  6100,  4904,  3906,
 /*  -5 */  3121,  2501,  1991,  1586,  1277,
 /*   0 */  1024,   820,   655,   526,   423,
 /*   5 */   335,   272,   215,   172,   137,
 /*  10 */   110,    87,    70,    56,    45,
 /*  15 */    36,    29,    23,    18,    15,
};

struct cfs_proc_entry {
	pid_t pid;
	int nice_val;
	u32 weight;
	u64 vruntime;
	u64 exec_count;
	u64 total_runtime_ms;
	unsigned long last_start_jiffies;
	bool active;
};

struct cfs_scheduler_state {
	spinlock_t lock;
	struct timer_list timer;
	struct cfs_proc_entry procs[MAX_CFS_PROCS];
	int count;
	int current_idx;
	int base_quantum_ms;
	u64 tick_count;
	bool running;
	bool timer_initialized;
};

/*
 * cfs_syscall_mutex serializes concurrent invocations of sys_cfs_timeshare in
 * sleepable process context so timer_delete_sync() can be called safely without
 * ever holding cfs_sched.lock (preventing timer-callback lock inversion).
 */
static DEFINE_MUTEX(cfs_syscall_mutex);

static struct cfs_scheduler_state cfs_sched = {
	.lock = __SPIN_LOCK_UNLOCKED(cfs_sched.lock),
	.count = 0,
	.current_idx = -1,
	.base_quantum_ms = 100,
	.tick_count = 0,
	.running = false,
	.timer_initialized = false,
};

static u32 cfs_nice_to_weight(int nice_val)
{
	int clamped = clamp(nice_val, CFS_NICE_MIN, CFS_NICE_MAX);

	return cfs_prio_to_weight[clamped - CFS_NICE_MIN];
}

/*
 * Must be called under rcu_read_lock(). Returns true only if the task exists
 * and has not entered exit/zombie/dead teardown.
 */
static bool cfs_task_is_alive(const struct task_struct *task)
{
	if (!task)
		return false;
	if (!pid_alive(task))
		return false;
	if (READ_ONCE(task->exit_state) & (EXIT_ZOMBIE | EXIT_DEAD))
		return false;
	if (task->flags & PF_EXITING)
		return false;
	return true;
}

/*
 * Deliver SIGSTOP or SIGCONT safely from atomic/softirq context via RCU lookup.
 * Returns true if the target process was alive and the signal was dispatched.
 */
static bool cfs_signal_pid(pid_t pid, int sig)
{
	struct task_struct *task;
	bool sent = false;

	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (cfs_task_is_alive(task)) {
		send_sig(sig, task, 1);
		sent = true;
	}
	rcu_read_unlock();

	return sent;
}

static bool cfs_pid_is_alive(pid_t pid)
{
	struct task_struct *task;
	bool alive;

	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	alive = cfs_task_is_alive(task);
	rcu_read_unlock();

	return alive;
}

/*
 * Compute a CFS-style dynamically weighted time slice for a process:
 *   sched_latency_ms = base_quantum_ms * active_count
 *   slice_ms         = (sched_latency_ms * weight) / total_active_weight
 *
 * Clamped safely between min_slice and CFS_MAX_SLICE_MS to prevent both
 * excessive context-switch thrashing and unbounded latency spikes.
 */
static u32 cfs_calc_slice_ms(int base_quantum_ms, int active_count,
			     u32 weight, u64 total_active_weight)
{
	u32 min_slice = min_t(u32, CFS_MIN_SLICE_MS, (u32)base_quantum_ms);
	u64 sched_latency_ms;
	u64 slice_ms;

	if (active_count <= 0 || total_active_weight == 0)
		return clamp_t(u32, (u32)base_quantum_ms, min_slice, CFS_MAX_SLICE_MS);

	sched_latency_ms = (u64)base_quantum_ms * (u64)active_count;
	slice_ms = div64_u64(sched_latency_ms * (u64)weight, total_active_weight);

	if (slice_ms < min_slice)
		slice_ms = min_slice;
	if (slice_ms > CFS_MAX_SLICE_MS)
		slice_ms = CFS_MAX_SLICE_MS;

	return (u32)slice_ms;
}

/*
 * Log a complete summary table to dmesg once all tracked tasks have exited.
 * Uses pure integer arithmetic (no floating point in kernel space).
 * Must be called with cfs_sched.lock held.
 */
static void cfs_log_summary_locked(void)
{
	u64 total_cpu_ms = 0;
	int i;

	for (i = 0; i < cfs_sched.count; i++)
		total_cpu_ms += cfs_sched.procs[i].total_runtime_ms;

	printk(KERN_INFO CFS_LOG_PREFIX "========================================================================\n");
	printk(KERN_INFO CFS_LOG_PREFIX "All %d processes finished after %llu ticks (total scheduled CPU: %llu ms). Timer stopped.\n",
	       cfs_sched.count, cfs_sched.tick_count, total_cpu_ms);
	printk(KERN_INFO CFS_LOG_PREFIX "+------+--------+------+--------+--------+----------+-----------+-----------+\n");
	printk(KERN_INFO CFS_LOG_PREFIX "| Slot |  PID   | Nice | Weight | Slices | CPU (ms) |  vruntime | CPU Share |\n");
	printk(KERN_INFO CFS_LOG_PREFIX "+------+--------+------+--------+--------+----------+-----------+-----------+\n");

	for (i = 0; i < cfs_sched.count; i++) {
		const struct cfs_proc_entry *p = &cfs_sched.procs[i];
		u64 share_tenths = 0;

		if (total_cpu_ms > 0)
			share_tenths = div64_u64(p->total_runtime_ms * 1000ULL,
						 total_cpu_ms);

		printk(KERN_INFO CFS_LOG_PREFIX "|  %02d  | %6d | %+4d | %6u | %6llu | %8llu | %9llu |   %3llu.%llu%%  |\n",
		       i, p->pid, p->nice_val, p->weight, p->exec_count,
		       p->total_runtime_ms, p->vruntime,
		       share_tenths / 10, share_tenths % 10);
	}

	printk(KERN_INFO CFS_LOG_PREFIX "+------+--------+------+--------+--------+----------+-----------+-----------+\n");
	printk(KERN_INFO CFS_LOG_PREFIX "========================================================================\n");
}

/*
 * Pick the active process with the smallest vruntime (Core CFS invariant).
 * Ties in vruntime are broken in favor of higher weight, then lower slot index.
 * Must be called with cfs_sched.lock held.
 */
static int cfs_select_next_locked(int *out_active_count, u64 *out_total_weight)
{
	int best_idx = -1;
	int active_count = 0;
	u64 total_weight = 0;
	u64 min_vruntime = U64_MAX;
	int i;

	for (i = 0; i < cfs_sched.count; i++) {
		struct cfs_proc_entry *p = &cfs_sched.procs[i];

		if (!p->active)
			continue;

		if (!cfs_pid_is_alive(p->pid)) {
			p->active = false;
			printk(KERN_INFO CFS_LOG_PREFIX "Tick %04llu | PID %-6d exited (final vrt=%llu, slices=%llu, cpu=%llu ms)\n",
			       cfs_sched.tick_count, p->pid, p->vruntime,
			       p->exec_count, p->total_runtime_ms);
			continue;
		}

		active_count++;
		total_weight += p->weight;

		if (best_idx < 0 ||
		    p->vruntime < min_vruntime ||
		    (p->vruntime == min_vruntime &&
		     p->weight > cfs_sched.procs[best_idx].weight)) {
			min_vruntime = p->vruntime;
			best_idx = i;
		}
	}

	*out_active_count = active_count;
	*out_total_weight = total_weight;
	return best_idx;
}

/*
 * Timer softirq callback driving preemptive CFS time-sharing ticks.
 * Strictly atomic: never sleeps, protects shared state with spin_lock_irqsave.
 */
static void cfs_timer_callback(struct timer_list *t)
{
	unsigned long flags;
	unsigned long now = jiffies;
	pid_t preempted_pid = -1;
	u32 actual_slice_ms = 0;
	u64 delta_vruntime = 0;
	u64 preempted_vrt = 0;
	bool preempted_exited = false;
	int active_count = 0;
	u64 total_weight = 0;
	int next_idx = -1;
	u32 next_slice_ms = 0;
	int attempts;

	(void)t;

	spin_lock_irqsave(&cfs_sched.lock, flags);

	if (!cfs_sched.running) {
		spin_unlock_irqrestore(&cfs_sched.lock, flags);
		return;
	}

	cfs_sched.tick_count++;

	/*
	 * Step 1: Preempt the currently running process via SIGSTOP and account
	 * its elapsed execution slice into vruntime.
	 */
	if (cfs_sched.current_idx >= 0 && cfs_sched.current_idx < cfs_sched.count) {
		struct cfs_proc_entry *curr = &cfs_sched.procs[cfs_sched.current_idx];

		preempted_pid = curr->pid;
		if (curr->active) {
			unsigned long elapsed_jiffies = now - curr->last_start_jiffies;

			actual_slice_ms = jiffies_to_msecs(elapsed_jiffies);
			if (actual_slice_ms == 0)
				actual_slice_ms = 1;

			if (!cfs_signal_pid(curr->pid, SIGSTOP)) {
				curr->active = false;
				preempted_exited = true;
			}

			delta_vruntime = div_u64((u64)actual_slice_ms * CFS_NICE_0_LOAD,
						 curr->weight);
			if (delta_vruntime == 0)
				delta_vruntime = 1;

			curr->vruntime += delta_vruntime;
			curr->total_runtime_ms += actual_slice_ms;
			curr->exec_count++;
			preempted_vrt = curr->vruntime;

			if (preempted_exited) {
				printk(KERN_INFO CFS_LOG_PREFIX "Tick %04llu | PID %-6d finished during slice (slice=%u ms, final vrt=%llu, slices=%llu)\n",
				       cfs_sched.tick_count, curr->pid,
				       actual_slice_ms, curr->vruntime,
				       curr->exec_count);
			}
		}
	}

	/*
	 * Step 2 & 3: Refresh alive tasks, pick the minimum-vruntime process,
	 * and resume it via SIGCONT. Retry boundedly if a chosen task exits in
	 * the microsecond window between selection and signal delivery.
	 */
	for (attempts = 0; attempts < cfs_sched.count; attempts++) {
		next_idx = cfs_select_next_locked(&active_count, &total_weight);
		if (next_idx < 0 || active_count == 0)
			break;

		if (cfs_signal_pid(cfs_sched.procs[next_idx].pid, SIGCONT)) {
			next_slice_ms = cfs_calc_slice_ms(cfs_sched.base_quantum_ms,
							  active_count,
							  cfs_sched.procs[next_idx].weight,
							  total_weight);
			cfs_sched.procs[next_idx].last_start_jiffies = now;
			cfs_sched.current_idx = next_idx;

			printk(KERN_INFO CFS_LOG_PREFIX "Tick %04llu | Preempt PID %-6d (slice=%3u ms, +dvrt=%4llu, vrt=%7llu) -> Next PID %-6d (weight=%-4u, vrt=%7llu, next_slice=%3u ms)\n",
			       cfs_sched.tick_count,
			       preempted_pid,
			       actual_slice_ms,
			       delta_vruntime,
			       preempted_vrt,
			       cfs_sched.procs[next_idx].pid,
			       cfs_sched.procs[next_idx].weight,
			       cfs_sched.procs[next_idx].vruntime,
			       next_slice_ms);

			mod_timer(&cfs_sched.timer,
				  now + msecs_to_jiffies(next_slice_ms));
			spin_unlock_irqrestore(&cfs_sched.lock, flags);
			return;
		}

		/* Task exited right before SIGCONT; mark inactive and re-select */
		cfs_sched.procs[next_idx].active = false;
		printk(KERN_INFO CFS_LOG_PREFIX "Tick %04llu | PID %-6d exited prior to dispatch (final vrt=%llu)\n",
		       cfs_sched.tick_count,
		       cfs_sched.procs[next_idx].pid,
		       cfs_sched.procs[next_idx].vruntime);
	}

	/* Step 4: All registered processes have terminated; stop scheduling cleanly */
	cfs_sched.running = false;
	cfs_sched.current_idx = -1;
	cfs_log_summary_locked();

	spin_unlock_irqrestore(&cfs_sched.lock, flags);
}

SYSCALL_DEFINE3(cfs_timeshare,
		struct cfs_proc_arg __user *, user_procs,
		int, count,
		int, base_quantum_ms)
{
	struct cfs_proc_arg kprocs[MAX_CFS_PROCS];
	unsigned long flags;
	unsigned long now;
	int i, j;
	int alive_count = 0;
	u64 total_weight = 0;
	int first_idx = -1;
	u32 first_slice_ms;

	if (!user_procs)
		return -EFAULT;

	if (count <= 0 || count > MAX_CFS_PROCS)
		return -EINVAL;

	if (base_quantum_ms < CFS_MIN_QUANTUM_MS ||
	    base_quantum_ms > CFS_MAX_QUANTUM_MS)
		return -EINVAL;

	/* Copy process descriptors from user space before taking any spinlock */
	if (copy_from_user(kprocs, user_procs,
			   (size_t)count * sizeof(struct cfs_proc_arg)))
		return -EFAULT;

	/* Validate PIDs, nice values, and reject duplicate PID entries */
	for (i = 0; i < count; i++) {
		if (kprocs[i].pid <= 0)
			return -EINVAL;
		if (kprocs[i].nice_val < CFS_NICE_MIN ||
		    kprocs[i].nice_val > CFS_NICE_MAX)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			if (kprocs[j].pid == kprocs[i].pid)
				return -EINVAL;
		}
	}

	mutex_lock(&cfs_syscall_mutex);

	/*
	 * Safely quiesce any previously active timer session before modifying
	 * scheduler state. Never hold cfs_sched.lock across timer_delete_sync().
	 */
	if (!cfs_sched.timer_initialized) {
		timer_setup(&cfs_sched.timer, cfs_timer_callback, 0);
		cfs_sched.timer_initialized = true;
	} else {
		spin_lock_irqsave(&cfs_sched.lock, flags);
		cfs_sched.running = false;
		spin_unlock_irqrestore(&cfs_sched.lock, flags);
		timer_delete_sync(&cfs_sched.timer);
	}

	spin_lock_irqsave(&cfs_sched.lock, flags);

	memset(cfs_sched.procs, 0, sizeof(cfs_sched.procs));
	cfs_sched.count = count;
	cfs_sched.base_quantum_ms = base_quantum_ms;
	cfs_sched.tick_count = 0;
	cfs_sched.current_idx = -1;
	cfs_sched.running = false;

	now = jiffies;

	for (i = 0; i < count; i++) {
		struct cfs_proc_entry *p = &cfs_sched.procs[i];

		p->pid = kprocs[i].pid;
		p->nice_val = kprocs[i].nice_val;
		p->weight = cfs_nice_to_weight(kprocs[i].nice_val);
		p->vruntime = 0;
		p->exec_count = 0;
		p->total_runtime_ms = 0;
		p->last_start_jiffies = now;

		if (cfs_pid_is_alive(p->pid)) {
			p->active = true;
			alive_count++;
			total_weight += p->weight;
		} else {
			p->active = false;
		}
	}

	if (alive_count == 0) {
		spin_unlock_irqrestore(&cfs_sched.lock, flags);
		mutex_unlock(&cfs_syscall_mutex);
		return -ESRCH;
	}

	/*
	 * Select the initial process (all start with vruntime = 0; tie-break
	 * selects the highest-weight task first). Ensure all other active
	 * processes are paused via SIGSTOP, then resume the selected task.
	 */
	for (i = 0; i < count; i++) {
		if (!cfs_sched.procs[i].active)
			continue;
		if (first_idx < 0 ||
		    cfs_sched.procs[i].weight > cfs_sched.procs[first_idx].weight)
			first_idx = i;
	}

	for (i = 0; i < count; i++) {
		if (!cfs_sched.procs[i].active || i == first_idx)
			continue;
		if (!cfs_signal_pid(cfs_sched.procs[i].pid, SIGSTOP)) {
			cfs_sched.procs[i].active = false;
			alive_count--;
			total_weight -= cfs_sched.procs[i].weight;
		}
	}

	if (first_idx < 0 || !cfs_signal_pid(cfs_sched.procs[first_idx].pid, SIGCONT)) {
		spin_unlock_irqrestore(&cfs_sched.lock, flags);
		mutex_unlock(&cfs_syscall_mutex);
		return -ESRCH;
	}

	first_slice_ms = cfs_calc_slice_ms(base_quantum_ms, alive_count,
					   cfs_sched.procs[first_idx].weight,
					   total_weight);
	cfs_sched.procs[first_idx].last_start_jiffies = now;
	cfs_sched.current_idx = first_idx;
	cfs_sched.running = true;

	printk(KERN_INFO CFS_LOG_PREFIX "========================================================================\n");
	printk(KERN_INFO CFS_LOG_PREFIX "Session started: registered=%d, alive=%d, base_quantum=%d ms\n",
	       count, alive_count, base_quantum_ms);
	for (i = 0; i < count; i++) {
		const struct cfs_proc_entry *p = &cfs_sched.procs[i];

		printk(KERN_INFO CFS_LOG_PREFIX "  slot=%02d | PID %-6d | nice=%+3d | weight=%-5u | vruntime=0 | %s\n",
		       i, p->pid, p->nice_val, p->weight,
		       p->active ? "ACTIVE" : "DEAD");
	}
	printk(KERN_INFO CFS_LOG_PREFIX "Initial dispatch -> PID %-6d (nice=%+d, weight=%u, slice=%u ms)\n",
	       cfs_sched.procs[first_idx].pid,
	       cfs_sched.procs[first_idx].nice_val,
	       cfs_sched.procs[first_idx].weight,
	       first_slice_ms);

	mod_timer(&cfs_sched.timer, now + msecs_to_jiffies(first_slice_ms));

	spin_unlock_irqrestore(&cfs_sched.lock, flags);
	mutex_unlock(&cfs_syscall_mutex);

	return 0;
}

# System Architecture — CFS-Inspired Virtual Runtime Time-Sharing Scheduler

This document details the end-to-end architecture, kernel-to-user-space boundary, virtual runtime (`vruntime`) mathematical model, concurrency invariants, and process lifecycle of the custom **Linux 7.0 (`x86_64`)** time-sharing scheduler targeting **Ubuntu Server 26.04 LTS ("Resolute Raccoon")**.

---

## 1. End-to-End Architecture Diagram

```text
+======================================================================================+
|                                   USER SPACE                                         |
|                                                                                      |
|  +-------------------------------------------+      +-----------------------------+  |
|  |  Orchestrator (userspace/main.c)          |      | ANSI Renderer (ui.c / ui.h) |  |
|  |  - CLI flags (-n, -q, -w, -p) / TUI menu  |----->| - Live 20 Hz progress bars  |  |
|  |  - CPU work unit calibration              |      | - Contention snapshot table |  |
|  |  - fork() N workers + WUNTRACED barrier   |      | - Fairness vs Weight report |  |
|  |  - waitpid(WNOHANG|WUNTRACED|WCONTINUED)  |      +-----------------------------+  |
|  +---------------------+---------------------+                                       |
|                        |                                                             |
|        mmap(MAP_SHARED | MAP_ANONYMOUS)  <--- C11 _Atomic progress & CPU telemetry   |
|                        |                                                             |
|     +------------------+------------------+------------------+                       |
|     v                  v                  v                  v                       |
|  +------------+     +------------+     +------------+     +------------+             |
|  | Worker #0  |     | Worker #1  |     | Worker #2  |     | Worker #N  |             |
|  | (HIGH -5)  |     | (NORM  0)  |     | (LOW  +5)  | ... | (nice ...) |             |
|  | worker.c   |     | worker.c   |     | worker.c   |     | worker.c   |             |
|  | - SIGUSR1  |     | - SIGUSR1  |     | - SIGUSR1  |     | - SIGUSR1  |             |
|  | - SIGTERM  |     | - SIGTERM  |     | - SIGTERM  |     | - SIGTERM  |             |
|  | - SIGSTOP  |     | - SIGSTOP  |     | - SIGSTOP  |     | - SIGSTOP  |             |
|  +------------+     +------------+     +------------+     +------------+             |
|         ^                  ^                  ^                  ^                   |
+=========|==================|==================|==================|===================+
          |                  |                  |                  |
          |  SIGSTOP / SIGCONT job-control signals from kernel     |
          |                  |                  |                  |
   =======|==================|==================|==================|================
   SYSCALL BOUNDARY: syscall(__NR_cfs_timeshare = 500, args[], count, base_quantum_ms)
   Shared ABI Header: include/cfs_shared.h  (struct cfs_proc_arg { pid_t; int; })
   Dispatch Table:    arch/x86/entry/syscalls/syscall_64.tbl -> __x64_sys_cfs_timeshare
   =================================================================================
          |                  |                  |                  |
+=========|==================|==================|==================|===================+
|         |                  |   KERNEL SPACE (Linux 7.0 — Ubuntu 26.04 LTS)           |
|         |                  |                  |                  |                   |
|  +--------------------------------------------------------------------------------+  |
|  |  SYSCALL_DEFINE3(cfs_timeshare, user_procs, count, base_quantum_ms)            |  |
|  |  (kernel/cfs_timeshare/cfs_timeshare.c)                                        |  |
|  |   1. Validate args (1 <= count <= 16, 10 <= quantum <= 2000, no dup PIDs)      |  |
|  |   2. copy_from_user() into kernel stack array kprocs[MAX_CFS_PROCS]            |  |
|  |   3. mutex_lock(&cfs_syscall_mutex) -> timer_delete_sync(&cfs_sched.timer)     |  |
|  |   4. spin_lock_irqsave(&cfs_sched.lock) -> map nice -> weight, pick 1st task   |  |
|  |   5. SIGSTOP peers, SIGCONT 1st task, mod_timer() -> return 0                  |  |
|  +---------------------------------------+----------------------------------------+  |
|                                          |                                           |
|                                          v                                           |
|  +--------------------------------------------------------------------------------+  |
|  |  Scheduler State (protected by spinlock_t cfs_sched.lock)                      |  |
|  |  procs[0..15]: { pid, nice_val, weight, vruntime, exec_count,                  |  |
|  |                  total_runtime_ms, last_start_jiffies, active }                |  |
|  +---------------------------------------+----------------------------------------+  |
|                                          ^                                           |
|                                          | softirq timer expiration                  |
|  +---------------------------------------+----------------------------------------+  |
|  |  Kernel Timer Engine: cfs_timer_callback(struct timer_list *t)                 |  |
|  |   1. spin_lock_irqsave(&cfs_sched.lock)                                        |  |
|  |   2. Preempt current task:                                                     |  |
|  |        rcu_read_lock() -> pid_task(find_vpid(pid), PIDTYPE_PID)                |  |
|  |        -> send_sig(SIGSTOP, task, 1) -> rcu_read_unlock()                      |  |
|  |   3. Accumulate vruntime:                                                      |  |
|  |        delta_vruntime = (actual_slice_ms * 1024) / weight                      |  |
|  |   4. Prune exited/zombie tasks (!pid_alive || EXIT_ZOMBIE | EXIT_DEAD)         |  |
|  |   5. Select alive task with MINIMUM vruntime (tie-break: higher weight)        |  |
|  |   6. Resume next task via send_sig(SIGCONT, task, 1)                           |  |
|  |   7. Compute dynamically weighted slice_ms & re-arm via mod_timer()            |  |
|  |   8. printk(KERN_INFO "[CFS_SYS] ...")  ===>  /dev/kmsg  ===>  dmesg -w        |  |
|  |   9. If 0 active tasks remain: stop timer & log final summary table            |  |
|  +--------------------------------------------------------------------------------+  |
+======================================================================================+
```

---

## 2. Virtual Runtime (`vruntime`) Mathematics

### 2.1 Priority-to-Weight Mapping

Following the Linux kernel's Completely Fair Scheduler (`kernel/sched/core.c`: `sched_prio_to_weight[40]`), each nice level $n \in [-20, +19]$ maps geometrically to a positive integer weight $w(n)$:

$$w(n) \approx \frac{1024}{1.25^n}$$

Each increment of `+1` nice reduces a process's CPU share by roughly 10% (factor of $1.25\times$ ratio between adjacent levels). The three canonical classes used by the simulator are:

| Priority Class | Nice Value | Weight ($w_i$) | Relative Weight vs. Normal | Relative Weight vs. Low |
| :--- | :---: | :---: | :---: | :---: |
| **HIGH** | `-5` | `3121` | `3.05x` | `9.32x` |
| **NORMAL** | `0` | `1024` (`NICE_0_LOAD`) | `1.00x` | `3.06x` |
| **LOW** | `+5` | `335` | `0.33x` | `1.00x` |

### 2.2 Virtual Runtime Accumulation

Whenever the currently running process $i$ is preempted after running for `actual_slice_ms` milliseconds (measured from `jiffies_to_msecs(now - last_start_jiffies)`, floored at `1 ms`), its virtual runtime advances by:

$$\Delta \text{vruntime}_i = \left\lfloor \frac{\text{actual\_slice\_ms} \times \text{NICE\_0\_LOAD}}{w_i} \right\rfloor, \quad \text{where } \text{NICE\_0\_LOAD} = 1024$$

$$\text{vruntime}_i \leftarrow \text{vruntime}_i + \max(1, \Delta \text{vruntime}_i)$$

Notice the inverse proportionality to $w_i$:
- For a **NORMAL** process ($w = 1024$), virtual time advances at exact wall-clock rate ($1:1$).
- For a **HIGH** process ($w = 3121$), virtual time advances **~3.05x slower** than wall-clock time.
- For a **LOW** process ($w = 335$), virtual time advances **~3.06x faster** than wall-clock time.

### 2.3 Core CFS Fairness Invariant & Minimum-`vruntime` Selection

At every scheduling tick, the kernel scans all active, live processes and selects:

$$i^* = \arg\min_{i \in \text{Active}} \text{vruntime}_i$$

Ties in `vruntime` (such as the initial tick when all tasks begin at `vruntime = 0`) are broken deterministically in favor of the higher weight $w_i$, and then by lower slot index.

Because every process's `vruntime` increases strictly while it executes and stays frozen while it waits, **starvation is mathematically impossible**: even the lowest-priority process eventually becomes the minimum-`vruntime` task and is guaranteed CPU execution.

### 2.4 Dynamically Weighted Time Slice

Rather than assigning every process an identical static quantum (like plain Round-Robin), the scheduler computes a target scheduling latency across all currently active tasks:

$$L_{\text{sched}} = Q_{\text{base}} \times N_{\text{active}}$$

Each selected process $i^*$ receives a weighted share of $L_{\text{sched}}$ clamped to safe operating bounds:

$$\text{slice}_{i^*} = \text{clamp}\!\left(\left\lfloor \frac{L_{\text{sched}} \times w_{i^*}}{\sum_{j \in \text{Active}} w_j} \right\rfloor,\ \min(20, Q_{\text{base}}),\ 1000\right)\ \text{ms}$$

#### Worked Numerical Example (3 Processes, $Q_{\text{base}} = 100\text{ ms}$)

Consider 1 HIGH (`3121`), 1 NORMAL (`1024`), and 1 LOW (`335`) worker competing concurrently:
- $N_{\text{active}} = 3$, $\sum w_j = 3121 + 1024 + 335 = 4480$
- $L_{\text{sched}} = 100 \times 3 = 300\text{ ms}$

| Tick | Dispatched Task | Weight | Weighted Slice | $\Delta\text{vruntime}$ | New `vruntime` | Expected Share | Actual Slice Share |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| Init | **HIGH** | `3121` | `209 ms` | — | `0` | `69.7%` | — |
| 1 | Preempt HIGH -> **NORMAL** | `1024` | `68 ms` | `+68` (HIGH) | HIGH=`68`, NORM=`0`, LOW=`0` | `22.9%` | `209 / 299 = 69.9%` |
| 2 | Preempt NORMAL -> **LOW** | `335` | `22 ms` | `+68` (NORM) | HIGH=`68`, NORM=`68`, LOW=`0` | `7.5%` | `68 / 299 = 22.7%` |
| 3 | Preempt LOW -> **HIGH** | `3121` | `209 ms` | `+67` (LOW) | HIGH=`68`, NORM=`68`, LOW=`67` | `69.7%` | `22 / 299 = 7.4%` |

Look at the result after one full scheduling period:
1. All three processes executed within a bounded `~300 ms` latency period (**zero starvation**).
2. All three processes converged to nearly identical `vruntime` values (`68`, `68`, `67`).
3. If `slice` is ever clamped up to `CFS_MIN_SLICE_MS` (`20 ms`) for a low-weight task, its `vruntime` jumps ahead of higher-weight peers; the minimum-`vruntime` selector automatically skips it on subsequent ticks until higher-weight tasks catch up, preserving exact long-term proportional CPU shares.

---

## 3. Concurrency, Locking & Atomic Context Safety

Kernel timer callbacks execute in **softirq (atomic) context** where sleeping is strictly forbidden. This implementation enforces five strict kernel synchronization invariants:

| Mechanism | Context | Purpose & Invariant |
| :--- | :--- | :--- |
| `copy_from_user()` | Process context (`sys_cfs_timeshare`), **no locks held** | User-memory faults may sleep; copy completes before taking `cfs_syscall_mutex` or `cfs_sched.lock`. |
| `DEFINE_MUTEX(cfs_syscall_mutex)` | Process context (`sys_cfs_timeshare` only) | Serializes concurrent syscall invocations so `timer_delete_sync()` never races with another caller. Never acquired in timer softirq. |
| `timer_delete_sync(&cfs_sched.timer)` | Process context under `cfs_syscall_mutex`, **outside `cfs_sched.lock`** | Waits for any in-flight softirq timer callback on another CPU to exit. Calling it outside `cfs_sched.lock` makes AB-BA deadlock impossible (uses Linux 7.0's native `timer_delete_sync()` API). |
| `spin_lock_irqsave(&cfs_sched.lock)` | Both `sys_cfs_timeshare` and `cfs_timer_callback` | Protects the 16-slot scheduler table, `tick_count`, `current_idx`, and `running` flag across SMP CPUs and local interrupts without sleeping. |
| `rcu_read_lock()` / `rcu_read_unlock()` | Inside `cfs_signal_pid()` and `cfs_pid_is_alive()` | Safely resolves `pid_task(find_vpid(pid), PIDTYPE_PID)`, checks `pid_alive()` + `exit_state` (`EXIT_ZOMBIE \| EXIT_DEAD`) + `PF_EXITING`, and invokes non-sleeping `send_sig()`. |
| C11 `<stdatomic.h>` (`_Atomic`) | User-space anonymous `mmap` shared region | Lock-free acquire/release progress and CPU-time updates between forked workers and the parent dashboard renderer. |

---

## 4. Worker Process Lifecycle State Machine

```text
  [Parent: fork()]
         |
         v
   +-----------+   install sigaction(SIGTERM, SIGUSR1, SIG_IGN on SIGINT)
   |   INIT    |   atomic_store(ready = 1) -> raise(SIGSTOP)
   +-----+-----+
         |  (Parent observes WIFSTOPPED via waitpid(..., WUNTRACED))
         v
   +-----------+ <--------------------------------------------+
   |  WAITING  |  Paused by SIGSTOP (in kernel CFS runqueue)  |
   +-----+-----+                                              |
         |  Kernel timer sends SIGCONT                        | Kernel timer sends SIGSTOP
         |  (Parent observes WIFCONTINUED)                    | (Parent observes WIFSTOPPED)
         v                                                    |
   +-----------+ ---------------------------------------------+
   |  RUNNING  |  Executes calibrated CPU-bound work units
   +-----+-----+  Updates atomic completed_work & cpu_time_us
         |
         +---(all work units complete)---> [ _exit(0) ] ---> Parent reaps (DONE)
         |                                                   Kernel prunes dead/zombie task
         +---(SIGTERM from Ctrl+C)-------> [ _exit(0) ] ---> Parent reaps (ABORTED)
```

---

## 5. Key Files Map

| File | Responsibility |
| :--- | :--- |
| [include/cfs_shared.h](../include/cfs_shared.h) | Dual kernel/user-space ABI (`__NR_cfs_timeshare = 500`, `MAX_CFS_PROCS = 16`, `struct cfs_proc_arg`, priority/weight constants). |
| [kernel/cfs_timeshare/cfs_timeshare.c](../kernel/cfs_timeshare/cfs_timeshare.c) | Custom Linux 7.0 syscall (`SYSCALL_DEFINE3`), CFS weight table, `timer_list` tick engine, `timer_delete_sync()`, RCU signal dispatch, and `dmesg` summary logger. |
| [kernel/cfs_timeshare/Makefile](../kernel/cfs_timeshare/Makefile) | Kbuild integration (`obj-y += cfs_timeshare.o`). |
| [userspace/main.c](../userspace/main.c) | CLI parser, interactive wizard, `mmap` shared state, deterministic `WUNTRACED` barrier, syscall invocation, and `waitpid` event loop. |
| [userspace/worker.c](../userspace/worker.c) / [userspace/worker.h](../userspace/worker.h) | Signal handlers (`SIGUSR1`, `SIGTERM`), calibrated CPU-bound workload, and atomic telemetry updates. |
| [userspace/ui.c](../userspace/ui.c) / [userspace/ui.h](../userspace/ui.h) | Flicker-free ANSI terminal dashboard and final fairness-vs-weight report. |
| [scripts/patch_kernel.sh](../scripts/patch_kernel.sh) | Idempotent Ubuntu 26.04 Linux 7.0 (`linux-source-7.0.0`) source patcher (sources, `Kbuild`, `syscalls.h`, `syscall_64.tbl`, `EXTRAVERSION`). |
| [scripts/run_demo.sh](../scripts/run_demo.sh) | Automated `dmesg -C` + 2-pane `tmux` split-screen demo launcher (`sudo-rs` aware). |

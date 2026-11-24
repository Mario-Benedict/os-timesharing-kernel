/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cfs_shared.h - Shared ABI definitions between the custom Linux 6.6 LTS
 *                CFS-inspired virtual runtime syscall and user-space simulator.
 *
 * This header is designed to compile cleanly in both kernel space (__KERNEL__)
 * and POSIX/C11 user space.
 */

#ifndef CFS_SHARED_H
#define CFS_SHARED_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/types.h>
#endif

/* Custom x86_64 system call number registered in syscall_64.tbl */
#ifndef __NR_cfs_timeshare
#define __NR_cfs_timeshare 462
#endif

/* Maximum number of processes managed in a single scheduling session */
#define MAX_CFS_PROCS 16

/* Standard CFS priority nice levels */
#define CFS_NICE_HIGH   (-5)
#define CFS_NICE_NORMAL (0)
#define CFS_NICE_LOW    (5)
#define CFS_NICE_MIN    (-20)
#define CFS_NICE_MAX    (19)

/* CFS weight constants matching Linux's sched_prio_to_weight table */
#define CFS_NICE_0_LOAD   1024U
#define CFS_WEIGHT_HIGH   3121U
#define CFS_WEIGHT_NORMAL 1024U
#define CFS_WEIGHT_LOW    335U

/* Base quantum validation bounds and dynamic time-slice clamps (milliseconds) */
#define CFS_MIN_QUANTUM_MS     10
#define CFS_MAX_QUANTUM_MS     2000
#define CFS_DEFAULT_QUANTUM_MS 100
#define CFS_MIN_SLICE_MS       20
#define CFS_MAX_SLICE_MS       1000

/**
 * struct cfs_proc_arg - User-to-kernel process descriptor passed to sys_cfs_timeshare.
 * @pid:      Target process ID to manage (must be > 0).
 * @nice_val: CFS nice value in [-20, 19] (e.g., -5 High, 0 Normal, +5 Low).
 */
struct cfs_proc_arg {
	pid_t pid;
	int nice_val;
};

#endif /* CFS_SHARED_H */

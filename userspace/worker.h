/* SPDX-License-Identifier: GPL-2.0 */
/*
 * worker.h - Shared-memory state structures, priority helpers, and worker
 *            process declarations for the CFS time-sharing user-space simulator.
 */

#ifndef CFS_WORKER_H
#define CFS_WORKER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "cfs_shared.h"

/* Target pure CPU duration (ms) for a single calibrated work unit */
#define CFS_WORK_UNIT_TARGET_MS 25U

/* Default simulation parameters */
#define CFS_DEFAULT_NUM_PROCS  6
#define CFS_DEFAULT_WORK_UNITS 80
#define CFS_MIN_WORK_UNITS     5
#define CFS_MAX_WORK_UNITS     1000

enum cfs_worker_state {
	CFS_STATE_INIT = 0,
	CFS_STATE_STOPPED,
	CFS_STATE_RUNNING,
	CFS_STATE_DONE,
	CFS_STATE_ABORTED
};

struct cfs_worker_slot {
	/* Static metadata initialized by parent before fork() */
	int id;
	pid_t pid;
	int nice_val;
	uint32_t weight;
	uint64_t total_work;

	/* Lock-free atomic progress counters updated by worker, read by parent */
	_Atomic uint64_t completed_work;
	_Atomic uint64_t cpu_time_us;
	_Atomic uint64_t sigusr1_count;
	_Atomic int ready;
	_Atomic int aborted;

	/* Parent-owned lifecycle tracking (updated via waitpid notifications) */
	enum cfs_worker_state state;
	uint64_t start_mono_us;
	uint64_t finish_mono_us;
	uint64_t dispatch_count;
	uint64_t preempt_count;
	int finish_rank;

	/* Contention-window snapshot taken when the first worker completes */
	uint64_t snap_work;
	uint64_t snap_cpu_us;
};

struct cfs_shm_region {
	int num_procs;
	int base_quantum_ms;
	uint64_t total_work_units;
	uint64_t iters_per_unit;
	uint64_t session_start_us;
	uint64_t session_end_us;
	bool snapshot_taken;
	uint64_t snapshot_elapsed_us;
	struct cfs_worker_slot workers[MAX_CFS_PROCS];
};

/* Monotonic wall-clock and process CPU-time helpers (microseconds) */
uint64_t cfs_mono_time_us(void);
uint64_t cfs_proc_cpu_time_us(void);

/* CFS priority nice -> weight & label helpers (mirrors kernel table) */
uint32_t cfs_user_nice_to_weight(int nice_val);
const char *cfs_priority_label(int nice_val);
const char *cfs_state_label(enum cfs_worker_state state);

/*
 * Calibrate the number of inner CPU-bound arithmetic iterations required to
 * consume approximately target_ms milliseconds of pure CPU time.
 */
uint64_t cfs_calibrate_work_unit(unsigned int target_ms);

/*
 * Entry point executed inside each forked child worker process.
 * Does not return; terminates cleanly via _exit().
 */
void cfs_worker_run(struct cfs_shm_region *shm, int slot_idx);

#endif /* CFS_WORKER_H */

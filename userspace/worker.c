/* SPDX-License-Identifier: GPL-2.0 */
/*
 * worker.c - Worker process implementation for the CFS time-sharing simulator.
 *
 * Each worker installs POSIX sigaction handlers for SIGUSR1 (status flush)
 * and SIGTERM (graceful abort), ignores SIGINT so the parent coordinates
 * shutdown cleanly, signals readiness in anonymous shared memory, immediately
 * stops itself with raise(SIGSTOP), and then runs calibrated CPU-bound work
 * units whenever resumed via SIGCONT by the kernel CFS syscall.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "worker.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Async-signal-safe flags set by worker signal handlers */
static volatile sig_atomic_t g_worker_terminate = 0;
static volatile sig_atomic_t g_worker_status_ping = 0;

/* Mirrors kernel/cfs_timeshare/cfs_timeshare.c cfs_prio_to_weight[40] */
static const uint32_t g_user_prio_to_weight[40] = {
	88761U, 71755U, 56483U, 46273U, 36291U,
	29154U, 23254U, 18705U, 14949U, 11916U,
	 9548U,  7620U,  6100U,  4904U,  3906U,
	 3121U,  2501U,  1991U,  1586U,  1277U,
	 1024U,   820U,   655U,   526U,   423U,
	  335U,   272U,   215U,   172U,   137U,
	  110U,    87U,    70U,    56U,    45U,
	   36U,    29U,    23U,    18U,    15U
};

/* Sink prevents GCC -O2 from optimizing away CPU-bound calibration/work loops */
static volatile uint64_t g_cpu_burn_sink = 0;

static void worker_sigterm_handler(int signo)
{
	(void)signo;
	g_worker_terminate = 1;
}

static void worker_sigusr1_handler(int signo)
{
	(void)signo;
	g_worker_status_ping = 1;
}

uint64_t cfs_mono_time_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

uint64_t cfs_proc_cpu_time_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

uint32_t cfs_user_nice_to_weight(int nice_val)
{
	int clamped = nice_val;

	if (clamped < CFS_NICE_MIN)
		clamped = CFS_NICE_MIN;
	if (clamped > CFS_NICE_MAX)
		clamped = CFS_NICE_MAX;
	return g_user_prio_to_weight[clamped - CFS_NICE_MIN];
}

const char *cfs_priority_label(int nice_val)
{
	if (nice_val <= CFS_NICE_HIGH)
		return "HIGH";
	if (nice_val >= CFS_NICE_LOW)
		return "LOW";
	if (nice_val == CFS_NICE_NORMAL)
		return "NORMAL";
	return (nice_val < 0) ? "ABOVE" : "BELOW";
}

const char *cfs_state_label(enum cfs_worker_state state)
{
	switch (state) {
	case CFS_STATE_INIT:
		return "INIT";
	case CFS_STATE_STOPPED:
		return "WAITING";
	case CFS_STATE_RUNNING:
		return "RUNNING";
	case CFS_STATE_DONE:
		return "DONE";
	case CFS_STATE_ABORTED:
		return "ABORTED";
	default:
		return "UNKNOWN";
	}
}

/*
 * Deterministic integer CPU-bound kernel (xorshift64* + FNV-style mixing).
 * Uses only 64-bit integer operations for consistent speed across VMs.
 */
static uint64_t burn_cpu_iterations(uint64_t iterations, uint64_t seed)
{
	uint64_t acc = seed ^ 0x9E3779B97F4A7C15ULL;
	uint64_t i;

	for (i = 0; i < iterations; i++) {
		acc ^= acc >> 12;
		acc ^= acc << 25;
		acc ^= acc >> 27;
		acc = acc * 0x2545F4914F6CDD1DULL + 0x100000001B3ULL;
	}

	g_cpu_burn_sink ^= acc;
	return acc;
}

uint64_t cfs_calibrate_work_unit(unsigned int target_ms)
{
	const uint64_t probe_iters = 1000000ULL;
	uint64_t start_us;
	uint64_t elapsed_us;
	uint64_t total_iters = 0;
	uint64_t seed = 0xCAFEBABE12345678ULL;
	uint64_t target_us = (uint64_t)target_ms * 1000ULL;
	uint64_t scaled;

	/* Run probes until at least 40 ms of process CPU time have elapsed */
	start_us = cfs_proc_cpu_time_us();
	do {
		seed = burn_cpu_iterations(probe_iters, seed);
		total_iters += probe_iters;
		elapsed_us = cfs_proc_cpu_time_us() - start_us;
	} while (elapsed_us < 40000ULL && total_iters < 500000000ULL);

	if (elapsed_us == 0)
		elapsed_us = 1;

	scaled = (total_iters * target_us) / elapsed_us;
	if (scaled < 100000ULL)
		scaled = 100000ULL;

	return scaled;
}

static int install_worker_signals(void)
{
	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = worker_sigterm_handler;
	if (sigaction(SIGTERM, &sa, NULL) != 0)
		return -1;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sa.sa_handler = worker_sigusr1_handler;
	if (sigaction(SIGUSR1, &sa, NULL) != 0)
		return -1;

	/* Let the parent handle Ctrl+C and coordinate graceful SIGTERM shutdown */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGINT, &sa, NULL) != 0)
		return -1;

	return 0;
}

void cfs_worker_run(struct cfs_shm_region *shm, int slot_idx)
{
	struct cfs_worker_slot *slot = &shm->workers[slot_idx];
	uint64_t iters_per_unit = shm->iters_per_unit;
	uint64_t sub_chunks = 4;
	uint64_t chunk_iters;
	uint64_t unit;
	uint64_t c;
	uint64_t seed;
	uint64_t base_cpu_us;

	if (install_worker_signals() != 0) {
		atomic_store_explicit(&slot->aborted, 1, memory_order_release);
		_exit(EXIT_FAILURE);
	}

	/* Announce readiness and immediately self-stop until kernel SIGCONT */
	atomic_store_explicit(&slot->ready, 1, memory_order_release);
	if (raise(SIGSTOP) != 0) {
		atomic_store_explicit(&slot->aborted, 1, memory_order_release);
		_exit(EXIT_FAILURE);
	}

	/*
	 * Execution resumes here when the kernel CFS syscall dispatches this
	 * worker via SIGCONT. Measure CPU time accumulated strictly during work.
	 */
	base_cpu_us = cfs_proc_cpu_time_us();
	chunk_iters = (iters_per_unit >= sub_chunks) ? (iters_per_unit / sub_chunks) : 1ULL;
	seed = (uint64_t)getpid() * 0x9E3779B97F4A7C15ULL + (uint64_t)slot_idx;

	for (unit = 0; unit < slot->total_work; unit++) {
		for (c = 0; c < sub_chunks; c++) {
			if (g_worker_terminate) {
				uint64_t cur_cpu = cfs_proc_cpu_time_us() - base_cpu_us;

				atomic_store_explicit(&slot->cpu_time_us, cur_cpu,
						      memory_order_release);
				atomic_store_explicit(&slot->aborted, 1,
						      memory_order_release);
				_exit(EXIT_SUCCESS);
			}

			seed = burn_cpu_iterations(chunk_iters, seed);

			if (g_worker_status_ping) {
				uint64_t cur_cpu = cfs_proc_cpu_time_us() - base_cpu_us;

				g_worker_status_ping = 0;
				atomic_fetch_add_explicit(&slot->sigusr1_count, 1ULL,
							  memory_order_relaxed);
				atomic_store_explicit(&slot->cpu_time_us, cur_cpu,
						      memory_order_release);
			}
		}

		atomic_store_explicit(&slot->completed_work, unit + 1ULL,
				      memory_order_release);
		atomic_store_explicit(&slot->cpu_time_us,
				      cfs_proc_cpu_time_us() - base_cpu_us,
				      memory_order_release);
	}

	_exit(EXIT_SUCCESS);
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * main.c - Interactive CLI and process orchestrator for the CFS-Inspired
 *          Virtual Runtime (vruntime) Time-Sharing Scheduler simulator.
 *
 * Lifecycle:
 *   1. Parse CLI flags (-n, -q, -w, -p, -i, -h) or prompt interactively.
 *   2. Calibrate a CPU-bound work unit so runtime scales smoothly on any VM.
 *   3. Allocate anonymous shared memory via mmap(MAP_SHARED | MAP_ANONYMOUS).
 *   4. Fork N worker processes; each worker self-stops via raise(SIGSTOP).
 *   5. Synchronize via waitpid(..., WUNTRACED) until every worker is stopped.
 *   6. Invoke custom syscall(__NR_cfs_timeshare, args, count, base_quantum_ms).
 *   7. Monitor worker states via waitpid(-1, ..., WNOHANG|WUNTRACED|WCONTINUED)
 *      and render the real-time ANSI dashboard until all workers exit.
 *   8. Print the final fairness-vs-weight report and clean up all resources.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cfs_shared.h"
#include "ui.h"
#include "worker.h"

enum cfs_priority_profile {
	PROFILE_BALANCED = 0,
	PROFILE_SKEWED,
	PROFILE_EQUAL,
	PROFILE_CUSTOM
};

struct cfs_sim_config {
	int num_procs;
	int base_quantum_ms;
	uint64_t work_units;
	enum cfs_priority_profile profile;
	int custom_nice[MAX_CFS_PROCS];
	bool force_interactive;
};

static volatile sig_atomic_t g_parent_abort = 0;

static void parent_abort_handler(int signo)
{
	(void)signo;
	g_parent_abort = 1;
}

static int install_parent_signals(void)
{
	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = parent_abort_handler;

	if (sigaction(SIGINT, &sa, NULL) != 0)
		return -1;
	if (sigaction(SIGTERM, &sa, NULL) != 0)
		return -1;
	return 0;
}

static void sleep_ms(unsigned int ms)
{
	struct timespec req;

	req.tv_sec = (time_t)(ms / 1000U);
	req.tv_nsec = (long)(ms % 1000U) * 1000000L;
	(void)nanosleep(&req, NULL);
}

static void print_usage(const char *prog)
{
	printf("CFS-Inspired Virtual Runtime (vruntime) Time-Sharing Simulator\n\n");
	printf("Usage:\n");
	printf("  %s [options]\n\n", prog);
	printf("Options:\n");
	printf("  -n <num_procs>       Number of worker processes (%d..%d, default: %d)\n",
	       1, MAX_CFS_PROCS, CFS_DEFAULT_NUM_PROCS);
	printf("  -q <base_quantum_ms> Base scheduling quantum in ms (%d..%d, default: %d)\n",
	       CFS_MIN_QUANTUM_MS, CFS_MAX_QUANTUM_MS, CFS_DEFAULT_QUANTUM_MS);
	printf("  -w <work_units>      Calibrated CPU work units per worker (%d..%d, default: %d)\n",
	       CFS_MIN_WORK_UNITS, CFS_MAX_WORK_UNITS, CFS_DEFAULT_WORK_UNITS);
	printf("  -p <profile>         Priority distribution: balanced | skewed | equal (default: balanced)\n");
	printf("  -i                   Launch interactive configuration prompt\n");
	printf("  -h                   Display this help message and exit\n\n");
	printf("Priority Weights (Linux CFS sched_prio_to_weight):\n");
	printf("  HIGH   (nice %+d) -> weight %u\n", CFS_NICE_HIGH, CFS_WEIGHT_HIGH);
	printf("  NORMAL (nice %+d) -> weight %u\n", CFS_NICE_NORMAL, CFS_WEIGHT_NORMAL);
	printf("  LOW    (nice %+d) -> weight %u\n\n", CFS_NICE_LOW, CFS_WEIGHT_LOW);
	printf("Examples:\n");
	printf("  %s                       # Interactive prompt (Enter accepts defaults)\n", prog);
	printf("  %s -n 6 -q 100 -w 80     # 6 balanced workers, 100ms quantum, 80 units\n", prog);
	printf("  %s -n 4 -q 60 -w 100 -p skewed\n", prog);
}

static bool parse_int_strict(const char *str, int min_v, int max_v, int *out)
{
	char *end = NULL;
	long val;

	if (str == NULL || str[0] == '\0')
		return false;

	errno = 0;
	val = strtol(str, &end, 10);
	if (errno != 0 || end == str || *end != '\0')
		return false;
	if (val < (long)min_v || val > (long)max_v)
		return false;

	*out = (int)val;
	return true;
}

static bool parse_profile(const char *str, enum cfs_priority_profile *out)
{
	if (strcmp(str, "balanced") == 0) {
		*out = PROFILE_BALANCED;
		return true;
	}
	if (strcmp(str, "skewed") == 0) {
		*out = PROFILE_SKEWED;
		return true;
	}
	if (strcmp(str, "equal") == 0) {
		*out = PROFILE_EQUAL;
		return true;
	}
	return false;
}

static void assign_profile_nice_values(struct cfs_sim_config *cfg)
{
	int i;

	if (cfg->profile == PROFILE_CUSTOM)
		return;

	for (i = 0; i < cfg->num_procs; i++) {
		if (cfg->profile == PROFILE_EQUAL) {
			cfg->custom_nice[i] = CFS_NICE_NORMAL;
		} else if (cfg->profile == PROFILE_SKEWED) {
			if (i == 0)
				cfg->custom_nice[i] = CFS_NICE_HIGH;
			else if (i == 1)
				cfg->custom_nice[i] = CFS_NICE_NORMAL;
			else
				cfg->custom_nice[i] = CFS_NICE_LOW;
		} else {
			/* PROFILE_BALANCED: round-robin HIGH, NORMAL, LOW */
			int mod = i % 3;

			if (mod == 0)
				cfg->custom_nice[i] = CFS_NICE_HIGH;
			else if (mod == 1)
				cfg->custom_nice[i] = CFS_NICE_NORMAL;
			else
				cfg->custom_nice[i] = CFS_NICE_LOW;
		}
	}
}

static bool read_line_trimmed(char *buf, size_t size)
{
	size_t len;

	if (fgets(buf, (int)size, stdin) == NULL)
		return false;

	len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
			   isspace((unsigned char)buf[len - 1]))) {
		buf[--len] = '\0';
	}
	return true;
}

static int prompt_int(const char *label, int default_val, int min_v, int max_v)
{
	char line[64];
	int parsed;

	for (;;) {
		printf("  %s [%d-%d, default %d]: ", label, min_v, max_v, default_val);
		fflush(stdout);
		if (!read_line_trimmed(line, sizeof(line)))
			return default_val;
		if (line[0] == '\0')
			return default_val;
		if (parse_int_strict(line, min_v, max_v, &parsed))
			return parsed;
		printf("    Invalid input. Please enter an integer between %d and %d.\n",
		       min_v, max_v);
	}
}

static void run_interactive_prompt(struct cfs_sim_config *cfg)
{
	char line[64];
	int w_val;
	int i;

	printf("\n================================================================================\n");
	printf("  CFS-Inspired Virtual Runtime (vruntime) Time-Sharing Scheduler — Setup\n");
	printf("================================================================================\n");
	printf("  Press Enter at any prompt to accept the default value.\n\n");

	cfg->num_procs = prompt_int("Number of worker processes",
				    cfg->num_procs, 1, MAX_CFS_PROCS);
	cfg->base_quantum_ms = prompt_int("Base time quantum (ms)",
					  cfg->base_quantum_ms,
					  CFS_MIN_QUANTUM_MS, CFS_MAX_QUANTUM_MS);
	w_val = prompt_int("CPU work units per worker",
			   (int)cfg->work_units,
			   CFS_MIN_WORK_UNITS, CFS_MAX_WORK_UNITS);
	cfg->work_units = (uint64_t)w_val;

	printf("\n  Priority profile options:\n");
	printf("    1) balanced  - Equal mix of HIGH (-5), NORMAL (0), LOW (+5) [default]\n");
	printf("    2) skewed    - 1 HIGH, 1 NORMAL, remaining LOW\n");
	printf("    3) equal     - All NORMAL (0) for pure equal-weight fairness\n");
	printf("    4) custom    - Assign H/N/L individually per worker\n");

	for (;;) {
		printf("  Select priority profile [1-4, default 1]: ");
		fflush(stdout);
		if (!read_line_trimmed(line, sizeof(line)) || line[0] == '\0' ||
		    strcmp(line, "1") == 0) {
			cfg->profile = PROFILE_BALANCED;
			break;
		} else if (strcmp(line, "2") == 0) {
			cfg->profile = PROFILE_SKEWED;
			break;
		} else if (strcmp(line, "3") == 0) {
			cfg->profile = PROFILE_EQUAL;
			break;
		} else if (strcmp(line, "4") == 0) {
			cfg->profile = PROFILE_CUSTOM;
			for (i = 0; i < cfg->num_procs; i++) {
				for (;;) {
					printf("    Worker #%02d priority (H=High/-5, N=Normal/0, L=Low/+5) [N]: ",
					       i);
					fflush(stdout);
					if (!read_line_trimmed(line, sizeof(line)) ||
					    line[0] == '\0' ||
					    line[0] == 'n' || line[0] == 'N') {
						cfg->custom_nice[i] = CFS_NICE_NORMAL;
						break;
					} else if (line[0] == 'h' || line[0] == 'H') {
						cfg->custom_nice[i] = CFS_NICE_HIGH;
						break;
					} else if (line[0] == 'l' || line[0] == 'L') {
						cfg->custom_nice[i] = CFS_NICE_LOW;
						break;
					}
					printf("      Please enter H, N, or L.\n");
				}
			}
			break;
		}
		printf("    Invalid choice. Please enter 1, 2, 3, or 4.\n");
	}

	assign_profile_nice_values(cfg);
	printf("\n");
}

static int find_worker_by_pid(const struct cfs_shm_region *shm, pid_t pid)
{
	int i;

	for (i = 0; i < shm->num_procs; i++) {
		if (shm->workers[i].pid == pid)
			return i;
	}
	return -1;
}

static void capture_contention_snapshot(struct cfs_shm_region *shm,
					uint64_t now_us)
{
	int i;

	if (shm->snapshot_taken)
		return;

	shm->snapshot_taken = true;
	shm->snapshot_elapsed_us = (now_us >= shm->session_start_us)
				   ? (now_us - shm->session_start_us)
				   : 0;

	for (i = 0; i < shm->num_procs; i++) {
		struct cfs_worker_slot *w = &shm->workers[i];

		w->snap_work = atomic_load_explicit(&w->completed_work,
						    memory_order_acquire);
		w->snap_cpu_us = atomic_load_explicit(&w->cpu_time_us,
						      memory_order_acquire);
	}
}

/*
 * Terminate and reap all remaining child workers safely (e.g., on Ctrl+C or
 * syscall error). Sending SIGCONT right after SIGTERM ensures stopped tasks
 * wake up immediately to process SIGTERM and exit cleanly.
 */
static void terminate_and_reap_all(struct cfs_shm_region *shm)
{
	int i;
	int status;

	for (i = 0; i < shm->num_procs; i++) {
		pid_t pid = shm->workers[i].pid;

		if (pid <= 0)
			continue;
		if (shm->workers[i].state == CFS_STATE_DONE ||
		    shm->workers[i].state == CFS_STATE_ABORTED)
			continue;

		(void)kill(pid, SIGTERM);
		(void)kill(pid, SIGCONT);
	}

	for (i = 0; i < shm->num_procs; i++) {
		pid_t pid = shm->workers[i].pid;

		if (pid <= 0)
			continue;
		if (shm->workers[i].state == CFS_STATE_DONE ||
		    shm->workers[i].state == CFS_STATE_ABORTED)
			continue;

		for (;;) {
			pid_t r = waitpid(pid, &status, 0);

			if (r > 0) {
				if (WIFEXITED(status) || WIFSIGNALED(status)) {
					shm->workers[i].finish_mono_us = cfs_mono_time_us();
					shm->workers[i].state = CFS_STATE_ABORTED;
					break;
				}
			} else if (r == -1 && errno == EINTR) {
				continue;
			} else {
				break;
			}
		}
	}
}

int main(int argc, char *argv[])
{
	struct cfs_sim_config cfg;
	struct cfs_ui_ctx ui;
	struct cfs_shm_region *shm = NULL;
	struct cfs_proc_arg syscall_args[MAX_CFS_PROCS];
	int opt;
	int tmp_val;
	int i;
	int completed_count = 0;
	int next_finish_rank = 1;
	bool has_cli_flags = false;
	bool was_aborted = false;
	long sys_ret;

	memset(&cfg, 0, sizeof(cfg));
	cfg.num_procs = CFS_DEFAULT_NUM_PROCS;
	cfg.base_quantum_ms = CFS_DEFAULT_QUANTUM_MS;
	cfg.work_units = CFS_DEFAULT_WORK_UNITS;
	cfg.profile = PROFILE_BALANCED;
	cfg.force_interactive = false;

	while ((opt = getopt(argc, argv, "n:q:w:p:ih")) != -1) {
		switch (opt) {
		case 'n':
			if (!parse_int_strict(optarg, 1, MAX_CFS_PROCS, &cfg.num_procs)) {
				fprintf(stderr, "Error: -n must be an integer between 1 and %d.\n",
					MAX_CFS_PROCS);
				return EXIT_FAILURE;
			}
			has_cli_flags = true;
			break;
		case 'q':
			if (!parse_int_strict(optarg, CFS_MIN_QUANTUM_MS,
					      CFS_MAX_QUANTUM_MS, &cfg.base_quantum_ms)) {
				fprintf(stderr, "Error: -q must be between %d and %d ms.\n",
					CFS_MIN_QUANTUM_MS, CFS_MAX_QUANTUM_MS);
				return EXIT_FAILURE;
			}
			has_cli_flags = true;
			break;
		case 'w':
			if (!parse_int_strict(optarg, CFS_MIN_WORK_UNITS,
					      CFS_MAX_WORK_UNITS, &tmp_val)) {
				fprintf(stderr, "Error: -w must be between %d and %d work units.\n",
					CFS_MIN_WORK_UNITS, CFS_MAX_WORK_UNITS);
				return EXIT_FAILURE;
			}
			cfg.work_units = (uint64_t)tmp_val;
			has_cli_flags = true;
			break;
		case 'p':
			if (!parse_profile(optarg, &cfg.profile)) {
				fprintf(stderr, "Error: -p must be one of: balanced, skewed, equal.\n");
				return EXIT_FAILURE;
			}
			has_cli_flags = true;
			break;
		case 'i':
			cfg.force_interactive = true;
			break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	cfs_ui_init(&ui);

	if ((cfg.force_interactive || !has_cli_flags) && isatty(STDIN_FILENO)) {
		run_interactive_prompt(&cfg);
	} else {
		assign_profile_nice_values(&cfg);
	}

	if (install_parent_signals() != 0) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	printf("[SIM] Calibrating CPU-bound work unit (~%u ms/unit)...",
	       CFS_WORK_UNIT_TARGET_MS);
	fflush(stdout);

	shm = mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shm == MAP_FAILED) {
		perror("\nmmap");
		return EXIT_FAILURE;
	}

	shm->num_procs = cfg.num_procs;
	shm->base_quantum_ms = cfg.base_quantum_ms;
	shm->total_work_units = cfg.work_units;
	shm->iters_per_unit = cfs_calibrate_work_unit(CFS_WORK_UNIT_TARGET_MS);
	shm->snapshot_taken = false;

	printf(" calibrated (%llu iters/unit).\n",
	       (unsigned long long)shm->iters_per_unit);

	/* Initialize per-worker shared memory descriptors prior to fork() */
	for (i = 0; i < cfg.num_procs; i++) {
		struct cfs_worker_slot *w = &shm->workers[i];

		w->id = i;
		w->pid = -1;
		w->nice_val = cfg.custom_nice[i];
		w->weight = cfs_user_nice_to_weight(cfg.custom_nice[i]);
		w->total_work = cfg.work_units;
		atomic_init(&w->completed_work, 0ULL);
		atomic_init(&w->cpu_time_us, 0ULL);
		atomic_init(&w->sigusr1_count, 0ULL);
		atomic_init(&w->ready, 0);
		atomic_init(&w->aborted, 0);
		w->state = CFS_STATE_INIT;
		w->finish_rank = 0;
	}

	/* Fork all worker processes */
	for (i = 0; i < cfg.num_procs; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			terminate_and_reap_all(shm);
			munmap(shm, sizeof(*shm));
			return EXIT_FAILURE;
		}
		if (pid == 0) {
			cfs_worker_run(shm, i);
			/* Unreachable */
		}
		shm->workers[i].pid = pid;
	}

	/*
	 * Deterministic startup barrier: wait until every child has installed
	 * its signal handlers and paused itself via raise(SIGSTOP).
	 */
	for (i = 0; i < cfg.num_procs; i++) {
		int status = 0;
		pid_t pid = shm->workers[i].pid;

		for (;;) {
			pid_t r = waitpid(pid, &status, WUNTRACED);

			if (r == pid && WIFSTOPPED(status)) {
				shm->workers[i].state = CFS_STATE_STOPPED;
				break;
			}
			if (r == pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
				fprintf(stderr,
					"Error: worker #%02d (PID %d) exited unexpectedly during startup.\n",
					i, (int)pid);
				terminate_and_reap_all(shm);
				munmap(shm, sizeof(*shm));
				return EXIT_FAILURE;
			}
			if (r == -1 && errno == EINTR)
				continue;

			perror("waitpid(WUNTRACED)");
			terminate_and_reap_all(shm);
			munmap(shm, sizeof(*shm));
			return EXIT_FAILURE;
		}

		syscall_args[i].pid = pid;
		syscall_args[i].nice_val = shm->workers[i].nice_val;
	}

	/* Record start timestamps and hand control to the custom Linux syscall */
	shm->session_start_us = cfs_mono_time_us();
	for (i = 0; i < cfg.num_procs; i++)
		shm->workers[i].start_mono_us = shm->session_start_us;

	errno = 0;
	sys_ret = syscall(__NR_cfs_timeshare, syscall_args,
			  cfg.num_procs, cfg.base_quantum_ms);
	if (sys_ret != 0) {
		int saved_errno = errno;

		fprintf(stderr,
			"\n[ERROR] syscall(__NR_cfs_timeshare=%d) failed: %s (errno=%d)\n",
			__NR_cfs_timeshare, strerror(saved_errno), saved_errno);
		if (saved_errno == ENOSYS) {
			fprintf(stderr,
				"        Running kernel '%s' does not implement sys_cfs_timeshare (#%d).\n"
				"        Patch, compile, and boot Ubuntu Server 26.04 LTS Linux 7.0 using:\n"
				"          ./scripts/patch_kernel.sh ~/linux-source-7.0.0\n",
				ui.kernel_release, __NR_cfs_timeshare);
		}
		terminate_and_reap_all(shm);
		munmap(shm, sizeof(*shm));
		return EXIT_FAILURE;
	}

	/* Live monitoring loop driven by waitpid job-control events */
	cfs_ui_begin_dashboard(&ui);

	{
		unsigned int frame_count = 0;

		while (completed_count < cfg.num_procs) {
			int status = 0;
			pid_t wpid;

			if (g_parent_abort) {
				was_aborted = true;
				capture_contention_snapshot(shm, cfs_mono_time_us());
				terminate_and_reap_all(shm);
				break;
			}

			/* Drain all pending stop/continue/exit notifications */
			while ((wpid = waitpid(-1, &status,
					       WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
				int idx = find_worker_by_pid(shm, wpid);

				if (idx < 0)
					continue;

				if (WIFSTOPPED(status)) {
					shm->workers[idx].state = CFS_STATE_STOPPED;
					shm->workers[idx].preempt_count++;
				} else if (WIFCONTINUED(status)) {
					shm->workers[idx].state = CFS_STATE_RUNNING;
					shm->workers[idx].dispatch_count++;
				} else if (WIFEXITED(status) || WIFSIGNALED(status)) {
					uint64_t now_us = cfs_mono_time_us();

					/* Snapshot all workers at the exact first completion */
					capture_contention_snapshot(shm, now_us);

					shm->workers[idx].finish_mono_us = now_us;
					if (atomic_load_explicit(&shm->workers[idx].aborted,
								 memory_order_acquire) ||
					    WIFSIGNALED(status)) {
						shm->workers[idx].state = CFS_STATE_ABORTED;
					} else {
						shm->workers[idx].state = CFS_STATE_DONE;
						shm->workers[idx].finish_rank = next_finish_rank++;
					}
					completed_count++;
				}
			}

			/* Periodically ping active workers with SIGUSR1 to flush CPU telemetry */
			if ((frame_count % 10U) == 0U) {
				for (i = 0; i < cfg.num_procs; i++) {
					if (shm->workers[i].state == CFS_STATE_RUNNING)
						(void)kill(shm->workers[i].pid, SIGUSR1);
				}
			}
			frame_count++;

			cfs_ui_render_dashboard(&ui, shm,
						cfs_mono_time_us() - shm->session_start_us,
						completed_count);

			if (completed_count < cfg.num_procs)
				sleep_ms(50U);
		}
	}

	shm->session_end_us = cfs_mono_time_us();
	if (!shm->snapshot_taken)
		capture_contention_snapshot(shm, shm->session_end_us);

	/* Draw final frame and restore terminal cursor before printing report */
	cfs_ui_render_dashboard(&ui, shm,
				shm->session_end_us - shm->session_start_us,
				completed_count);
	cfs_ui_end_dashboard(&ui);

	cfs_ui_render_summary(&ui, shm, was_aborted);

	munmap(shm, sizeof(*shm));
	return was_aborted ? EXIT_FAILURE : EXIT_SUCCESS;
}

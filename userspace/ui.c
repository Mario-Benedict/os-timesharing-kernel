/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ui.c - Clean, colorized ANSI terminal dashboard and final CFS fairness
 *        summary renderer for the user-space simulator.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/* ANSI SGR color and control sequences */
#define ANSI_RESET       "\033[0m"
#define ANSI_BOLD        "\033[1m"
#define ANSI_DIM         "\033[2m"
#define ANSI_RED         "\033[31m"
#define ANSI_GREEN       "\033[32m"
#define ANSI_YELLOW      "\033[33m"
#define ANSI_BLUE        "\033[34m"
#define ANSI_MAGENTA     "\033[35m"
#define ANSI_CYAN        "\033[36m"
#define ANSI_B_RED       "\033[91m"
#define ANSI_B_GREEN     "\033[92m"
#define ANSI_B_YELLOW    "\033[93m"
#define ANSI_B_CYAN      "\033[96m"
#define ANSI_B_MAGENTA   "\033[95m"
#define ANSI_CLEAR_SCR   "\033[2J\033[H"
#define ANSI_HOME        "\033[H"
#define ANSI_CLEAR_EOL   "\033[K"
#define ANSI_CLEAR_BELOW "\033[J"
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"

#define BAR_WIDTH 24

static const char *c(const struct cfs_ui_ctx *ui, const char *code)
{
	return ui->use_color ? code : "";
}

static const char *priority_color(const struct cfs_ui_ctx *ui, int nice_val)
{
	if (!ui->use_color)
		return "";
	if (nice_val <= CFS_NICE_HIGH)
		return ANSI_BOLD ANSI_B_MAGENTA;
	if (nice_val >= CFS_NICE_LOW)
		return ANSI_YELLOW;
	return ANSI_B_CYAN;
}

static const char *state_color(const struct cfs_ui_ctx *ui,
			       enum cfs_worker_state st)
{
	if (!ui->use_color)
		return "";
	switch (st) {
	case CFS_STATE_RUNNING:
		return ANSI_BOLD ANSI_B_GREEN;
	case CFS_STATE_STOPPED:
		return ANSI_YELLOW;
	case CFS_STATE_DONE:
		return ANSI_CYAN;
	case CFS_STATE_ABORTED:
		return ANSI_B_RED;
	case CFS_STATE_INIT:
	default:
		return ANSI_DIM;
	}
}

static void render_progress_bar(const struct cfs_ui_ctx *ui,
				uint64_t done, uint64_t total,
				int nice_val)
{
	unsigned int filled = 0;
	unsigned int pct = 0;
	unsigned int i;

	if (total > 0) {
		if (done > total)
			done = total;
		filled = (unsigned int)((done * (uint64_t)BAR_WIDTH) / total);
		pct = (unsigned int)((done * 100ULL) / total);
	}

	fputs("[", stdout);
	fputs(priority_color(ui, nice_val), stdout);
	for (i = 0; i < BAR_WIDTH; i++) {
		if (i < filled) {
			fputs(ui->use_utf8 ? "\xE2\x96\x88" : "#", stdout);
		} else {
			if (i == filled)
				fputs(c(ui, ANSI_RESET ANSI_DIM), stdout);
			fputs(ui->use_utf8 ? "\xE2\x96\x91" : "-", stdout);
		}
	}
	fputs(c(ui, ANSI_RESET), stdout);
	printf("] %3u%%", pct);
}

void cfs_ui_init(struct cfs_ui_ctx *ui)
{
	struct utsname uts;
	const char *no_color = getenv("NO_COLOR");
	const char *lang = getenv("LANG");

	memset(ui, 0, sizeof(*ui));
	ui->is_tty = (isatty(STDOUT_FILENO) != 0);
	ui->use_color = ui->is_tty && (no_color == NULL || no_color[0] == '\0');
	ui->use_utf8 = ui->is_tty;

	if (lang != NULL && strstr(lang, "UTF-8") == NULL &&
	    strstr(lang, "utf8") == NULL && strstr(lang, "utf-8") == NULL) {
		/* Keep ASCII fallback only if LANG explicitly specifies non-UTF-8 C locale */
		if (strcmp(lang, "C") == 0 || strcmp(lang, "POSIX") == 0)
			ui->use_utf8 = false;
	}

	if (uname(&uts) == 0) {
		snprintf(ui->kernel_release, sizeof(ui->kernel_release),
			 "%s", uts.release);
	} else {
		snprintf(ui->kernel_release, sizeof(ui->kernel_release),
			 "unknown");
	}
}

void cfs_ui_begin_dashboard(struct cfs_ui_ctx *ui)
{
	if (!ui->is_tty)
		return;
	fputs(ANSI_HIDE_CURSOR ANSI_CLEAR_SCR, stdout);
	fflush(stdout);
	ui->dashboard_started = true;
}

void cfs_ui_end_dashboard(struct cfs_ui_ctx *ui)
{
	if (!ui->is_tty)
		return;
	fputs(ANSI_SHOW_CURSOR ANSI_RESET, stdout);
	fflush(stdout);
	ui->dashboard_started = false;
}

void cfs_ui_render_dashboard(const struct cfs_ui_ctx *ui,
			     struct cfs_shm_region *shm,
			     uint64_t elapsed_us,
			     int completed_count)
{
	const char *eol = ui->is_tty ? ANSI_CLEAR_EOL : "";
	double elapsed_sec = (double)elapsed_us / 1000000.0;
	int running_count = 0;
	int i;

	for (i = 0; i < shm->num_procs; i++) {
		if (shm->workers[i].state == CFS_STATE_RUNNING)
			running_count++;
	}

	if (ui->is_tty)
		fputs(ANSI_HOME, stdout);

	printf("%s========================================================================================%s%s\n",
	       c(ui, ANSI_BOLD ANSI_CYAN), c(ui, ANSI_RESET), eol);
	printf("%s  CFS-Inspired Virtual Runtime (vruntime) Time-Sharing Scheduler — Live Dashboard%s%s\n",
	       c(ui, ANSI_BOLD), c(ui, ANSI_RESET), eol);
	printf("%s========================================================================================%s%s\n",
	       c(ui, ANSI_BOLD ANSI_CYAN), c(ui, ANSI_RESET), eol);

	printf("  Kernel: %s%s%s  |  Syscall: %s#%d%s  |  Quantum: %s%d ms%s  |  Elapsed: %s%.2fs%s%s\n",
	       c(ui, ANSI_BOLD), ui->kernel_release, c(ui, ANSI_RESET),
	       c(ui, ANSI_B_CYAN), __NR_cfs_timeshare, c(ui, ANSI_RESET),
	       c(ui, ANSI_BOLD), shm->base_quantum_ms, c(ui, ANSI_RESET),
	       c(ui, ANSI_B_GREEN), elapsed_sec, c(ui, ANSI_RESET), eol);

	printf("  Workers: %d total (%s%d running%s, %d waiting, %s%d done%s)  |  Work Target: %llu units/proc%s\n",
	       shm->num_procs,
	       c(ui, ANSI_B_GREEN), running_count, c(ui, ANSI_RESET),
	       shm->num_procs - completed_count - running_count,
	       c(ui, ANSI_CYAN), completed_count, c(ui, ANSI_RESET),
	       (unsigned long long)shm->total_work_units, eol);

	printf("%s----------------------------------------------------------------------------------------%s%s\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET), eol);
	printf("%s  ID   PID     Priority  Nice  Weight  Work Progress                    Units     CPU(ms)  State%s%s\n",
	       c(ui, ANSI_BOLD), c(ui, ANSI_RESET), eol);
	printf("%s----------------------------------------------------------------------------------------%s%s\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET), eol);

	for (i = 0; i < shm->num_procs; i++) {
		struct cfs_worker_slot *w = &shm->workers[i];
		uint64_t done = atomic_load_explicit(&w->completed_work,
						     memory_order_acquire);
		uint64_t cpu_us = atomic_load_explicit(&w->cpu_time_us,
						       memory_order_acquire);
		double cpu_ms = (double)cpu_us / 1000.0;

		printf("  #%02d  %-6d  %s%-6s%s    %+3d   %5u   ",
		       w->id,
		       (int)w->pid,
		       priority_color(ui, w->nice_val),
		       cfs_priority_label(w->nice_val),
		       c(ui, ANSI_RESET),
		       w->nice_val,
		       w->weight);

		render_progress_bar(ui, done, w->total_work, w->nice_val);

		printf("  %3llu/%-3llu  %7.1f  %s%-7s%s%s\n",
		       (unsigned long long)done,
		       (unsigned long long)w->total_work,
		       cpu_ms,
		       state_color(ui, w->state),
		       cfs_state_label(w->state),
		       c(ui, ANSI_RESET),
		       eol);
	}

	printf("%s----------------------------------------------------------------------------------------%s%s\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET), eol);
	printf("  %sTip:%s Watch %ssudo dmesg -w | grep --line-buffered \"\\[CFS_SYS\\]\"%s for live kernel ticks. Press Ctrl+C to abort.%s\n",
	       c(ui, ANSI_BOLD), c(ui, ANSI_RESET),
	       c(ui, ANSI_CYAN), c(ui, ANSI_RESET), eol);

	if (ui->is_tty)
		fputs(ANSI_CLEAR_BELOW, stdout);

	fflush(stdout);
}

struct class_stats {
	const char *label;
	int nice_val;
	int count;
	uint64_t weight_sum;
	uint64_t snap_work_sum;
	uint64_t snap_cpu_us_sum;
	uint64_t total_cpu_us_sum;
	double turnaround_ms_sum;
};

static int nice_to_class_idx(int nice_val)
{
	if (nice_val <= CFS_NICE_HIGH)
		return 0;
	if (nice_val >= CFS_NICE_LOW)
		return 2;
	return 1;
}

void cfs_ui_render_summary(const struct cfs_ui_ctx *ui,
			   struct cfs_shm_region *shm,
			   bool was_aborted)
{
	struct class_stats classes[3] = {
		{ "HIGH",   CFS_NICE_HIGH,   0, 0, 0, 0, 0, 0.0 },
		{ "NORMAL", CFS_NICE_NORMAL, 0, 0, 0, 0, 0, 0.0 },
		{ "LOW",    CFS_NICE_LOW,    0, 0, 0, 0, 0, 0.0 }
	};
	uint64_t total_weight = 0;
	uint64_t total_snap_work = 0;
	uint64_t total_snap_cpu_us = 0;
	double session_sec = (double)(shm->session_end_us - shm->session_start_us) / 1000000.0;
	int i;

	printf("\n");
	printf("%s========================================================================================%s\n",
	       c(ui, ANSI_BOLD ANSI_CYAN), c(ui, ANSI_RESET));
	if (was_aborted) {
		printf("%s  FINAL SIMULATION SUMMARY (ABORTED BY USER)%s\n",
		       c(ui, ANSI_BOLD ANSI_B_RED), c(ui, ANSI_RESET));
	} else {
		printf("%s  FINAL CFS SCHEDULING REPORT — FAIRNESS VS. PRIORITY WEIGHTS%s\n",
		       c(ui, ANSI_BOLD ANSI_B_GREEN), c(ui, ANSI_RESET));
	}
	printf("%s========================================================================================%s\n",
	       c(ui, ANSI_BOLD ANSI_CYAN), c(ui, ANSI_RESET));
	printf("  Total Wall Time: %.2f s  |  Base Quantum: %d ms  |  Processes: %d  |  Work Units: %llu\n\n",
	       session_sec, shm->base_quantum_ms, shm->num_procs,
	       (unsigned long long)shm->total_work_units);

	/* Section 1: Per-Process Completion Table */
	printf("%s  1. Per-Process Execution Metrics%s\n",
	       c(ui, ANSI_BOLD), c(ui, ANSI_RESET));
	printf("%s  --------------------------------------------------------------------------------------%s\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET));
	printf("%s  ID   PID     Priority  Nice  Weight  Units    CPU(ms)  Turnaround(ms)  Eff.CPU%%  Rank%s\n",
	       c(ui, ANSI_BOLD), c(ui, ANSI_RESET));
	printf("%s  --------------------------------------------------------------------------------------%s\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET));

	for (i = 0; i < shm->num_procs; i++) {
		struct cfs_worker_slot *w = &shm->workers[i];
		uint64_t done = atomic_load_explicit(&w->completed_work,
						     memory_order_acquire);
		uint64_t cpu_us = atomic_load_explicit(&w->cpu_time_us,
						       memory_order_acquire);
		uint64_t end_us = (w->finish_mono_us >= w->start_mono_us)
				  ? w->finish_mono_us
				  : shm->session_end_us;
		double cpu_ms = (double)cpu_us / 1000.0;
		double turn_ms = (double)(end_us - w->start_mono_us) / 1000.0;
		double eff_cpu_pct = (turn_ms > 0.0) ? ((cpu_ms / turn_ms) * 100.0) : 0.0;
		int cidx = nice_to_class_idx(w->nice_val);

		classes[cidx].count++;
		classes[cidx].weight_sum += w->weight;
		classes[cidx].snap_work_sum += w->snap_work;
		classes[cidx].snap_cpu_us_sum += w->snap_cpu_us;
		classes[cidx].total_cpu_us_sum += cpu_us;
		classes[cidx].turnaround_ms_sum += turn_ms;

		total_weight += w->weight;
		total_snap_work += w->snap_work;
		total_snap_cpu_us += w->snap_cpu_us;

		printf("  #%02d  %-6d  %s%-6s%s    %+3d   %5u  %3llu/%-3llu  %7.1f  %14.1f  %7.1f%%  ",
		       w->id,
		       (int)w->pid,
		       priority_color(ui, w->nice_val),
		       cfs_priority_label(w->nice_val),
		       c(ui, ANSI_RESET),
		       w->nice_val,
		       w->weight,
		       (unsigned long long)done,
		       (unsigned long long)w->total_work,
		       cpu_ms,
		       turn_ms,
		       eff_cpu_pct);

		if (w->finish_rank > 0)
			printf("#%d\n", w->finish_rank);
		else
			printf("-\n");
	}
	printf("%s  --------------------------------------------------------------------------------------%s\n\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET));

	/* Section 2: Priority Class Fairness Comparison (Contention Window) */
	printf("%s  2. Priority-Weight Fairness Analysis (Full-Contention Window @ %.2f s)%s\n",
	       c(ui, ANSI_BOLD),
	       (double)shm->snapshot_elapsed_us / 1000000.0,
	       c(ui, ANSI_RESET));
	printf("     Measured at the moment the first worker finished (while all %d processes competed).\n",
	       shm->num_procs);
	printf("%s  --------------------------------------------------------------------------------------%s\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET));
	printf("%s  Class   Procs  Weight Sum  Expected Share  Observed Work%%  Observed CPU%%   Avg Turnaround%s\n",
	       c(ui, ANSI_BOLD), c(ui, ANSI_RESET));
	printf("%s  --------------------------------------------------------------------------------------%s\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET));

	for (i = 0; i < 3; i++) {
		double expected_pct;
		double obs_work_pct;
		double obs_cpu_pct;
		double avg_turn_ms;

		if (classes[i].count == 0)
			continue;

		expected_pct = (total_weight > 0)
			       ? ((double)classes[i].weight_sum * 100.0 / (double)total_weight)
			       : 0.0;
		obs_work_pct = (total_snap_work > 0)
			       ? ((double)classes[i].snap_work_sum * 100.0 / (double)total_snap_work)
			       : 0.0;
		obs_cpu_pct = (total_snap_cpu_us > 0)
			      ? ((double)classes[i].snap_cpu_us_sum * 100.0 / (double)total_snap_cpu_us)
			      : 0.0;
		avg_turn_ms = classes[i].turnaround_ms_sum / (double)classes[i].count;

		printf("  %s%-6s%s  %5d  %10llu  %13.1f%%  %13.1f%%  %12.1f%%  %12.1f ms\n",
		       priority_color(ui, classes[i].nice_val),
		       classes[i].label,
		       c(ui, ANSI_RESET),
		       classes[i].count,
		       (unsigned long long)classes[i].weight_sum,
		       expected_pct,
		       obs_work_pct,
		       obs_cpu_pct,
		       avg_turn_ms);
	}
	printf("%s  --------------------------------------------------------------------------------------%s\n",
	       c(ui, ANSI_DIM), c(ui, ANSI_RESET));

	if (!was_aborted && classes[0].count > 0 && classes[2].count > 0) {
		double high_avg = classes[0].turnaround_ms_sum / (double)classes[0].count;
		double low_avg = classes[2].turnaround_ms_sum / (double)classes[2].count;
		double speedup = (high_avg > 0.0) ? (low_avg / high_avg) : 0.0;

		printf("\n  %sKey Takeaway:%s HIGH-priority tasks (weight %u) completed %s%.2fx faster%s on average\n",
		       c(ui, ANSI_BOLD ANSI_B_GREEN), c(ui, ANSI_RESET),
		       CFS_WEIGHT_HIGH,
		       c(ui, ANSI_BOLD), speedup, c(ui, ANSI_RESET));
		printf("  than LOW-priority tasks (weight %u) while guaranteeing zero starvation via vruntime.\n",
		       CFS_WEIGHT_LOW);
	}

	printf("  Check %ssudo dmesg | grep \"\\[CFS_SYS\\]\" | tail -n 30%s to inspect the kernel summary table.\n\n",
	       c(ui, ANSI_CYAN), c(ui, ANSI_RESET));
	fflush(stdout);
}

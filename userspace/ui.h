/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ui.h - ANSI terminal dashboard and fairness report renderer declarations.
 */

#ifndef CFS_UI_H
#define CFS_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "worker.h"

struct cfs_ui_ctx {
	bool use_color;
	bool use_utf8;
	bool is_tty;
	bool dashboard_started;
	char kernel_release[128];
};

/* Initialize UI context (auto-detects TTY, color, and kernel uname release) */
void cfs_ui_init(struct cfs_ui_ctx *ui);

/* Enter/exit live dashboard mode (hides/restores terminal cursor cleanly) */
void cfs_ui_begin_dashboard(struct cfs_ui_ctx *ui);
void cfs_ui_end_dashboard(struct cfs_ui_ctx *ui);

/* Render one live frame of the real-time process progress dashboard */
void cfs_ui_render_dashboard(const struct cfs_ui_ctx *ui,
			     struct cfs_shm_region *shm,
			     uint64_t elapsed_us,
			     int completed_count);

/* Render the final execution summary and CFS weight-vs-fairness analysis */
void cfs_ui_render_summary(const struct cfs_ui_ctx *ui,
			   struct cfs_shm_region *shm,
			   bool was_aborted);

#endif /* CFS_UI_H */

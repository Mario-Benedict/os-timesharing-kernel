#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run_demo.sh - Clear dmesg and launch a 2-pane tmux split-screen session:
#                 Left  pane: ./build/cfs_sim (user-space interactive simulator)
#                 Right pane: live kernel [CFS_SYS] scheduler logs via dmesg -w

set -euo pipefail

log_info()  { printf '[INFO]  %s\n' "$*"; }
log_warn()  { printf '[WARN]  %s\n' "$*"; }
log_error() { printf '[ERROR] %s\n' "$*" >&2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SIM_BIN="${REPO_ROOT}/build/cfs_sim"
SESSION_NAME="cfs-demo"

if ! command -v tmux >/dev/null 2>&1; then
    log_error "tmux is not installed. Run ./scripts/setup_vm_deps.sh first."
    exit 1
fi

if [[ ! -x "${SIM_BIN}" ]]; then
    log_info "Simulator binary not found at ${SIM_BIN}. Building with make all..."
    make -C "${REPO_ROOT}" all
fi

KERNEL_REL="$(uname -r)"
if [[ "${KERNEL_REL}" != *"-cfs-timeshare"* ]]; then
    log_warn "Current kernel (${KERNEL_REL}) does not contain '-cfs-timeshare'."
    log_warn "Syscall #462 requires booting the patched Linux 6.6 LTS kernel."
fi

# Authenticate sudo upfront and clear the kernel ring buffer cleanly
log_info "Clearing kernel ring buffer (sudo dmesg -C)..."
sudo dmesg -C

# Preserve any optional CLI arguments forwarded to ./build/cfs_sim
SIM_ARGS=""
if [[ $# -gt 0 ]]; then
    printf -v SIM_ARGS ' %q' "$@"
fi

LEFT_CMD="cd $(printf '%q' "${REPO_ROOT}") && ./build/cfs_sim${SIM_ARGS}; status=\$?; printf '\n[DEMO] Simulator exited (code %d). Press Enter to close tmux session...' \"\$status\"; read -r _; tmux kill-session -t ${SESSION_NAME} 2>/dev/null || true"
RIGHT_CMD='printf "=== Live Kernel CFS Scheduler Log Stream ([CFS_SYS]) ===\n"; sudo dmesg -w | grep --line-buffered "\[CFS_SYS\]"'

# Replace any stale demo session cleanly
if tmux has-session -t "${SESSION_NAME}" 2>/dev/null; then
    log_info "Closing previous tmux session '${SESSION_NAME}'..."
    tmux kill-session -t "${SESSION_NAME}"
fi

log_info "Launching 2-pane tmux split-screen demo session '${SESSION_NAME}'..."
tmux new-session -d -s "${SESSION_NAME}" -n "cfs-scheduler" "${LEFT_CMD}"
tmux split-window -h -t "${SESSION_NAME}:0" "${RIGHT_CMD}"
tmux select-pane -t "${SESSION_NAME}:0.0"

# Enable mouse scrolling and generous scrollback history for this demo session
tmux set-option -t "${SESSION_NAME}" mouse on >/dev/null 2>&1 || true
tmux set-option -t "${SESSION_NAME}" history-limit 10000 >/dev/null 2>&1 || true

if [[ -n "${TMUX:-}" ]]; then
    exec tmux switch-client -t "${SESSION_NAME}"
else
    exec tmux attach-session -t "${SESSION_NAME}"
fi

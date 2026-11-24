#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# patch_kernel.sh - Idempotently integrate the custom CFS-inspired vruntime
#                   time-sharing system call (#462) into a Linux 6.6 LTS source tree.
#
# Usage:
#   ./scripts/patch_kernel.sh <kernel_source_dir>

set -euo pipefail

log_info()  { printf '[INFO]  %s\n' "$*"; }
log_ok()    { printf '[OK]    %s\n' "$*"; }
log_warn()  { printf '[WARN]  %s\n' "$*"; }
log_error() { printf '[ERROR] %s\n' "$*" >&2; }

usage() {
    printf 'Usage: %s <kernel_source_dir>\n' "$0" >&2
    printf 'Example: %s ~/linux-6.6.60\n' "$0" >&2
}

if [[ $# -ne 1 ]]; then
    log_error "Expected exactly 1 argument: <kernel_source_dir>."
    usage
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
KERNEL_DIR="$(cd "$1" 2>/dev/null && pwd || true)"

if [[ -z "${KERNEL_DIR}" || ! -d "${KERNEL_DIR}" ]]; then
    log_error "Target kernel directory does not exist: $1"
    exit 1
fi

ROOT_MAKEFILE="${KERNEL_DIR}/Makefile"
ROOT_KBUILD="${KERNEL_DIR}/Kbuild"
SYSCALLS_H="${KERNEL_DIR}/include/linux/syscalls.h"
SYSCALL_TBL="${KERNEL_DIR}/arch/x86/entry/syscalls/syscall_64.tbl"
SRC_MOD_DIR="${REPO_ROOT}/kernel/cfs_timeshare"
SRC_SHARED_H="${REPO_ROOT}/include/cfs_shared.h"

for required_file in "${ROOT_MAKEFILE}" "${SYSCALLS_H}" "${SYSCALL_TBL}"; do
    if [[ ! -f "${required_file}" ]]; then
        log_error "Not a valid Linux x86_64 kernel tree (missing: ${required_file})."
        exit 1
    fi
done

if [[ ! -d "${SRC_MOD_DIR}" || ! -f "${SRC_SHARED_H}" ]]; then
    log_error "Repository kernel sources missing under ${REPO_ROOT}."
    exit 1
fi

# Atomic replacement helper using a temporary file in the same directory
atomic_write_with_awk() {
    local target_file="$1"
    local awk_program="$2"
    local tmp_file

    tmp_file="$(mktemp "${target_file}.cfs_patch.XXXXXX")"
    if awk "${awk_program}" "${target_file}" > "${tmp_file}"; then
        mv "${tmp_file}" "${target_file}"
    else
        rm -f "${tmp_file}"
        return 1
    fi
}

log_info "Patching Linux kernel tree at: ${KERNEL_DIR}"

# ---------------------------------------------------------------------------
# Step 1: Copy kernel/cfs_timeshare/ and include/cfs_shared.h into kernel tree
# ---------------------------------------------------------------------------
mkdir -p "${KERNEL_DIR}/cfs_timeshare"
cp -f "${SRC_MOD_DIR}/cfs_timeshare.c" "${KERNEL_DIR}/cfs_timeshare/cfs_timeshare.c"
cp -f "${SRC_MOD_DIR}/Makefile"        "${KERNEL_DIR}/cfs_timeshare/Makefile"
cp -f "${SRC_SHARED_H}"                "${KERNEL_DIR}/include/linux/cfs_shared.h"
log_ok "[1/5] Installed cfs_timeshare/ sources and include/linux/cfs_shared.h."

# ---------------------------------------------------------------------------
# Step 2: Register cfs_timeshare/ in root Kbuild (Linux 6.6) or Makefile
# ---------------------------------------------------------------------------
if [[ -f "${ROOT_KBUILD}" ]] && grep -Eq '^[[:space:]]*obj-y[[:space:]]*\+=[[:space:]]*kernel/' "${ROOT_KBUILD}"; then
    if grep -Eq '^[[:space:]]*obj-y[[:space:]]*\+=[[:space:]]*cfs_timeshare/' "${ROOT_KBUILD}"; then
        log_ok "[2/5] Root Kbuild already includes cfs_timeshare/ (no change)."
    else
        atomic_write_with_awk "${ROOT_KBUILD}" '
            {
                print $0
                if (!done && $0 ~ /^[ \t]*obj-y[ \t]*\+=[ \t]*kernel\//) {
                    print "obj-y\t\t\t+= cfs_timeshare/"
                    done = 1
                }
            }
            END {
                if (!done) {
                    print "obj-y\t\t\t+= cfs_timeshare/"
                }
            }
        '
        log_ok "[2/5] Added obj-y += cfs_timeshare/ to root Kbuild."
    fi
else
    if grep -Eq 'cfs_timeshare/' "${ROOT_MAKEFILE}"; then
        log_ok "[2/5] Root Makefile already includes cfs_timeshare/ (no change)."
    else
        atomic_write_with_awk "${ROOT_MAKEFILE}" '
            {
                if (!done && $0 ~ /^[ \t]*core-y[ \t]*\+=/) {
                    print $0 " cfs_timeshare/"
                    done = 1
                } else {
                    print $0
                }
            }
            END {
                if (!done) {
                    print "core-y += cfs_timeshare/"
                }
            }
        '
        log_ok "[2/5] Added cfs_timeshare/ to root Makefile core-y."
    fi
fi

# ---------------------------------------------------------------------------
# Step 3: Insert sys_cfs_timeshare prototype into include/linux/syscalls.h
# ---------------------------------------------------------------------------
if grep -q 'sys_cfs_timeshare' "${SYSCALLS_H}"; then
    log_ok "[3/5] include/linux/syscalls.h already declares sys_cfs_timeshare (no change)."
else
    LAST_ENDIF_LINE="$(awk '/^[ \t]*#endif/ { last = NR } END { print last + 0 }' "${SYSCALLS_H}")"
    if [[ "${LAST_ENDIF_LINE}" -le 0 ]]; then
        log_error "Could not locate closing #endif in ${SYSCALLS_H}."
        exit 1
    fi

    tmp_syscalls="$(mktemp "${SYSCALLS_H}.cfs_patch.XXXXXX")"
    if awk -v target_line="${LAST_ENDIF_LINE}" '
        NR == target_line {
            print ""
            print "/* Custom CFS-inspired virtual runtime time-sharing scheduler syscall */"
            print "struct cfs_proc_arg;"
            print "asmlinkage long sys_cfs_timeshare(struct cfs_proc_arg __user *user_procs,"
            print "\t\t\t\t  int count, int base_quantum_ms);"
            print ""
        }
        { print $0 }
    ' "${SYSCALLS_H}" > "${tmp_syscalls}"; then
        mv "${tmp_syscalls}" "${SYSCALLS_H}"
    else
        rm -f "${tmp_syscalls}"
        exit 1
    fi
    log_ok "[3/5] Declared sys_cfs_timeshare() in include/linux/syscalls.h."
fi

# ---------------------------------------------------------------------------
# Step 4: Register syscall 462 in arch/x86/entry/syscalls/syscall_64.tbl
# ---------------------------------------------------------------------------
if grep -Eq '^[[:space:]]*462[[:space:]]+common[[:space:]]+cfs_timeshare[[:space:]]+sys_cfs_timeshare' "${SYSCALL_TBL}"; then
    log_ok "[4/5] syscall_64.tbl already registers syscall 462 (cfs_timeshare) (no change)."
elif grep -Eq '^[[:space:]]*462[[:space:]]' "${SYSCALL_TBL}"; then
    log_error "Syscall number 462 is already occupied by another syscall in ${SYSCALL_TBL}:"
    grep -E '^[[:space:]]*462[[:space:]]' "${SYSCALL_TBL}" >&2
    log_error "Ensure you are patching Linux 6.6 LTS (x86_64)."
    exit 1
else
    atomic_write_with_awk "${SYSCALL_TBL}" '
        BEGIN { inserted = 0 }
        !inserted && ($0 ~ /^# Due to a historical design error/ || $0 ~ /^512[ \t]/) {
            print "462\tcommon\tcfs_timeshare\t\tsys_cfs_timeshare"
            print ""
            inserted = 1
        }
        { print $0 }
        END {
            if (!inserted) {
                print "462\tcommon\tcfs_timeshare\t\tsys_cfs_timeshare"
            }
        }
    '
    log_ok "[4/5] Registered syscall 462 (cfs_timeshare) in arch/x86/entry/syscalls/syscall_64.tbl."
fi

# ---------------------------------------------------------------------------
# Step 5: Set EXTRAVERSION = -cfs-timeshare in root Makefile
# ---------------------------------------------------------------------------
if grep -Eq '^EXTRAVERSION[[:space:]]*=[[:space:]]*-cfs-timeshare[[:space:]]*$' "${ROOT_MAKEFILE}"; then
    log_ok "[5/5] Root Makefile already has EXTRAVERSION = -cfs-timeshare (no change)."
else
    atomic_write_with_awk "${ROOT_MAKEFILE}" '
        BEGIN { updated = 0 }
        !updated && /^EXTRAVERSION[ \t]*=/ {
            print "EXTRAVERSION = -cfs-timeshare"
            updated = 1
            next
        }
        { print $0 }
        END {
            if (!updated) {
                print "EXTRAVERSION = -cfs-timeshare"
            }
        }
    '
    log_ok "[5/5] Updated EXTRAVERSION = -cfs-timeshare in root Makefile."
fi

printf '\n'
log_ok "Kernel tree patched cleanly and idempotently!"
printf '\nNext steps inside %s:\n' "${KERNEL_DIR}"
printf '  1. cp -v /boot/config-$(uname -r) .config && make olddefconfig\n'
printf '  2. scripts/config --set-str SYSTEM_TRUSTED_KEYS ""\n'
printf '  3. scripts/config --set-str SYSTEM_REVOCATION_KEYS ""\n'
printf '  4. make -j"$(nproc)"\n'
printf '  5. sudo make modules_install && sudo make install && sudo reboot\n'

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# patch_kernel.sh - Idempotently integrate the custom CFS-inspired vruntime
#                   time-sharing system call (#500) into the Ubuntu Server
#                   26.04 LTS Linux 7.0 (`linux-source-7.0.0`) source tree.
#
# Usage:
#   ./scripts/patch_kernel.sh <kernel_source_dir>
# Example:
#   ./scripts/patch_kernel.sh ~/linux-source-7.0.0

set -euo pipefail

log_info()  { printf '[INFO]  %s\n' "$*"; }
log_ok()    { printf '[OK]    %s\n' "$*"; }
log_warn()  { printf '[WARN]  %s\n' "$*"; }
log_error() { printf '[ERROR] %s\n' "$*" >&2; }

usage() {
    printf 'Usage: %s <kernel_source_dir>\n' "$0" >&2
    printf 'Example: %s ~/linux-source-7.0.0\n' "$0" >&2
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

# Read the single-source-of-truth syscall number from include/cfs_shared.h
SYSCALL_NR="$(awk '$1 == "#define" && $2 == "__NR_cfs_timeshare" { print $3; exit }' "${SRC_SHARED_H}")"
if [[ -z "${SYSCALL_NR}" || ! "${SYSCALL_NR}" =~ ^[0-9]+$ ]]; then
    log_error "Could not parse numeric __NR_cfs_timeshare from ${SRC_SHARED_H}."
    exit 1
fi

# Verify kernel version tuple from root Makefile (Ubuntu Server 26.04 LTS uses Linux 7.0)
K_VER="$(awk -F= '/^VERSION[ \t]*=/ { gsub(/[ \t]/, "", $2); print $2; exit }' "${ROOT_MAKEFILE}")"
K_PATCH="$(awk -F= '/^PATCHLEVEL[ \t]*=/ { gsub(/[ \t]/, "", $2); print $2; exit }' "${ROOT_MAKEFILE}")"
K_SUB="$(awk -F= '/^SUBLEVEL[ \t]*=/ { gsub(/[ \t]/, "", $2); print $2; exit }' "${ROOT_MAKEFILE}")"
K_FULL="${K_VER:-?}.${K_PATCH:-?}.${K_SUB:-?}"

if [[ "${K_VER}" != "7" || "${K_PATCH}" != "0" ]]; then
    log_warn "Target kernel tree is Linux ${K_FULL}; Ubuntu Server 26.04 LTS ships with Linux 7.0 (linux-source-7.0.0)."
fi

# Atomic replacement helper using a temporary file in the same directory.
# Explicitly sets 0644 permissions (POSIX/uutils-safe; mktemp defaults to 0600).
atomic_write_with_awk() {
    local target_file="$1"
    local awk_program="$2"
    local tmp_file

    tmp_file="$(mktemp "${target_file}.cfs_patch.XXXXXX")"
    if awk "${awk_program}" "${target_file}" > "${tmp_file}"; then
        chmod 0644 "${tmp_file}"
        mv -f "${tmp_file}" "${target_file}"
    else
        rm -f "${tmp_file}"
        return 1
    fi
}

log_info "Patching Linux ${K_FULL} kernel tree at: ${KERNEL_DIR}"
log_info "Using custom x86_64 syscall number: #${SYSCALL_NR} (from include/cfs_shared.h)"

# ---------------------------------------------------------------------------
# Step 1: Copy kernel/cfs_timeshare/ and include/cfs_shared.h into kernel tree
# ---------------------------------------------------------------------------
mkdir -p "${KERNEL_DIR}/cfs_timeshare"
cp -f "${SRC_MOD_DIR}/cfs_timeshare.c" "${KERNEL_DIR}/cfs_timeshare/cfs_timeshare.c"
cp -f "${SRC_MOD_DIR}/Makefile"        "${KERNEL_DIR}/cfs_timeshare/Makefile"
cp -f "${SRC_SHARED_H}"                "${KERNEL_DIR}/include/linux/cfs_shared.h"
log_ok "[1/5] Installed cfs_timeshare/ sources and include/linux/cfs_shared.h."

# ---------------------------------------------------------------------------
# Step 2: Register cfs_timeshare/ in root Kbuild (Linux 7.0) or Makefile
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
        chmod 0644 "${tmp_syscalls}"
        mv -f "${tmp_syscalls}" "${SYSCALLS_H}"
    else
        rm -f "${tmp_syscalls}"
        exit 1
    fi
    log_ok "[3/5] Declared sys_cfs_timeshare() in include/linux/syscalls.h."
fi

# ---------------------------------------------------------------------------
# Step 4: Register syscall #500 in arch/x86/entry/syscalls/syscall_64.tbl
# ---------------------------------------------------------------------------
if grep -Eq "^[[:space:]]*${SYSCALL_NR}[[:space:]]+common[[:space:]]+cfs_timeshare[[:space:]]+sys_cfs_timeshare" "${SYSCALL_TBL}"; then
    log_ok "[4/5] syscall_64.tbl already registers syscall ${SYSCALL_NR} (cfs_timeshare) (no change)."
elif grep -Eq "^[[:space:]]*${SYSCALL_NR}[[:space:]]" "${SYSCALL_TBL}"; then
    log_error "Syscall number ${SYSCALL_NR} is already occupied by another syscall in ${SYSCALL_TBL}:"
    grep -E "^[[:space:]]*${SYSCALL_NR}[[:space:]]" "${SYSCALL_TBL}" >&2
    log_error "Ensure you are patching Ubuntu Server 26.04 LTS Linux 7.0 (x86_64)."
    exit 1
else
    tmp_tbl="$(mktemp "${SYSCALL_TBL}.cfs_patch.XXXXXX")"
    if awk -v nr="${SYSCALL_NR}" '
        BEGIN { inserted = 0 }
        !inserted && ($0 ~ /^# Due to a historical design error/ || $0 ~ /^# x32-specific/ || $0 ~ /^512[ \t]/) {
            printf "%s\tcommon\tcfs_timeshare\t\tsys_cfs_timeshare\n\n", nr
            inserted = 1
        }
        { print $0 }
        END {
            if (!inserted) {
                printf "%s\tcommon\tcfs_timeshare\t\tsys_cfs_timeshare\n", nr
            }
        }
    ' "${SYSCALL_TBL}" > "${tmp_tbl}"; then
        chmod 0644 "${tmp_tbl}"
        mv -f "${tmp_tbl}" "${SYSCALL_TBL}"
    else
        rm -f "${tmp_tbl}"
        exit 1
    fi
    log_ok "[4/5] Registered syscall ${SYSCALL_NR} (cfs_timeshare) in arch/x86/entry/syscalls/syscall_64.tbl."
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
log_ok "Ubuntu Server 26.04 LTS Linux 7.0 kernel tree patched cleanly and idempotently!"
printf '\nNext steps inside %s:\n' "${KERNEL_DIR}"
printf '  1. cp -v /boot/config-"$(uname -r)" .config\n'
printf '  2. scripts/config --set-str SYSTEM_TRUSTED_KEYS ""\n'
printf '  3. scripts/config --set-str SYSTEM_REVOCATION_KEYS ""\n'
printf '  4. scripts/config --disable WERROR\n'
printf '  5. scripts/config --disable RUST\n'
printf '  6. make olddefconfig && make -j"$(nproc)"\n'
printf '  7. sudo make modules_install && sudo make install\n'
printf '  8. Ensure Dracut initramfs is present for the custom kernel:\n'
printf '       KVER="$(make -s kernelrelease)"\n'
printf '       if [ ! -f "/boot/initrd.img-${KVER}" ]; then\n'
printf '         sudo dracut --force "/boot/initrd.img-${KVER}" "${KVER}"\n'
printf '       fi\n'
printf '  9. Update GRUB and pin the custom kernel as default:\n'
printf '       sudo update-grub\n'
printf '       sudo sed -i "s|^GRUB_DEFAULT=.*|GRUB_DEFAULT=\\"Advanced options for Ubuntu>Ubuntu, with Linux ${KVER}\\"|" /etc/default/grub\n'
printf '       sudo update-grub && sudo reboot\n'

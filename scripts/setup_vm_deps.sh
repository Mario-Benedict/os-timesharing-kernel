#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# setup_vm_deps.sh - Install Ubuntu Server 26.04 LTS ("Resolute Raccoon")
#                    — including Ubuntu Server (minimized) — build dependencies,
#                    native Linux 7.0 kernel source (`linux-source` ->
#                    linux-source-7.0.0), GCC 15, Perl (for localmodconfig),
#                    pahole, and tmux required for end-to-end compilation and
#                    demo runs.

set -euo pipefail

log_info()  { printf '[INFO]  %s\n' "$*"; }
log_ok()    { printf '[OK]    %s\n' "$*"; }
log_warn()  { printf '[WARN]  %s\n' "$*"; }
log_error() { printf '[ERROR] %s\n' "$*" >&2; }

if ! command -v apt-get >/dev/null 2>&1; then
    log_error "apt-get not found. This script targets Ubuntu Server 26.04 LTS."
    exit 1
fi

SUDO_CMD=()
if [[ "${EUID}" -ne 0 ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
        log_error "Please run as root or install sudo."
        exit 1
    fi
    SUDO_CMD=(sudo)
fi

if [[ -r /etc/os-release ]]; then
    OS_PRETTY="$(awk -F= '$1 == "PRETTY_NAME" { gsub(/"/, "", $2); print $2 }' /etc/os-release)"
    OS_VER_ID="$(awk -F= '$1 == "VERSION_ID" { gsub(/"/, "", $2); print $2 }' /etc/os-release)"
    log_info "Detected OS: ${OS_PRETTY:-Ubuntu}"
    if [[ "${OS_VER_ID}" != "26.04" ]]; then
        log_warn "This project targets Ubuntu Server 26.04 LTS (detected VERSION_ID=${OS_VER_ID:-unknown})."
    fi
fi

# Note:
#   - Uses `env DEBIAN_FRONTEND=noninteractive` (rather than `sudo -E`) so
#     the script works cleanly with Ubuntu 26.04 LTS's default `sudo-rs`.
#   - Explicitly includes packages omitted by Ubuntu Server (minimized) such as
#     `perl` (required by `make localmodconfig` / streamline_config.pl),
#     `ca-certificates`, `file`, `patch`, `procps`, `bzip2`, and `nano`.
PACKAGES=(
    build-essential
    gcc
    make
    pkg-config
    libncurses-dev
    bison
    flex
    libssl-dev
    libelf-dev
    libdw-dev
    openssl
    ca-certificates
    perl
    bc
    gawk
    python3
    dwarves
    zstd
    bzip2
    xz-utils
    tar
    file
    patch
    procps
    nano
    fakeroot
    rsync
    kmod
    cpio
    git
    curl
    wget
    tmux
    linux-source
)

log_info "Updating APT package index..."
"${SUDO_CMD[@]}" apt-get update -y

log_info "Installing Ubuntu Server 26.04 LTS kernel build toolchain, Linux 7.0 source, GCC 15, and tmux..."
"${SUDO_CMD[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y "${PACKAGES[@]}"

log_ok "All Ubuntu Server 26.04 LTS dependencies installed successfully."
log_info "Detected environment & toolchain versions:"
printf '  - kernel:       %s\n' "$(uname -r)"
printf '  - gcc:          %s\n' "$(gcc --version | head -n 1)"
printf '  - make:         %s\n' "$(make --version | head -n 1)"
if command -v pahole >/dev/null 2>&1; then
    printf '  - pahole:       %s\n' "$(pahole --version 2>/dev/null | head -n 1)"
fi
printf '  - tmux:         %s\n' "$(tmux -V)"
if command -v dracut >/dev/null 2>&1; then
    printf '  - initramfs:    dracut (%s)\n' "$(dracut --version 2>/dev/null | head -n 1)"
elif command -v update-initramfs >/dev/null 2>&1; then
    printf '  - initramfs:    initramfs-tools\n'
fi
if [[ -f /usr/src/linux-source-7.0.0.tar.bz2 ]]; then
    printf '  - linux-source: /usr/src/linux-source-7.0.0.tar.bz2\n'
else
    SRC_TAR="$(ls /usr/src/linux-source-*.tar.bz2 2>/dev/null | head -n 1 || true)"
    if [[ -n "${SRC_TAR}" ]]; then
        printf '  - linux-source: %s\n' "${SRC_TAR}"
    fi
fi

printf '\nNext step — extract Ubuntu 26.04 Linux 7.0 source into your home directory:\n'
printf '  cd ~ && tar -xjf /usr/src/linux-source-7.0.0.tar.bz2\n'
printf '  ./scripts/patch_kernel.sh ~/linux-source-7.0.0\n'

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# setup_vm_deps.sh - Install Ubuntu/Debian build dependencies required to
#                    compile Linux 6.6 LTS, build the C11 user-space simulator,
#                    and run the split-screen tmux demo.

set -euo pipefail

log_info()  { printf '[INFO]  %s\n' "$*"; }
log_ok()    { printf '[OK]    %s\n' "$*"; }
log_error() { printf '[ERROR] %s\n' "$*" >&2; }

if ! command -v apt-get >/dev/null 2>&1; then
    log_error "apt-get not found. This script targets Ubuntu/Debian systems."
    exit 1
fi

SUDO=""
if [[ "${EUID}" -ne 0 ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
        log_error "Please run as root or install sudo."
        exit 1
    fi
    SUDO="sudo"
fi

PACKAGES=(
    build-essential
    gcc
    make
    libncurses-dev
    bison
    flex
    libssl-dev
    libelf-dev
    bc
    dwarves
    zstd
    fakeroot
    rsync
    kmod
    cpio
    git
    curl
    wget
    tar
    xz-utils
    tmux
)

log_info "Updating APT package index..."
${SUDO} apt-get update -y

log_info "Installing Linux 6.6 LTS kernel build tools, GCC, and tmux..."
${SUDO} env DEBIAN_FRONTEND=noninteractive apt-get install -y "${PACKAGES[@]}"

log_ok "All VM dependencies installed successfully."
log_info "Detected toolchain versions:"
printf '  - gcc:  %s\n' "$(gcc --version | head -n 1)"
printf '  - make: %s\n' "$(make --version | head -n 1)"
printf '  - tmux: %s\n' "$(tmux -V)"

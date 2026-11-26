# CFS-Inspired Virtual Runtime (`vruntime`) Time-Sharing Scheduler

A complete Operating Systems project implementing a **Completely Fair Scheduler (CFS)-inspired virtual runtime (`vruntime`) time-sharing scheduler** as a **custom Linux 7.0 (`x86_64`) system call** (`#500`, `sys_cfs_timeshare`) on **Ubuntu Server 26.04 LTS ("Resolute Raccoon")**, paired with a modular **C11 user-space simulator** and a live ANSI terminal dashboard.

---

## Concept Overview: Why CFS `vruntime` Beats Plain Round-Robin

Traditional **Round-Robin (RR)** time-sharing gives every runnable process an equal fixed quantum regardless of priority. While simple, plain Round-Robin has two classic drawbacks:
1. **No Priority Differentiation:** Critical interactive or high-priority tasks receive the exact same CPU bandwidth as background batch jobs.
2. **Rigid Priority Queues Risk Starvation:** Naive strict-priority extensions to Round-Robin can starve low-priority tasks indefinitely whenever high-priority tasks remain runnable.

This project solves both problems using the core mathematical invariant of Linux's **Completely Fair Scheduler (CFS)**:

1. **Geometric Priority Weights (`sched_prio_to_weight`):**
   Every process nice value $n \in [-20, +19]$ maps to a CFS weight:
   - **HIGH** (`nice = -5`) $\rightarrow$ weight **`3121`**
   - **NORMAL** (`nice = 0`) $\rightarrow$ weight **`1024`** (`NICE_0_LOAD`)
   - **LOW** (`nice = +5`) $\rightarrow$ weight **`335`**

2. **Weighted Virtual Runtime (`vruntime`) Formula:**
   Each time a process is preempted after executing for `actual_slice_ms` milliseconds, the kernel updates its virtual runtime:

   $$\Delta \text{vruntime} = \frac{\text{actual\_slice\_ms} \times 1024}{\text{weight}}$$

   - High-priority tasks (`3121`) accumulate `vruntime` **~3.05x slower** than Normal tasks and **~9.32x slower** than Low tasks.
   - Low-priority tasks (`335`) accumulate `vruntime` **~3.06x faster** than Normal tasks.

3. **Minimum-`vruntime` Dispatch + Dynamically Weighted Time Slices:**
   - On every kernel timer tick (`struct timer_list`), the scheduler preempts the active task (`SIGSTOP`), prunes any exited/zombie tasks, selects the live task with the **smallest `vruntime`**, resumes it (`SIGCONT`), and re-arms the timer with a dynamically weighted slice:
     $$\text{slice\_ms} = \text{clamp}\!\left(\frac{\text{base\_quantum\_ms} \times N_{\text{active}} \times \text{weight}}{\sum \text{weight}_{\text{active}}},\ \text{min\_slice},\ 1000\right)$$
   - **Result:** High-priority processes receive proportionally more CPU time and finish dramatically earlier, yet every low-priority process makes steady progress with **zero starvation**.

For the full ASCII architecture diagram, locking guarantees, and worked numerical trace, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Standardized Platform Stack (Ubuntu Server 26.04 LTS)

Every script, kernel source file, user-space binary, and build instruction in this repository is aligned to a single consistent platform:

| Layer | Version / Standard |
| :--- | :--- |
| **Operating System** | **Ubuntu Server 26.04 LTS** (*Resolute Raccoon*, `x86_64`) |
| **Kernel Source & Target** | **Linux 7.0** via official Ubuntu package `linux-source` (`linux-source-7.0.0` $\rightarrow$ **`7.0.0-cfs-timeshare`**) |
| **Custom System Call** | **`#500`** (`__NR_cfs_timeshare` / `sys_cfs_timeshare`, safely above Linux 7.0's `#472 fchroot` and below `#512` x32) |
| **Kernel Timer API** | `timer_setup()`, `mod_timer()`, **`timer_delete_sync()`** (Linux 7.0 native API) |
| **Compiler & C Library** | **GCC 15** (`-std=c11 -Wall -Wextra -O2`), **glibc 2.43**, **binutils 2.46** |
| **System Tooling** | **Dracut** (default initramfs generator), **`sudo-rs`**, **`uutils` coreutils**, **GRUB2**, **tmux** |

---

## Repository Structure

```text
.
├── Makefile                            # Root unified Makefile (all, check, demo, clean)
├── include/
│   └── cfs_shared.h                    # Shared kernel/user-space ABI (#500, struct cfs_proc_arg)
├── kernel/
│   └── cfs_timeshare/
│       ├── cfs_timeshare.c             # Custom Linux 7.0 syscall + timer_delete_sync/signal engine
│       └── Makefile                    # Kbuild rule (obj-y += cfs_timeshare.o)
├── userspace/
│   ├── main.c                          # CLI/interactive orchestrator, mmap shm, waitpid monitor
│   ├── worker.c                        # Signal-safe calibrated CPU-bound worker process
│   ├── worker.h                        # Shared-memory atomic structs & worker API
│   ├── ui.c                            # Flicker-free ANSI terminal dashboard & summary report
│   └── ui.h                            # Renderer interface
├── scripts/
│   ├── setup_vm_deps.sh                # Ubuntu Server 26.04 LTS build deps + linux-source-7.0.0 + tmux
│   ├── patch_kernel.sh                 # Idempotent Linux 7.0 (linux-source-7.0.0) source patcher
│   └── run_demo.sh                     # Automated 2-pane tmux split-screen demo launcher (sudo-rs aware)
├── docs/
│   └── ARCHITECTURE.md                 # Architecture diagram, vruntime math & locking model
├── .gitattributes                      # Enforces LF line endings across all scripts & code
└── .gitignore                          # Ignores build/, kernel objects, and editor artifacts
```

---

## Prerequisites & VM Setup (Ubuntu Server 26.04 LTS x86_64)

Recommended virtual machine specification for compiling Ubuntu 26.04's Linux 7.0 kernel smoothly:
- **OS:** **Ubuntu Server 26.04 LTS** (`x86_64`, *Resolute Raccoon*)
- **CPU:** 4+ vCPUs (more cores significantly reduce kernel build time)
- **RAM:** 6–8 GB (4 GB minimum; 8 GB recommended if keeping `CONFIG_DEBUG_INFO_BTF=y`)
- **Disk:** 40+ GB free space (Linux 7.0 build tree + modules + debug info take ~25–30 GB)

Install all required build packages **and** Ubuntu 26.04's matching `linux-source` (`linux-source-7.0.0`) package in one step:

```bash
chmod +x scripts/*.sh
./scripts/setup_vm_deps.sh
```

---

## Building & Installing the Patched Linux 7.0 Kernel

### 1. Extract Ubuntu Server 26.04 LTS's Native Linux 7.0 Source

`setup_vm_deps.sh` installs `/usr/src/linux-source-7.0.0.tar.bz2` directly from Ubuntu 26.04's official APT repository (guaranteeing a 1:1 match with your VM's running kernel configuration, GCC 15, AppArmor, and Dracut):

```bash
cd ~
tar -xjf /usr/src/linux-source-7.0.0.tar.bz2
```

This creates `~/linux-source-7.0.0/`.

### 2. Patch the Kernel Source Tree Idempotently

From the root of this repository, run `patch_kernel.sh` pointing to `~/linux-source-7.0.0`:

```bash
cd /path/to/os-timesharing-kernel
./scripts/patch_kernel.sh ~/linux-source-7.0.0
```

This script safely performs 5 idempotent steps (running it again creates zero duplicates):
1. Copies `kernel/cfs_timeshare/` into `~/linux-source-7.0.0/cfs_timeshare/` and `include/cfs_shared.h` into `~/linux-source-7.0.0/include/linux/cfs_shared.h`.
2. Adds `obj-y += cfs_timeshare/` to root `~/linux-source-7.0.0/Kbuild`.
3. Declares `asmlinkage long sys_cfs_timeshare(...)` in `~/linux-source-7.0.0/include/linux/syscalls.h`.
4. Registers syscall **`500`** (`cfs_timeshare`, parsed from `include/cfs_shared.h`) in `~/linux-source-7.0.0/arch/x86/entry/syscalls/syscall_64.tbl`.
5. Sets `EXTRAVERSION = -cfs-timeshare` in `~/linux-source-7.0.0/Makefile`.

### 3. Configure, Compile, Install, and Pin in GRUB (Dracut-Ready)

```bash
cd ~/linux-source-7.0.0

# 1. Seed .config from your running Ubuntu Server 26.04 LTS Linux 7.0 kernel
cp -v /boot/config-"$(uname -r)" .config

# 2. Clear Canonical signing key paths and disable WERROR & RUST for clean local GCC 15 builds
scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
scripts/config --set-str SYSTEM_REVOCATION_KEYS ""
scripts/config --disable WERROR
scripts/config --disable RUST

# Optional: disable BTF debug generation to save ~15 GB disk space and speed up linking
# scripts/config --disable DEBUG_INFO_BTF

# 3. Reconcile config and compile using all available vCPUs
make olddefconfig
make -j"$(nproc)"

# 4. Install kernel modules and kernel image into /boot
sudo make modules_install
sudo make install

# 5. Verify initramfs image exists (fresh Ubuntu Server 26.04 LTS uses Dracut by default)
KVER="$(make -s kernelrelease)"
echo "Installed kernel release: ${KVER}"
if [ ! -f "/boot/initrd.img-${KVER}" ]; then
    sudo dracut --force "/boot/initrd.img-${KVER}" "${KVER}"
fi

# 6. Update GRUB, verify the custom entry, pin it as default, and reboot
sudo update-grub
grep -E "menuentry 'Ubuntu, with Linux ${KVER}'" /boot/grub/grub.cfg
sudo sed -i "s|^GRUB_DEFAULT=.*|GRUB_DEFAULT=\"Advanced options for Ubuntu>Ubuntu, with Linux ${KVER}\"|" /etc/default/grub
sudo update-grub
sudo reboot
```

### 4. Verify the Custom Kernel After Reboot

```bash
uname -r
# Expected output: 7.0.0-cfs-timeshare
```

---

## Building & Checking the User-Space Simulator

Return to the repository root and verify zero-warning compilation with Ubuntu 26.04's **GCC 15** (`-std=c11 -Wall -Wextra -O2`):

```bash
make check   # Strict C11 (-Werror -Wpedantic -Wshadow -Wformat=2) + bash -n checks
make all     # Builds build/cfs_sim
```

Clean build artifacts anytime with:

```bash
make clean
```

---

## Running the Simulator & Split-Screen `tmux` Demo

### Option A: Automated 2-Pane `tmux` Split-Screen Demo (Recommended)

Clears `dmesg`, launches `./build/cfs_sim` in the **left pane**, and streams live `[CFS_SYS]` kernel scheduling decisions (`dmesg -w | grep --line-buffered "\[CFS_SYS\]"`) in the **right pane**:

```bash
# Recommended on Ubuntu Server 26.04 LTS (`sudo-rs` enforces per-TTY auth, so running
# with sudo avoids a second password prompt inside the right tmux log pane):
sudo ./scripts/run_demo.sh

# Or run as a normal user (press Ctrl-b -> in tmux if sudo-rs prompts in the right pane):
./scripts/run_demo.sh
```

You can also forward CLI flags directly to `./build/cfs_sim`:

```bash
sudo ./scripts/run_demo.sh -n 6 -q 100 -w 80
```

### Option B: Standalone CLI / Interactive Execution

```bash
# Interactive setup prompt (press Enter to accept defaults)
./build/cfs_sim

# Or run directly via flags
./build/cfs_sim -n 6 -q 100 -w 80 -p balanced

# Via Makefile shortcut
make demo ARGS="-n 6 -q 100 -w 80"
```

#### Command-Line Flags

| Flag | Description | Range / Values | Default |
| :--- | :--- | :--- | :--- |
| `-n <num_procs>` | Number of worker processes to spawn | `1..16` | `6` |
| `-q <base_quantum_ms>` | Base scheduling quantum in milliseconds | `10..2000` | `100` |
| `-w <work_units>` | Calibrated CPU work units per worker (~25 ms CPU/unit) | `5..1000` | `80` |
| `-p <profile>` | Priority distribution across workers | `balanced`, `skewed`, `equal` | `balanced` |
| `-i` | Force interactive configuration menu | — | Auto when no flags |
| `-h` | Display usage help | — | — |

---

## Sample Output

### User-Space Dashboard (Left Pane)

```text
========================================================================================
  CFS-Inspired Virtual Runtime (vruntime) Time-Sharing Scheduler — Live Dashboard
========================================================================================
  Kernel: 7.0.0-cfs-timeshare  |  Syscall: #500  |  Quantum: 100 ms  |  Elapsed: 4.85s
  Workers: 6 total (1 running, 4 waiting, 1 done)  |  Work Target: 80 units/proc
----------------------------------------------------------------------------------------
  ID   PID     Priority  Nice  Weight  Work Progress                    Units     CPU(ms)  State
----------------------------------------------------------------------------------------
  #00  2145    HIGH       -5    3121   [████████████████████████] 100%   80/80    1994.2  DONE
  #01  2146    NORMAL     +0    1024   [████████░░░░░░░░░░░░░░░░]  33%   26/80     651.4  WAITING
  #02  2147    LOW        +5     335   [██░░░░░░░░░░░░░░░░░░░░░░]  10%    8/80     208.1  WAITING
  #03  2148    HIGH       -5    3121   [███████████████████████░]  98%   78/80    1952.0  RUNNING
  #04  2149    NORMAL     +0    1024   [████████░░░░░░░░░░░░░░░░]  32%   26/80     648.7  WAITING
  #05  2150    LOW        +5     335   [██░░░░░░░░░░░░░░░░░░░░░░]  10%    8/80     205.3  WAITING
----------------------------------------------------------------------------------------
```

### Live Kernel `dmesg` Stream (Right Pane)

```text
[  142.310245] [CFS_SYS] Session started: registered=6, alive=6, base_quantum=100 ms
[  142.310261] [CFS_SYS] Initial dispatch -> PID 2145   (nice=-5, weight=3121, slice=209 ms)
[  142.520018] [CFS_SYS] Tick 0001 | Preempt PID 2145   (slice=209 ms, +dvrt=  68, vrt=     68) -> Next PID 2148   (weight=3121, vrt=      0, next_slice=209 ms)
[  142.732011] [CFS_SYS] Tick 0002 | Preempt PID 2148   (slice=212 ms, +dvrt=  69, vrt=     69) -> Next PID 2146   (weight=1024, vrt=      0, next_slice= 68 ms)
[  142.800023] [CFS_SYS] Tick 0003 | Preempt PID 2146   (slice= 68 ms, +dvrt=  68, vrt=     68) -> Next PID 2149   (weight=1024, vrt=      0, next_slice= 68 ms)
```

---

## Troubleshooting (Ubuntu Server 26.04 LTS)

- **`syscall(__NR_cfs_timeshare=500) failed: Function not implemented (errno=38 / ENOSYS)`**
  - You are still booted into Ubuntu 26.04's stock `7.0.0-*-generic` kernel. Run `uname -r` and verify `7.0.0-cfs-timeshare` appears. Re-run Step 6 (`GRUB_DEFAULT` + `sudo update-grub`) or select **Advanced options for Ubuntu → Ubuntu, with Linux 7.0.0-cfs-timeshare** at the GRUB prompt.
- **Boot stops with `unable to mount root fs` or missing `initrd.img-7.0.0-cfs-timeshare`**
  - Ubuntu Server 26.04 LTS uses **Dracut** for initramfs generation. Boot the stock `7.0.0-*-generic` kernel and generate the missing initrd:
    ```bash
    sudo dracut --force /boot/initrd.img-7.0.0-cfs-timeshare 7.0.0-cfs-timeshare
    sudo update-grub
    ```
- **Kernel build fails with `Rust compiler ... not found` or `CONFIG_RUST` errors**
  - Ubuntu 26.04's stock `.config` enables `CONFIG_RUST=y`. Disable it before building:
    ```bash
    scripts/config --disable RUST
    make olddefconfig
    ```
- **Kernel build fails on `debian/canonical-certs.pem` or `SYSTEM_TRUSTED_KEYS`**
  - Clear both Canonical signing key strings before `make -j"$(nproc)"`:
    ```bash
    scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
    scripts/config --set-str SYSTEM_REVOCATION_KEYS ""
    ```
- **Kernel build fails with `BTF: .tmp_vmlinux.btf: pahole not found` or runs low on RAM/disk**
  - Ensure `dwarves` and `libdw-dev` are installed via `./scripts/setup_vm_deps.sh`, or disable BTF cleanly with:
    ```bash
    scripts/config --disable DEBUG_INFO_BTF
    make olddefconfig
    ```
- **Right `tmux` pane waits for a password during `./scripts/run_demo.sh`**
  - Ubuntu 26.04's `sudo-rs` strictly enforces per-TTY credential caching, so the right pane PTY cannot share your outer shell's sudo ticket. Press `Ctrl-b →` in `tmux` to enter your password, or launch with `sudo ./scripts/run_demo.sh`.
- **Bash script fails with `/usr/bin/env: 'bash\r': No such file or directory`**
  - Normalize Windows CRLF endings with `sed -i 's/\r$//' scripts/*.sh`.
- **Inspecting full kernel summary after the demo finishes:**
  ```bash
  sudo dmesg | grep "\[CFS_SYS\]" | tail -n 35
  ```

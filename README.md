# CFS-Inspired Virtual Runtime (`vruntime`) Time-Sharing Scheduler

A complete Operating Systems project implementing a **Completely Fair Scheduler (CFS)-inspired virtual runtime (`vruntime`) time-sharing scheduler** as a **custom Linux 6.6 LTS (`x86_64`) system call** (`#462`, `sys_cfs_timeshare`), paired with a modular **C11 user-space simulator** and a live ANSI terminal dashboard.

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

## Repository Structure

```text
.
├── Makefile                            # Root unified Makefile (all, check, demo, clean)
├── include/
│   └── cfs_shared.h                    # Shared kernel/user-space ABI (#462, struct cfs_proc_arg)
├── kernel/
│   └── cfs_timeshare/
│       ├── cfs_timeshare.c             # Custom Linux 6.6 LTS syscall + timer/signal engine
│       └── Makefile                    # Kbuild rule (obj-y += cfs_timeshare.o)
├── userspace/
│   ├── main.c                          # CLI/interactive orchestrator, mmap shm, waitpid monitor
│   ├── worker.c                        # Signal-safe calibrated CPU-bound worker process
│   ├── worker.h                        # Shared-memory atomic structs & worker API
│   ├── ui.c                            # Flicker-free ANSI terminal dashboard & summary report
│   └── ui.h                            # Renderer interface
├── scripts/
│   ├── setup_vm_deps.sh                # Ubuntu/Debian kernel build tools + GCC + tmux installer
│   ├── patch_kernel.sh                 # Idempotent Linux 6.6 LTS kernel source tree patcher
│   └── run_demo.sh                     # Automated 2-pane tmux split-screen demo launcher
├── docs/
│   └── ARCHITECTURE.md                 # Architecture diagram, vruntime math & locking model
├── .gitattributes                      # Enforces LF line endings across all scripts & code
└── .gitignore                          # Ignores build/, kernel objects, and editor artifacts
```

---

## Prerequisites & VM Setup (Ubuntu Server 22.04 / 24.04 x86_64)

Recommended virtual machine specification for compiling Linux 6.6 LTS smoothly:
- **OS:** Ubuntu Server 22.04 LTS or 24.04 LTS (`x86_64`)
- **CPU:** 4+ vCPUs (more cores significantly reduce kernel build time)
- **RAM:** 4–8 GB
- **Disk:** 35+ GB free space (Linux 6.6 build tree + modules take ~20–25 GB)

Install all required packages (kernel build toolchain, GCC, `dwarves`, `libelf-dev`, `tmux`, etc.):

```bash
chmod +x scripts/*.sh
./scripts/setup_vm_deps.sh
```

---

## Building & Installing the Patched Linux 6.6 LTS Kernel

### 1. Download & Extract Linux 6.6 LTS

```bash
cd ~
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.63.tar.xz
tar -xf linux-6.6.63.tar.xz
```

*(Any Linux 6.6.x LTS release is supported.)*

### 2. Patch the Kernel Source Tree Idempotently

From the root of this repository, run `patch_kernel.sh` pointing to your extracted Linux 6.6 directory:

```bash
cd /path/to/os-timesharing-kernel
./scripts/patch_kernel.sh ~/linux-6.6.63
```

This script safely performs 5 idempotent steps (running it again creates zero duplicates):
1. Copies `kernel/cfs_timeshare/` into `<kernel>/cfs_timeshare/` and `include/cfs_shared.h` into `<kernel>/include/linux/cfs_shared.h`.
2. Adds `obj-y += cfs_timeshare/` to root `<kernel>/Kbuild`.
3. Declares `asmlinkage long sys_cfs_timeshare(...)` in `<kernel>/include/linux/syscalls.h`.
4. Registers syscall **`462`** (`cfs_timeshare`) in `<kernel>/arch/x86/entry/syscalls/syscall_64.tbl`.
5. Sets `EXTRAVERSION = -cfs-timeshare` in `<kernel>/Makefile`.

### 3. Configure, Compile, and Install

```bash
cd ~/linux-6.6.63

# Seed config from your current Ubuntu kernel and accept defaults for new symbols
cp -v /boot/config-"$(uname -r)" .config
make olddefconfig

# Clear Ubuntu Canonical signing key paths so local compilation succeeds
scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
scripts/config --set-str SYSTEM_REVOCATION_KEYS ""

# Compile kernel and modules using all available vCPUs
make -j"$(nproc)"

# Install modules and kernel image, then update GRUB
sudo make modules_install
sudo make install
sudo update-grub
sudo reboot
```

### 4. Verify the Custom Kernel After Reboot

```bash
uname -r
# Expected output: 6.6.63-cfs-timeshare
```

---

## Building & Checking the User-Space Simulator

Return to the repository root and verify zero-warning compilation with GCC (C11, `-Wall -Wextra -O2`):

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

Clears `dmesg`, launches `./build/cfs_sim` in the **left pane**, and streams live `[CFS_SYS]` kernel scheduling decisions (`sudo dmesg -w | grep --line-buffered "\[CFS_SYS\]"`) in the **right pane**:

```bash
./scripts/run_demo.sh
```

You can also forward CLI flags directly to `./build/cfs_sim`:

```bash
./scripts/run_demo.sh -n 6 -q 100 -w 80
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
  Kernel: 6.6.63-cfs-timeshare  |  Syscall: #462  |  Quantum: 100 ms  |  Elapsed: 4.85s
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

## Troubleshooting

- **`syscall(__NR_cfs_timeshare=462) failed: Function not implemented (errno=38 / ENOSYS)`**
  - You are still booted into the stock Ubuntu kernel. Run `uname -r` and verify `-cfs-timeshare` appears. If GRUB booted the old kernel by default, select **Advanced options for Ubuntu → Linux 6.6.x-cfs-timeshare** at boot.
- **Kernel build fails on `debian/canonical-certs.pem` or `SYSTEM_TRUSTED_KEYS`**
  - Run both commands in the kernel source directory before `make -j"$(nproc)"`:
    ```bash
    scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
    scripts/config --set-str SYSTEM_REVOCATION_KEYS ""
    ```
- **Kernel build fails with `BTF: .tmp_vmlinux.btf: pahole not found`**
  - Install `dwarves` via `./scripts/setup_vm_deps.sh` (or disable BTF with `scripts/config --disable DEBUG_INFO_BTF`).
- **Bash script fails with `/usr/bin/env: 'bash\r': No such file or directory`**
  - This happens if files were copied manually from Windows with CRLF endings. Normalize with `sed -i 's/\r$//' scripts/*.sh`.
- **`dmesg: read kernel buffer failed: Operation not permitted`**
  - Ubuntu restricts `dmesg` to root (`kernel.dmesg_restrict=1`). Always use `sudo dmesg -w` or run `./scripts/run_demo.sh` which prompts for `sudo` automatically.
- **Inspecting full kernel summary after the demo finishes:**
  ```bash
  sudo dmesg | grep "\[CFS_SYS\]" | tail -n 35
  ```

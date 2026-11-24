# SPDX-License-Identifier: GPL-2.0
#
# Root Unified Makefile for the CFS-Inspired Virtual Runtime Time-Sharing
# Scheduler user-space C11 simulator and verification suite.
#

CC        := gcc
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/cfs_sim

SRCS := userspace/main.c userspace/worker.c userspace/ui.c
OBJS := $(patsubst userspace/%.c,$(BUILD_DIR)/%.o,$(SRCS))
HDRS := include/cfs_shared.h userspace/worker.h userspace/ui.h
SCRIPTS := scripts/setup_vm_deps.sh scripts/patch_kernel.sh scripts/run_demo.sh

CFLAGS       := -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE -Iinclude -Iuserspace
CHECK_CFLAGS := $(CFLAGS) -Wpedantic -Wshadow -Wformat=2 -Werror
LDFLAGS      :=

# Optional arguments forwarded by `make demo ARGS="-n 6 -q 100 -w 80"`
ARGS ?=

.PHONY: all clean check demo help

all: $(TARGET)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: userspace/%.c $(HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
	@echo "[OK] Built user-space simulator: $@"

check:
	@echo "[CHECK] Running strict C11 syntax and warning analysis..."
	@set -e; for src in $(SRCS); do \
		echo "  -> $$src"; \
		$(CC) $(CHECK_CFLAGS) -fsyntax-only "$$src"; \
	done
	@echo "[CHECK] Validating bash script syntax..."
	@set -e; for script in $(SCRIPTS); do \
		echo "  -> $$script"; \
		bash -n "$$script"; \
	done
	@echo "[OK] All C11 and bash checks passed with zero warnings."

demo: $(TARGET)
	./$(TARGET) $(ARGS)

clean:
	rm -rf $(BUILD_DIR)
	@echo "[OK] Cleaned build artifacts."

help:
	@echo "CFS-Inspired Time-Sharing Scheduler — Available Make Targets:"
	@echo "  make all             Build user-space simulator ($(TARGET))"
	@echo "  make check           Run strict GCC warning checks and bash syntax checks"
	@echo "  make demo [ARGS=...] Build and run the simulator (e.g. ARGS='-n 6 -q 100 -w 80')"
	@echo "  make clean           Remove build/ directory and compiled objects"

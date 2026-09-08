# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Cross-build for QEMU's `-machine virt` RV32 board: bare-metal (picolibc, no OS), so this cannot
# reuse the top-level Makefile's host-compiled targets - it builds its own bootable ELF, statically
# linking TickLE's own src/*.c (including src/hal_freertos.c) alongside FreeRTOS and lwIP, instead
# of building libtickle.a + POSIX example binaries. See milestones in
# /home/semih/.claude/plans/lively-sauteeing-rainbow.md - this Makefile currently builds through
# milestone 3 (real virtio-net link, one QEMU instance; the two-instance round trip is milestone 4).

CROSS ?= riscv64-unknown-elf-
CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy

ROOT = ../..
FREERTOS = $(ROOT)/third_party/FreeRTOS-Kernel
RISCV_PORT = $(FREERTOS)/portable/GCC/RISC-V
CHIP_EXT = $(RISCV_PORT)/chip_specific_extensions/RV32I_CLINT_no_extensions
LWIP = $(ROOT)/third_party/lwip
LWIP_FREERTOS_PORT = $(LWIP)/contrib/ports/freertos

ARCH_FLAGS = -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medlow

# -DTT_PLATFORM_FREERTOS: see include/tickle/hal.h's platform detection - unlike __linux__,
# FreeRTOS has no standard compiler-predefined macro of its own.
CFLAGS = $(ARCH_FLAGS) -O1 -g -Wall -Wextra -ffreestanding -fno-common -DTT_PLATFORM_FREERTOS \
         -I. -Iboard -Ilwip_arch \
         -I$(FREERTOS)/include -I$(RISCV_PORT) \
         -I$(LWIP)/src/include -I$(LWIP_FREERTOS_PORT)/include \
         -I$(ROOT)/include -I$(ROOT)/src

# portASM.S includes "freertos_risc_v_chip_specific_extensions.h" via the assembler's include
# path, per FreeRTOS's own RISC-V port convention (see that header's own comment).
ASFLAGS = $(ARCH_FLAGS) -I$(CHIP_EXT) -I.

# --specs=picolibc.specs: this Ubuntu toolchain pairs gcc-riscv64-unknown-elf with picolibc, not
# newlib - it supplies the isystem include path and -lc. -nostartfiles overrides its default
# crt0/linker script so our own start.S/link.ld stay in control of boot and memory layout.
CFLAGS += --specs=picolibc.specs
LDFLAGS = $(ARCH_FLAGS) -nostartfiles --specs=picolibc.specs -T board/link.ld \
          -Wl,--gc-sections -Wl,-Map=RTOSDemo.map

FREERTOS_SRCS = $(FREERTOS)/tasks.c $(FREERTOS)/list.c $(FREERTOS)/queue.c \
                $(FREERTOS)/timers.c $(FREERTOS)/event_groups.c \
                $(FREERTOS)/portable/MemMang/heap_4.c \
                $(RISCV_PORT)/port.c

FREERTOS_ASM = $(RISCV_PORT)/portASM.S

# UDP/IPv4-only subset (LWIP_TCP/LWIP_ARP/LWIP_DHCP/LWIP_IGMP are all off - see lwipopts.h, this
# target has no real Ethernet under it yet, and TickLE itself is UDP-only).
LWIP_SRCS = $(LWIP)/src/core/def.c $(LWIP)/src/core/inet_chksum.c $(LWIP)/src/core/init.c \
            $(LWIP)/src/core/ip.c $(LWIP)/src/core/mem.c $(LWIP)/src/core/memp.c \
            $(LWIP)/src/core/netif.c $(LWIP)/src/core/pbuf.c $(LWIP)/src/core/stats.c \
            $(LWIP)/src/core/sys.c $(LWIP)/src/core/timeouts.c $(LWIP)/src/core/udp.c \
            $(LWIP)/src/core/ipv4/ip4.c $(LWIP)/src/core/ipv4/ip4_addr.c \
            $(LWIP)/src/core/ipv4/ip4_frag.c $(LWIP)/src/core/ipv4/icmp.c \
            $(LWIP)/src/api/api_lib.c $(LWIP)/src/api/api_msg.c $(LWIP)/src/api/err.c \
            $(LWIP)/src/api/netbuf.c $(LWIP)/src/api/netifapi.c $(LWIP)/src/api/sockets.c \
            $(LWIP)/src/api/tcpip.c \
            $(LWIP_FREERTOS_PORT)/sys_arch.c

BOARD_SRCS = board/start.S board/uart.c board/stdio_uart.c board/runtime_stubs.c board/virtio_net.c

# TickLE's own protocol/encoding sources plus this platform's HAL - the same src/*.c the
# top-level Makefile builds into libtickle.a for other platforms, just cross-compiled here
# instead of built as a separate static archive (no host-side build system to link against).
TICKLE_SRCS = $(ROOT)/src/tickle.c $(ROOT)/src/encoding.c $(ROOT)/src/log.c $(ROOT)/src/hal_freertos.c

SRCS = main.c net_init.c $(BOARD_SRCS) $(TICKLE_SRCS) $(FREERTOS_SRCS) $(FREERTOS_ASM) $(LWIP_SRCS)

# REPO_ROOT-relative object paths (via $(abspath) + stripping that prefix), not a plain
# `$(OBJ_DIR)/$(src).o` pattern: several SRCS entries reach outside this directory with a
# leading `../../` (TICKLE_SRCS, FREERTOS_SRCS, LWIP_SRCS), and naively prepending $(OBJ_DIR)/
# to those lets the `..` components normalize straight back out of $(OBJ_DIR) entirely (e.g.
# `obj/../../src/tickle.c.o` resolves to a path *outside* obj/) - silently escaping `make
# clean` and letting stale .o files survive a supposedly-clean rebuild.
REPO_ROOT = $(abspath $(ROOT))
OBJ_DIR = obj
OBJS = $(foreach src,$(SRCS),$(OBJ_DIR)/$(patsubst $(REPO_ROOT)/%,%,$(abspath $(src))).o)

.PHONY: all clean run lint

all: RTOSDemo.elf

$(OBJ_DIR)/%.c.o: $(REPO_ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/%.S.o: $(REPO_ROOT)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

RTOSDemo.elf: $(OBJS) board/link.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

# Auto-generated per-object .d files (headers each .c actually used, e.g. FreeRTOSConfig.h) -
# pulling these in means changing a header rebuilds everything that includes it, instead of
# `make` only checking the .c file's own mtime and reusing a now-stale .o.
-include $(OBJS:.o=.d)

# -bios none: no OpenSBI/prior firmware - our ELF's entry point runs directly in M-mode.
# -netdev socket,mcast=: the same UDP-multicast "virtual hub" two instances of this invocation
# will join in milestone 4 to simulate the two-node broadcast segment (see the plan) - joining it
# solo here still lets -object filter-dump capture what this instance actually transmits, for a
# single-instance smoke test of the driver itself.
# -global virtio-mmio.force-legacy=off: QEMU's virtio-mmio bus defaults to legacy (v1) transport
# for backward compatibility with old machine types; this driver only implements modern (v2) -
# see virtio_net.c's file-level comment for why.
run: RTOSDemo.elf
	qemu-system-riscv32 -machine virt -nographic -bios none -kernel RTOSDemo.elf \
	    -global virtio-mmio.force-legacy=off \
	    -netdev socket,id=net0,mcast=230.0.0.1:5000 \
	    -device virtio-net-device,netdev=net0 \
	    -object filter-dump,id=dump0,netdev=net0,file=net0.pcap

clean:
	rm -rf $(OBJ_DIR) RTOSDemo.elf RTOSDemo.map

# clang-tidy uses clang's own frontend, not gcc, so it doesn't understand --specs=picolibc.specs
# (a gcc-only mechanism) - --target names the same triple explicitly and -isystem points
# directly at picolibc's own include dir instead. Covers hal_freertos.c/h too (they live outside
# this directory, alongside hal_linux.*, but need these same cross-compile flags - the top-level
# Makefile's own `lint` explicitly excludes both this directory and hal_freertos.* for that
# reason, deferring to this target instead).
PICOLIBC_INCLUDE = /usr/lib/picolibc/riscv64-unknown-elf/include
TIDY_ARGS = --extra-arg=--target=riscv32-unknown-elf --extra-arg=-isystem --extra-arg=$(PICOLIBC_INCLUDE) \
            --extra-arg=-DTT_PLATFORM_FREERTOS \
            --extra-arg=-I. --extra-arg=-Iboard --extra-arg=-Ilwip_arch \
            --extra-arg=-I$(FREERTOS)/include --extra-arg=-I$(RISCV_PORT) \
            --extra-arg=-I$(LWIP)/src/include --extra-arg=-I$(LWIP_FREERTOS_PORT)/include \
            --extra-arg=-I$(ROOT)/include --extra-arg=-I$(ROOT)/src

# hal_freertos.c/h live under include/src, outside this directory's own .clang-tidy (which
# disables misc-include-cleaner/performance-no-int-to-ptr for the same reasons given there) - so
# repeat that same override explicitly here via --checks.
HAL_FREERTOS_TIDY_CHECKS = --checks=-misc-include-cleaner,-performance-no-int-to-ptr

lint:
	find . -name '*.[ch]' -exec clang-format --dry-run --Werror {} +
	find . -name '*.[ch]' -exec clang-tidy $(TIDY_ARGS) {} +
	clang-format --dry-run --Werror $(ROOT)/src/hal_freertos.c $(ROOT)/include/tickle/hal_freertos.h
	clang-tidy $(TIDY_ARGS) $(HAL_FREERTOS_TIDY_CHECKS) $(ROOT)/src/hal_freertos.c $(ROOT)/include/tickle/hal_freertos.h

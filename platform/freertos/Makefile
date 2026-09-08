# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Cross-build for QEMU's `-machine virt` RV32 board: bare-metal (newlib, no OS), so this cannot
# reuse the top-level Makefile's host-compiled targets - it builds its own bootable ELF instead
# of libtickle.a + POSIX example binaries. See milestones in
# /home/semih/.claude/plans/lively-sauteeing-rainbow.md - this Makefile currently only builds
# milestone 1 (boot + UART, no lwIP/tickle yet).

CROSS ?= riscv64-unknown-elf-
CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy

ROOT = ../..
FREERTOS = $(ROOT)/third_party/FreeRTOS-Kernel
RISCV_PORT = $(FREERTOS)/portable/GCC/RISC-V
CHIP_EXT = $(RISCV_PORT)/chip_specific_extensions/RV32I_CLINT_no_extensions

ARCH_FLAGS = -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medlow

CFLAGS = $(ARCH_FLAGS) -O1 -g -Wall -Wextra -ffreestanding -fno-common \
         -I. -Iboard \
         -I$(FREERTOS)/include -I$(RISCV_PORT) \
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

BOARD_SRCS = board/start.S board/uart.c board/stdio_uart.c

SRCS = main.c $(BOARD_SRCS) $(FREERTOS_SRCS) $(FREERTOS_ASM)

OBJ_DIR = obj
OBJS = $(patsubst %,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean run

all: RTOSDemo.elf

$(OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

RTOSDemo.elf: $(OBJS) board/link.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

# -bios none: no OpenSBI/prior firmware - our ELF's entry point runs directly in M-mode. Two
# instances of this same invocation (each given its own -netdev, once milestone 3+ adds one)
# are how the two-node broadcast segment is simulated - see the plan.
run: RTOSDemo.elf
	qemu-system-riscv32 -machine virt -nographic -bios none -kernel RTOSDemo.elf

clean:
	rm -rf $(OBJ_DIR) RTOSDemo.elf RTOSDemo.map

/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Reset entry point for QEMU's `-machine virt`, booted with `-bios none`: QEMU sets pc to this
// ELF's entry point directly in M-mode, hart 0 only expected (see hart-parking below), no prior
// firmware/SBI layer has run.

.section .text.start
.global _start
_start:
    // QEMU virt can be started with -smp > 1; this port only supports single-hart. Every hart
    // other than 0 parks itself immediately instead of racing hart 0 through the init below.
    csrr t0, mhartid
    bnez t0, park

    // Required by the RISC-V calling convention for gp-relative addressing of small data;
    // norelax avoids the linker "optimizing" this exact sequence assuming gp is already valid,
    // which it isn't yet - this is what's setting it up.
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    la sp, _stack_top

    // Zero .bss - nothing has initialized it yet, and C code (including FreeRTOS/lwIP globals)
    // assumes zero-initialized statics.
    la t0, __bss_start
    la t1, __bss_end
clear_bss:
    bge t0, t1, bss_done
    sw zero, 0(t0)
    addi t0, t0, 4
    j clear_bss
bss_done:

    // FreeRTOS-Kernel's RISC-V port (portASM.S) provides this as the single entry point for all
    // traps (exceptions, the mtimer tick, and - once used - external/software interrupts).
    la t0, freertos_risc_v_trap_handler
    csrw mtvec, t0

    call main

hang:
    j hang

park:
    wfi
    j park

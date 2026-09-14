/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

/* Minimal linker script for QEMU's `-machine virt` RV32 board, booted with `-bios none` (no
 * OpenSBI - QEMU jumps straight to _start in M-mode at RAM's base). RAM base/size match QEMU
 * virt's documented memory map (0x80000000, size as given to `-m`). */

OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64M
}

STACK_SIZE = 8K;

SECTIONS {
    .text : {
        *(.text.start) /* _start first, always */
        *(.text*)
    } > RAM

    .rodata : {
        *(.rodata*)
    } > RAM

    .data : {
        . = ALIGN(8);
        /* Conventional placement so 12-bit gp-relative offsets (+/-2KB) reach the small-data
         * area around here - see the `la gp, __global_pointer$` sequence in start.S. */
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.data*)
    } > RAM

    .bss : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(16);
    __heap_start = .;
    . = ORIGIN(RAM) + LENGTH(RAM) - STACK_SIZE;
    __heap_end = .;

    .stack (NOLOAD) : {
        . = ALIGN(16);
        _stack_bottom = .;
        . += STACK_SIZE;
        _stack_top = .;
    } > RAM
}

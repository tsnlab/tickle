/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include "uart.h"

#include <stdint.h>

// QEMU `-machine virt`'s fixed ns16550a UART0 address.
#define UART0_BASE 0x10000000UL

#define UART_REG(offset) (*(volatile uint8_t*)(UART0_BASE + (offset)))
#define UART_RBR UART_REG(0) // Receiver Buffer Register (read, DLAB=0)
#define UART_THR UART_REG(0) // Transmitter Holding Register (write, DLAB=0)
#define UART_DLL UART_REG(0) // Divisor Latch Low (DLAB=1)
#define UART_IER UART_REG(1) // Interrupt Enable Register (DLAB=0)
#define UART_DLM UART_REG(1) // Divisor Latch High (DLAB=1)
#define UART_FCR UART_REG(2) // FIFO Control Register
#define UART_LCR UART_REG(3) // Line Control Register
#define UART_LSR UART_REG(5) // Line Status Register

#define UART_LCR_DLAB 0x80
#define UART_LCR_8N1 0x03
#define UART_FCR_ENABLE_FIFO_CLEAR 0x07
#define UART_LSR_DATA_READY 0x01
#define UART_LSR_THR_EMPTY 0x20

void uart_init(void) {
    UART_IER = 0x00; // No interrupts - this driver polls
    UART_LCR = UART_LCR_DLAB;
    UART_DLL = 0x01; // Divisor is irrelevant to QEMU's emulated UART, but set for real-hardware correctness
    UART_DLM = 0x00;
    UART_LCR = UART_LCR_8N1;
    UART_FCR = UART_FCR_ENABLE_FIFO_CLEAR;
}

void uart_putc(char c) {
    while ((UART_LSR & UART_LSR_THR_EMPTY) == 0) {
    }
    UART_THR = (uint8_t)c;
}

int uart_getc_nonblock(void) {
    if ((UART_LSR & UART_LSR_DATA_READY) == 0) {
        return -1;
    }
    return UART_RBR;
}

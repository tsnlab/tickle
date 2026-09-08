/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

// Minimal polling driver for the ns16550a-compatible UART QEMU's `-machine virt` provides at a
// fixed address, wired to whatever `-serial` chardev the QEMU invocation specifies (a real
// terminal, a log file, or - for the two-instance test - a per-instance pty/file).

void uart_init(void);
void uart_putc(char c);
// Returns the next received byte, or -1 if none is available right now (never blocks).
int uart_getc_nonblock(void);

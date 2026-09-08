/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// picolibc expects the application to provide stdin/stdout/stderr itself (see stdio.h's
// "__PICOLIBC_STDIO_GLOBALS" - there's no default weak definition to fall back on for a bare-
// metal target), wired to a device through FDEV_SETUP_STREAM's put/get function pointers.

#include <stdio.h>

#include "uart.h"

static int uart_put(char c, FILE* file) {
    (void)file;
    if (c == '\n') {
        uart_putc('\r');
    }
    uart_putc(c);
    return 0;
}

static int uart_get(FILE* file) {
    (void)file;
    int c = uart_getc_nonblock();
    return c < 0 ? _FDEV_EOF : c;
}

static FILE uart_stream = FDEV_SETUP_STREAM(uart_put, uart_get, NULL, _FDEV_SETUP_RW);

FILE* const stdin = &uart_stream;
FILE* const stdout = &uart_stream;
FILE* const stderr = &uart_stream;

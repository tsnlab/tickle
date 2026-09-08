/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// FreeRTOS hooks every role (main.c, main_ping.c, main_pong.c) needs identically - split out
// here so each role's main file doesn't have to repeat them.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

void vApplicationMallocFailedHook(void) {
    printf("tickle/freertos: malloc failed\n");
    for (;;) {
    }
}

// FreeRTOS's own task.h declares this with its Hungarian-notation parameter names
// (xTask/pcTaskName); this project doesn't use that convention, so it keeps its own descriptive
// names instead.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void vApplicationStackOverflowHook(TaskHandle_t task, char* task_name) {
    (void)task;
    printf("tickle/freertos: stack overflow in task '%s'\n", task_name);
    for (;;) {
    }
}

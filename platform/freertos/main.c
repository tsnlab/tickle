/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Milestone 1 harness: boot FreeRTOS on QEMU's `-machine virt` and prove the toolchain + board
// glue work at all, before any lwIP/tickle code is wired in (that's milestone 2+, see
// /home/semih/.claude/plans/lively-sauteeing-rainbow.md).

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include "board/uart.h"

static void heartbeat_task(void* param) {
    (void)param;
    uint32_t count = 0;
    for (;;) {
        printf("tickle/freertos: alive (%lu)\n", (unsigned long)count++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: boot OK, starting FreeRTOS scheduler\n");

    xTaskCreate(heartbeat_task, "heartbeat", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void) {
    printf("tickle/freertos: malloc failed\n");
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char* task_name) {
    (void)task;
    printf("tickle/freertos: stack overflow in task '%s'\n", task_name);
    for (;;) {
    }
}

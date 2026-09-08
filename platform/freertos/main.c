/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Milestone 2 harness: bring up lwIP + hal_freertos.c's loopback netif, then run a real
// tt_Node_create()/tt_Node_poll() loop and confirm it doesn't crash - see
// /home/semih/.claude/plans/lively-sauteeing-rainbow.md. node_update()'s very first broadcast
// (scheduled within ~1ms of tt_Node_create(), see tickle.c) loops straight back to this same
// node via net_init.c's software loopback, so a poll should come back tt_RET_OK almost
// immediately - a real round trip through lwIP's UDP/IP stack, not just "didn't crash".

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "board/uart.h"
#include "net_init.h"

#define SELFTEST_POLL_COUNT 10
#define SELFTEST_POLL_TIMEOUT_NS (500LL * tt_MILLISECOND)
#define SELFTEST_TASK_STACK_WORDS 1024

// Too large for a task's own stack (tx_buffer/rx_buffer alone are ~5.75KB) - static instead.
static struct tt_Node node;

static void tickle_selftest_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != tt_RET_OK) {
        printf("tickle/freertos: tt_Node_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("tickle/freertos: node created, id=%u\n", node.id);

    for (int i = 0; i < SELFTEST_POLL_COUNT; i++) {
        tt_ret_t poll_ret = tt_Node_poll(&node, SELFTEST_POLL_TIMEOUT_NS);
        printf("tickle/freertos: poll[%d] -> %d\n", i, poll_ret);
    }

    printf("tickle/freertos: milestone 2 self-test done\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: boot OK, starting FreeRTOS scheduler\n");

    xTaskCreate(tickle_selftest_task, "tickle", SELFTEST_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

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

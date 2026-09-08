/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Milestone 3 harness: bring up lwIP over the real virtio-net link (net_init.c), then run a
// real tt_Node_create()/tt_Node_poll() loop - see
// /home/semih/.claude/plans/lively-sauteeing-rainbow.md. Unlike milestone 2 (a software loopback
// netif, where node_update()'s own broadcast looped straight back and every poll came back
// tt_RET_OK), this is a single, unconnected QEMU instance talking to a real NIC with nobody on
// the other end - every poll legitimately timing out (tt_RET_TIMEOUT) is the *expected* result
// here, not a regression. What this milestone actually checks is that virtio_net_init()
// succeeds and tt_Node_create()/tt_Node_poll() run to completion without hanging or crashing
// while real packets are actually going out - see platform/freertos/Makefile's `run` target,
// which captures those into net0.pcap for inspection. The real round trip is milestone 4, with
// two instances of this same image on each end of the link.

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

    printf("tickle/freertos: milestone 3 self-test done\n");
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

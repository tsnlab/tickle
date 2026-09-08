/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=perf_server: see main_perf_client.c's file-level comment for why this exists and why it
// reuses examples/linux/perf/Bulk.{c,h} but not perf_server.c itself.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "Bulk.h"
#include "board/uart.h"
#include "net_init.h"

#define PERF_SERVER_TASK_STACK_WORDS 1024

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Node node;
static struct tt_Subscriber sub;

static void bulk_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct BulkData* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no; // truncated to 16 bits by the framework; data->seq is the real 32-bit one
    printf("perf_server: recv seq=%lu size=%lu\n", (unsigned long)data->seq, (unsigned long)data->size);
}

static void perf_server_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != tt_RET_OK) {
        printf("perf_server: tt_Node_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("perf_server: node created, id=%u\n", node.id);

    ret = tt_Node_create_subscriber(&node, &sub, &BulkTopic, "bulk_topic", (tt_SUBSCRIBER_CALLBACK)bulk_callback);
    if (ret != tt_RET_OK) {
        printf("perf_server: tt_Node_create_subscriber failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("perf_server: ready\n");

    for (;;) {
        tt_Node_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: perf_server role boot OK\n");

    xTaskCreate(perf_server_task, "perf_server", PERF_SERVER_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}

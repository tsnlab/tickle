/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=publisher: the pub/sub counterpart to ROLE=ping/pong's RPC round trip (see main_ping.c's
// file-level comment) - reuses examples/linux/uint64/UInt64.{c,h} verbatim, same reasoning as main_ping
// reusing PingPong.{c,h} over ping.c itself.
//
// This exists specifically to give the QEMU tier real-HAL coverage of tt_Publisher_publish()'s
// batched (not immediately flushed) send path - unlike tt_Client_call() (see DESIGN.md's "RPC
// flushes immediately; Publish batches"), a publish only actually reaches the wire once
// tt_Node_poll()'s periodic flush fires, which the RPC-only ping/pong round trip never exercises
// at all.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "UInt64.h"
#include "board/uart.h"
#include "net_init.h"

#define PUBLISH_INTERVAL_NS (500LL * tt_MILLISECOND)
#define PUBLISHER_TASK_STACK_WORDS 1024

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Node node;
static struct tt_Publisher pub;
static uint64_t next_value = 0;

static void publish_value(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;

    struct UInt64Data data = {.data = next_value};
    tt_ret_t ret = tt_Publisher_publish(pub, (struct tt_Data*)&data);
    if (ret == tt_RET_OK) {
        printf("publisher: sent data=%lu\n", (unsigned long)next_value);
        next_value++;
    } else {
        printf("publisher: cannot publish: %d\n", ret);
    }

    tt_Node_schedule(node, time + PUBLISH_INTERVAL_NS, publish_value, pub);
}

static void publisher_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != tt_RET_OK) {
        printf("publisher: tt_Node_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("publisher: node created, id=%u\n", node.id);

    ret = tt_Node_create_publisher(&node, &pub, &UInt64Topic, "uint64_topic");
    if (ret != tt_RET_OK) {
        printf("publisher: tt_Node_create_publisher failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    tt_Node_schedule(&node, tt_get_ns(), publish_value, &pub);

    for (;;) {
        tt_Node_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: publisher role boot OK\n");

    xTaskCreate(publisher_task, "publisher", PUBLISHER_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}

/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=subscriber: see main_publisher.c's file-level comment for why this exists and why it
// reuses examples/linux/uint64/UInt64.{c,h} but not subscriber.c itself.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "UInt64.h"
#include "board/uart.h"
#include "net_init.h"

#define SUBSCRIBER_TASK_STACK_WORDS 1024

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Context node;
static struct tt_Subscriber sub;
// The endpoint's storage for the default QoS (RELIABLE, KEEP_LAST 10): static, so that `size` of the ELF counts it.
// tt_Subscriber_storage_size() asks for this for UInt64: a reorder slot per sample of the 256-sample window, 32 bytes
// each (fewer slots storm under loss, so they are refused); the create checks rather than trusting it.
#define SUB_STORAGE_BYTES 8192U
static uint64_t sub_storage[SUB_STORAGE_BYTES / sizeof(uint64_t)]; // 8-aligned, as the create requires

static void subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                struct UInt64Data* data) {
    (void)subscriber;
    (void)time;
    // %lx (hex), not %lu - matches examples/linux/uint64/subscriber.c's own uint64_data_callback().
    printf("subscriber: seq=%u data=%lx\n", seq_no, (unsigned long)data->data);
}

static void subscriber_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Context_create(&node);
    if (ret != tt_RET_OK) {
        printf("subscriber: tt_Context_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("subscriber: node created, id=%u\n", node.id);

    // The default QoS (a NULL profile): RELIABLE, KEEP_LAST 10, VOLATILE - rmw's default, as the publisher's.
    size_t storage_size = tt_Subscriber_storage_size(&UInt64Topic, NULL);
    printf("subscriber: storage %u of %u bytes\n", (unsigned)storage_size, (unsigned)sizeof(sub_storage));
    struct tt_SubscriberOptions sub_options = {.callback = (tt_SUBSCRIBER_CALLBACK)subscriber_callback};
    ret = tt_Node_create_subscriber(tt_Context_default_node(&node), &sub, &UInt64Topic, "uint64_topic", NULL,
                                    &sub_options, sub_storage, sizeof(sub_storage));
    if (ret != tt_RET_OK) {
        printf("subscriber: tt_Node_create_subscriber failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("subscriber: ready\n");

    for (;;) {
        tt_Context_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: subscriber role boot OK\n");

    xTaskCreate(subscriber_task, "subscriber", SUBSCRIBER_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}

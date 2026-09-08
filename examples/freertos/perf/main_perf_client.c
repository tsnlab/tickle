/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=perf_client: the perf (throughput) round trip's sending side - reuses
// examples/linux/perf/Bulk.{c,h} verbatim (pure protocol encode/decode code, no POSIX
// dependency), same reasoning as main_ping.c reusing PingPong.{c,h} over ping.c itself. Unlike
// examples/linux/perf/perf_client.c (which defaults to filling a whole Ethernet frame - see its
// own comment - to measure real throughput), this sends a small, fixed-size message at a modest,
// fixed interval: this exists to prove the round trip works under QEMU/virtio-net, not to
// measure throughput (that's the two-real-Raspberry-Pi HIL benchmark's job - see
// .github/workflows/performance.yml). Logs one line per send ("perf_client: sent seq=N") rather
// than a final summary, since - like main_ping.c/main_client.c - this task runs forever;
// platform/freertos/test.sh counts these per-send lines the same way it already does for the
// other pairs.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "Bulk.h"
#include "board/uart.h"
#include "net_init.h"

#define PERF_MESSAGE_SIZE 64
#define PERF_INTERVAL_NS (200LL * tt_MILLISECOND)
#define PERF_CLIENT_TASK_STACK_WORDS 1024

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Node node;
static struct tt_Publisher pub;
static struct BulkData bulk = {0}; // zero-initialized, reused for every publish

static void publish_bulk(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;

    bulk.size = PERF_MESSAGE_SIZE;
    tt_ret_t ret = tt_Publisher_publish(pub, (struct tt_Data*)&bulk);
    if (ret == tt_RET_OK) {
        printf("perf_client: sent seq=%lu\n", (unsigned long)bulk.seq);
        bulk.seq++;
    } else {
        printf("perf_client: cannot publish: %d\n", ret);
    }

    tt_Node_schedule(node, time + PERF_INTERVAL_NS, publish_bulk, pub);
}

static void perf_client_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != tt_RET_OK) {
        printf("perf_client: tt_Node_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("perf_client: node created, id=%u\n", node.id);

    ret = tt_Node_create_publisher(&node, &pub, &BulkTopic, "bulk_topic");
    if (ret != tt_RET_OK) {
        printf("perf_client: tt_Node_create_publisher failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    tt_Node_schedule(&node, tt_get_ns(), publish_bulk, &pub);

    for (;;) {
        tt_Node_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: perf_client role boot OK\n");

    xTaskCreate(perf_client_task, "perf_client", PERF_CLIENT_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}

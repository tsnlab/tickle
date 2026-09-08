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
// dependency), same reasoning as main_ping.c reusing PingPong.{c,h} over ping.c itself. Same
// intent as examples/linux/perf/perf_client.c's own defaults too now (see its own comment on why
// throttling it down doesn't make sense on either platform): fills a full Ethernet frame and
// republishes itself as soon as tt_Node_poll() next runs it, i.e. as fast as this platform's
// virtio-net driver and lwIP stack actually allow - real max throughput, not a fixed rate.
//
// Reports once a second (interval_sent_msgs/bytes, like perf_client.c's own report()) rather than
// logging every single publish - unlike a receive callback, this runs inside the same scheduled-
// task loop as tt_Node_poll() itself, so at max rate that could be many hundreds of printf()s a
// second onto a byte-at-a-time UART, dominating the loop's own timing instead of just observing
// it. platform/freertos/test.sh counts these periodic report lines the same way it already counts
// per-message lines for the other pairs.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "../../format.h"
#include "Bulk.h"
#include "board/uart.h"
#include "net_init.h"

#define PERF_CLIENT_TASK_STACK_WORDS 1024

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Node node;
static struct tt_Publisher pub;
static struct BulkData bulk = {0}; // zero-initialized, reused for every publish

static uint64_t total_buffer_full = 0;
static uint64_t interval_sent_msgs = 0;
static uint64_t interval_sent_bytes = 0;

// Field set and wording mirror examples/linux/perf/perf_client.c's own report(): MB/Mbps computed
// from this interval's bytes, no running sent-total (only perf_server.c's own RESULT line -
// which can see loss - is authoritative; this side is just visibility into what it attempted).
static void report(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;

    char sent_buf[TT_GROUPED_BUF_LEN];
    char buffer_full_buf[TT_GROUPED_BUF_LEN];
    const double bytes_per_mb = 1e6;
    double megabytes = (double)interval_sent_bytes / bytes_per_mb;
    double mbps = ((double)interval_sent_bytes * 8) / bytes_per_mb;
    printf("perf_client: sent %s msgs, %.3f MB, %.3f Mbps this interval (%s buffer-full so far)\n",
           tt_format_grouped(interval_sent_msgs, sent_buf), megabytes, mbps,
           tt_format_grouped(total_buffer_full, buffer_full_buf));

    interval_sent_msgs = 0;
    interval_sent_bytes = 0;

    tt_Node_schedule(node, time + tt_SECOND, report, NULL);
}

static void publish_bulk(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;

    bulk.size = BULK_MAX_PAYLOAD_SIZE;
    tt_ret_t ret = tt_Publisher_publish(pub, (struct tt_Data*)&bulk);
    if (ret == tt_RET_OK) {
        bulk.seq++;
        interval_sent_msgs++;
        interval_sent_bytes += bulk.size;
    } else if (ret == tt_RET_OUT_OF_BUFFER) {
        total_buffer_full++;
    }

    // Rescheduled for its own already-due time (not time + some interval): the next publish
    // becomes due again the moment tt_Node_poll() next processes the scheduler, which is exactly
    // "as fast as poll() allows" - the same target examples/linux/perf/perf_client.c's own -i 0
    // (its default) describes, just reached via this platform's scheduled-callback loop instead
    // of perf_client.c's manual next_send_time comparison.
    tt_Node_schedule(node, time, publish_bulk, pub);
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

    uint64_t start_time = tt_get_ns();
    tt_Node_schedule(&node, start_time, publish_bulk, &pub);
    tt_Node_schedule(&node, start_time + tt_SECOND, report, NULL);

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

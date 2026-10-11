/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// frag_rx: a receive counter for ../freertos_frag_q.sh. One cell per boot.
//
// Boots the FreeRTOS test tier's network (platform/freertos/net_init.c: virtio-net, lwIP over netconn), prints the
// lwIP reassembly configuration it was compiled with, binds UDP port FRAG_PORT and counts datagrams from the Linux
// sender (freertos_frag_send.py). Each datagram starts with a 16-byte header: magic, cell id, seq, n (MTU units),
// flags. A data datagram marks bit `seq` in a bitmap (distinct arrivals) and its length is checked against
// n * FRAG_UNIT. Once a second it prints a tick line (arrivals so far and the lwIP witnesses). An END datagram
// (flags & 1, seq = number sent) prints the final summary, the first gaps in the sequence, and ends QEMU through the
// virt test finisher.

#include <FreeRTOS.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <task.h>

#include <lwip/api.h>
#include <lwip/init.h>
#include <lwip/ip4_frag.h>
#include <lwip/memp.h>
#include <lwip/netbuf.h>
#include <lwip/opt.h>
#include <lwip/stats.h>

#include "board/uart.h"
#include "net_init.h"

#define FRAG_PORT 7650
#define FRAG_MAGIC 0x46524147U // "FRAG"
#define FRAG_UNIT 1446U
#define FRAG_FLAG_END 1U
#define FRAG_FLAG_WARMUP 2U // not counted: the first datagrams after boot lose 1-3 at >= 1.25 MB/s (probe2)
#define FRAG_HDR_BYTES 16U
#define BITMAP_BITS (1U << 20)
#define MAX_GAPS_PRINTED 48U
#define TICK_MS 1000U
#define RX_TASK_STACK_WORDS 2048

#define VIRT_TEST_FINISHER ((volatile uint32_t*)0x00100000UL)
#define FINISHER_PASS 0x5555U

static uint32_t bitmap[BITMAP_BITS / 32U];

static uint32_t get_u32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void print_witnesses(const char* tag) {
    const struct stats_mem* pool = lwip_stats.memp[MEMP_PBUF_POOL];
    const struct stats_mem* reass = lwip_stats.memp[MEMP_REASSDATA];
    printf("frag_rx: %s ipfrag_recv=%lu ipfrag_drop=%lu ipfrag_memerr=%lu ipfrag_err=%lu ip_recv=%lu ip_drop=%lu "
           "udp_recv=%lu pool_err=%lu pool_max=%lu pool_used=%lu reassdata_err=%lu reassdata_max=%lu mbox_err=%lu\n",
           tag, (unsigned long)lwip_stats.ip_frag.recv, (unsigned long)lwip_stats.ip_frag.drop,
           (unsigned long)lwip_stats.ip_frag.memerr, (unsigned long)lwip_stats.ip_frag.err,
           (unsigned long)lwip_stats.ip.recv, (unsigned long)lwip_stats.ip.drop, (unsigned long)lwip_stats.udp.recv,
           (unsigned long)pool->err, (unsigned long)pool->max, (unsigned long)pool->used, (unsigned long)reass->err,
           (unsigned long)reass->max, (unsigned long)lwip_stats.sys.mbox.err);
}

static void rx_task(void* param) {
    (void)param;
    net_init();

    printf("frag_rx: config LWIP_VERSION=%d.%d.%d IP_REASSEMBLY=%d IP_FRAG=%d IP_REASS_MAX_PBUFS=%d "
           "IP_REASS_MAXAGE=%d IP_TMR_INTERVAL_MS=%d MEMP_NUM_REASSDATA=%d PBUF_POOL_SIZE=%d "
           "PBUF_POOL_BUFSIZE=%d TCPIP_MBOX_SIZE=%d DEFAULT_UDP_RECVMBOX_SIZE=%d MEM_SIZE=%d LWIP_STATS=%d\n",
           LWIP_VERSION_MAJOR, LWIP_VERSION_MINOR, LWIP_VERSION_REVISION, IP_REASSEMBLY, IP_FRAG, IP_REASS_MAX_PBUFS,
           IP_REASS_MAXAGE, IP_TMR_INTERVAL, MEMP_NUM_REASSDATA, PBUF_POOL_SIZE, PBUF_POOL_BUFSIZE, TCPIP_MBOX_SIZE,
           DEFAULT_UDP_RECVMBOX_SIZE, MEM_SIZE, LWIP_STATS);

    struct netconn* conn = netconn_new(NETCONN_UDP);
    if (conn == NULL || netconn_bind(conn, IP_ADDR_ANY, FRAG_PORT) != ERR_OK) {
        printf("frag_rx: FATAL cannot bind port %d\n", FRAG_PORT);
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    netconn_set_recvtimeout(conn, 100);
    printf("frag_rx: ready port=%d\n", FRAG_PORT);

    uint32_t cell = 0;
    uint32_t n_units = 0;
    uint32_t got = 0;
    uint32_t dup = 0;
    uint32_t bad_len = 0;
    uint32_t foreign = 0;
    uint32_t warmup = 0;
    uint32_t max_seq = 0;
    TickType_t first_ms = 0;
    TickType_t last_tick = xTaskGetTickCount();

    for (;;) {
        struct netbuf* buf = NULL;
        err_t err = netconn_recv(conn, &buf);
        TickType_t now = xTaskGetTickCount();
        if (now - last_tick >= pdMS_TO_TICKS(TICK_MS)) {
            last_tick = now;
            char tag[96];
            snprintf(tag, sizeof(tag), "tick ms=%lu got=%lu", (unsigned long)now, (unsigned long)got);
            print_witnesses(tag);
        }
        if (err != ERR_OK || buf == NULL) {
            continue;
        }
        uint8_t hdr[FRAG_HDR_BYTES];
        uint16_t len = netbuf_len(buf);
        if (len < FRAG_HDR_BYTES || netbuf_copy(buf, hdr, FRAG_HDR_BYTES) != FRAG_HDR_BYTES ||
            get_u32(hdr) != FRAG_MAGIC) {
            foreign++;
            netbuf_delete(buf);
            continue;
        }
        netbuf_delete(buf);
        uint32_t this_cell = get_u32(hdr + 4);
        uint32_t seq = get_u32(hdr + 8);
        uint32_t n = get_u32(hdr + 12) & 0xffffU;
        uint32_t flags = get_u32(hdr + 12) >> 16;
        if (flags & FRAG_FLAG_WARMUP) {
            warmup++;
            continue;
        }
        if (cell == 0) {
            cell = this_cell;
            n_units = n;
            first_ms = now;
        }
        if (this_cell != cell) {
            foreign++;
            continue;
        }
        if (flags & FRAG_FLAG_END) {
            uint32_t sent = seq;
            uint32_t gaps = 0;
            uint32_t longest = 0;
            uint32_t run = 0;
            printf("frag_rx: gaps");
            for (uint32_t s = 0; s < sent && s < BITMAP_BITS; s++) {
                if ((bitmap[s / 32U] >> (s % 32U)) & 1U) {
                    if (run > 0) {
                        if (gaps < MAX_GAPS_PRINTED) {
                            printf(" %lu+%lu", (unsigned long)(s - run), (unsigned long)run);
                        }
                        gaps++;
                        longest = run > longest ? run : longest;
                        run = 0;
                    }
                } else {
                    run++;
                }
            }
            if (run > 0) {
                if (gaps < MAX_GAPS_PRINTED) {
                    printf(" %lu+%lu", (unsigned long)(sent - run), (unsigned long)run);
                }
                gaps++;
                longest = run > longest ? run : longest;
            }
            printf("\n");
            printf(
                "frag_rx: final cell=%lu n=%lu sent=%lu got=%lu dup=%lu bad_len=%lu foreign=%lu warmup=%lu max_seq=%lu "
                "gaps=%lu longest_gap=%lu span_ms=%lu\n",
                (unsigned long)cell, (unsigned long)n_units, (unsigned long)sent, (unsigned long)got,
                (unsigned long)dup, (unsigned long)bad_len, (unsigned long)foreign, (unsigned long)warmup,
                (unsigned long)max_seq, (unsigned long)gaps, (unsigned long)longest, (unsigned long)(now - first_ms));
            print_witnesses("stats");
            printf("frag_rx: END\n");
            vTaskDelay(pdMS_TO_TICKS(50));
            *VIRT_TEST_FINISHER = FINISHER_PASS;
            for (;;) {
            }
        }
        if (len != n * FRAG_UNIT) {
            bad_len++;
            continue;
        }
        if (seq >= BITMAP_BITS) {
            foreign++;
            continue;
        }
        if ((bitmap[seq / 32U] >> (seq % 32U)) & 1U) {
            dup++;
            continue;
        }
        bitmap[seq / 32U] |= 1U << (seq % 32U);
        got++;
        max_seq = seq > max_seq ? seq : max_seq;
    }
}

int main(void) {
    uart_init();
    printf("\nfrag_rx: boot\n");
    xTaskCreate(rx_task, "frag_rx", RX_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);
    vTaskStartScheduler();
    for (;;) {
    }
}

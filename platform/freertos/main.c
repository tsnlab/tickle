/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=selftest (the Makefile's default): the single-instance harness. platform/freertos/test.sh runs it once per
// RX_PATH (netconn, socket) before the two-instance pairs.
//
// It brings up lwIP over the real virtio-net link (net_init.c), creates a context, and checks, in order:
//   1. polls run and time out solo (the original milestone-3 check: nothing hangs or crashes with real packets going
//      out);
//   2. tt_Context_interrupt() wakes a poll - signalled before the poll (the signal must wait for it), and from another
//      task while the poll is blocked;
//   3. the receive path's cost: a sender task sends datagrams to this node's own address, alternating its well-known
//      and data ports, while the node polls - first with a waiting poll, then with a busy tt_Context_poll(0) loop. The
//      HAL counts what each read cost (struct tt_hal.rx_reads/rx_empty_reads/rx_quiet). Pre-registered reading:
//        - every build: core received every datagram sent (rx_malformed_drops rose by exactly the number sent - the
//          payload is deliberately not a TickLE packet), or the run says nothing;
//        - netconn build: at most 1 empty read per 20 datagrams in each phase, and the busy phase's quiet checks > 0
//          (the arrival count answered instead of lwIP - the hint is in use);
//        - socket build, the control: the busy phase has empty reads > 0 - the counter can see an empty read, so the
//          netconn build's zero is a measurement and not a blind instrument;
//      then tt_rx_maybe_ready() itself: true with a datagram queued and, in the netconn build, false once it is read.
// It ends the QEMU process through the virt machine's test device: exit status 0 on PASS, 1 on FAIL, so test.sh does
// not have to wait out a timeout.

#include <FreeRTOS.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <task.h>

#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>
#if tt_HAL_FREERTOS_NETCONN
#include <lwip/api.h>
#include <lwip/udp.h>
#endif

#include "board/uart.h"
#include "net_init.h"

#define SELFTEST_POLL_COUNT 3
#define SELFTEST_POLL_TIMEOUT_NS (500LL * tt_MILLISECOND)
#define SELFTEST_TASK_STACK_WORDS 1024
#define HELPER_TASK_STACK_WORDS 2048

#define INTERRUPT_DELAY_MS 200
#define INTERRUPT_SLACK_MS 50 // a wake must land this soon after the signal
#define SELF_SIGNAL_MAX_MS 100
#define INTERRUPT_BUDGET_NS (3LL * tt_SECOND)

#define RX_DATAGRAMS 100
#define RX_DATAGRAM_BYTES 64
#define RX_SETTLE_NS (200LL * tt_MILLISECOND) // polling continues this long after the last send
#define RX_WAITING_POLL_NS (50LL * tt_MILLISECOND)
#define RX_GAP_WAITING_MS 5
#define RX_GAP_BUSY_MS 2
#define EMPTY_READS_PER_DATAGRAM_MAX_DIV 20 // at most 1 empty read per 20 datagrams
#define PER_MILLE 1000U

// QEMU virt's "sifive_test" finisher: writing PASS exits QEMU with status 0, FAIL | code << 16 with that code.
#define VIRT_TEST_FINISHER ((volatile uint32_t*)0x00100000UL)
#define FINISHER_PASS 0x5555U
#define FINISHER_FAIL 0x3333U

// Too large for a task's own stack (tx_buffer/rx_buffer alone are ~5.75KB) - static instead.
static struct tt_Context node;
static bool failed;

static void check(bool passed, const char* what) {
    printf("selftest: %s %s\n", passed ? "ok  " : "FAIL", what);
    if (!passed) {
        failed = true;
    }
}

static void finish(void) {
    printf("selftest: %s\n", failed ? "FAIL" : "PASS");
    *VIRT_TEST_FINISHER = failed ? (FINISHER_FAIL | (1U << 16)) : FINISHER_PASS;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- 2. interrupt ---

static void interrupter_task(void* param) {
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(INTERRUPT_DELAY_MS));
    tt_Context_interrupt(&node);
    vTaskDelete(NULL);
}

static void check_interrupt(void) {
    // Signalled before anyone waits: the next poll must find it at once.
    tt_Context_interrupt(&node);
    uint64_t start = tt_get_ns();
    tt_ret_t ret = tt_Context_poll(&node, INTERRUPT_BUDGET_NS);
    uint64_t took_ms = (tt_get_ns() - start) / tt_MILLISECOND;
    printf("selftest: interrupt before poll -> %d after %llu ms\n", ret, (unsigned long long)took_ms);
    check(ret == tt_RET_INTERRUPTED && took_ms < SELF_SIGNAL_MAX_MS,
          "interrupt signalled before the poll ends it at once");

    // From another task while this one is blocked. A datagram that arrives meanwhile ends a poll with OK; poll again
    // for what is left of the budget.
    xTaskCreate(interrupter_task, "interrupter", HELPER_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 2, NULL);
    start = tt_get_ns();
    do {
        int64_t left = INTERRUPT_BUDGET_NS - (int64_t)(tt_get_ns() - start);
        ret = left > 0 ? tt_Context_poll(&node, left) : tt_RET_TIMEOUT;
    } while ((ret == tt_RET_OK || ret == tt_RET_TIMEOUT) && tt_get_ns() - start < (uint64_t)INTERRUPT_BUDGET_NS);
    took_ms = (tt_get_ns() - start) / tt_MILLISECOND;
    printf("selftest: interrupt from a task after %d ms -> %d after %llu ms\n", INTERRUPT_DELAY_MS, ret,
           (unsigned long long)took_ms);
    // Within INTERRUPT_SLACK_MS of the signal: a wake that only lands when the next timer ends the wait (an announce,
    // up to a second later) is the failure this exists to catch.
    check(ret == tt_RET_INTERRUPTED && took_ms >= INTERRUPT_DELAY_MS / 2 &&
              took_ms < INTERRUPT_DELAY_MS + INTERRUPT_SLACK_MS,
          "interrupt from another task wakes a blocked poll");
}

// --- 3. receive cost ---

struct sender {
    uint16_t data_port; // network order
    uint32_t gap_ms;
    uint32_t sent;
    volatile bool done;
};

static uint16_t own_data_port(void) {
#if tt_HAL_FREERTOS_NETCONN
    return lwip_htons(node.hal.data_conn->pcb.udp->local_port);
#else
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(node.hal.data_sock, (struct sockaddr*)&addr, &len) != 0) {
        return 0;
    }
    return addr.sin_port;
#endif
}

static int send_one(int sock, uint16_t port_network_order) {
    static const uint8_t junk[RX_DATAGRAM_BYTES] = {0}; // no TickLE magic: core counts it malformed and drops it
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_len = sizeof(dest);
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = ip4_addr_get_u32(netif_ip4_addr(netif_default));
    dest.sin_port = port_network_order;
    return sendto(sock, junk, sizeof(junk), 0, (struct sockaddr*)&dest, sizeof(dest)) == (int)sizeof(junk);
}

// One sending socket for the whole selftest, never closed: closing an lwIP socket makes lwIP's own event_callback()
// look the socket up after it is gone and set errno, and errno is thread-local in picolibc while this board never
// sets up tp - the store faults (found 2026-10-06; nothing in these tests closes a socket otherwise).
static int sender_socket(void) {
    static int sock = -1;
    if (sock < 0) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    return sock;
}

static void sender_task(void* param) {
    struct sender* sender = (struct sender*)param;
    int sock = sender_socket();
    for (uint32_t i = 0; sock >= 0 && i < RX_DATAGRAMS; i++) {
        uint16_t port = (i % 2U) == 0 ? lwip_htons(_tt_CONFIG.port) : sender->data_port;
        sender->sent += (uint32_t)send_one(sock, port);
        vTaskDelay(pdMS_TO_TICKS(sender->gap_ms));
    }
    sender->done = true;
    vTaskDelete(NULL);
}

struct rx_cost {
    uint32_t reads;
    uint32_t empty;
    uint32_t quiet;
};

// Runs one phase: a sender at `gap_ms`, this task polling with `poll_timeout` until RX_SETTLE_NS after the sender is
// done. Prints and returns what the HAL counted during the phase.
static struct rx_cost rx_phase(const char* name, uint32_t gap_ms, int64_t poll_timeout) {
    static struct sender sender;
    memset(&sender, 0, sizeof(sender));
    sender.data_port = own_data_port();
    sender.gap_ms = gap_ms;

    struct rx_cost before = {node.hal.rx_reads, node.hal.rx_empty_reads, node.hal.rx_quiet};
    uint64_t malformed = node.rx_malformed_drops;

    tt_log_set_level(TT_LOG_NONE); // every junk datagram would log "Illegal magic"
    xTaskCreate(sender_task, "sender", HELPER_TASK_STACK_WORDS, &sender, tskIDLE_PRIORITY + 2, NULL);
    uint64_t done_at = 0;
    while (done_at == 0 || tt_get_ns() - done_at < (uint64_t)RX_SETTLE_NS) {
        (void)tt_Context_poll(&node, poll_timeout);
        if (done_at == 0 && sender.done) {
            done_at = tt_get_ns();
        }
    }
    tt_log_set_level(TT_LOG_INFO);

    struct rx_cost cost = {node.hal.rx_reads - before.reads, node.hal.rx_empty_reads - before.empty,
                           node.hal.rx_quiet - before.quiet};
    uint32_t delivered = (uint32_t)(node.rx_malformed_drops - malformed);
    uint32_t milli = cost.reads == 0 ? 0 : (uint32_t)((uint64_t)cost.empty * PER_MILLE / cost.reads);
    printf("selftest: rx %-7s sent=%lu delivered=%lu reads=%lu empty_reads=%lu (%lu.%03lu per datagram) quiet=%lu\n",
           name, (unsigned long)sender.sent, (unsigned long)delivered, (unsigned long)cost.reads,
           (unsigned long)cost.empty, (unsigned long)(milli / PER_MILLE), (unsigned long)(milli % PER_MILLE),
           (unsigned long)cost.quiet);
    check(sender.sent == RX_DATAGRAMS && delivered == sender.sent, "every datagram sent reached core");
    return cost;
}

static void check_rx_cost(void) {
    struct rx_cost waiting = rx_phase("waiting", RX_GAP_WAITING_MS, RX_WAITING_POLL_NS);
    struct rx_cost busy = rx_phase("busy", RX_GAP_BUSY_MS, 0);
#if tt_HAL_FREERTOS_NETCONN
    check(waiting.empty * EMPTY_READS_PER_DATAGRAM_MAX_DIV <= waiting.reads,
          "netconn: at most 1 empty read per 20 datagrams, waiting poll");
    check(busy.empty * EMPTY_READS_PER_DATAGRAM_MAX_DIV <= busy.reads,
          "netconn: at most 1 empty read per 20 datagrams, busy poll");
    check(busy.quiet > 0, "netconn: the arrival count answered the busy poll's checks");
#else
    (void)waiting;
    check(busy.empty > 0, "socket (control): the counter sees the busy poll's empty reads");
#endif
}

static void check_rx_hint(void) {
    int sock = sender_socket();
    bool sent = sock >= 0 && send_one(sock, own_data_port());
    vTaskDelay(pdMS_TO_TICKS(20)); // the loopback queue and the tcpip thread deliver it
    check(sent && tt_rx_maybe_ready(&node), "tt_rx_maybe_ready() is true with a datagram queued");
    uint8_t buf[RX_DATAGRAM_BYTES * 2];
    uint32_t ip = 0;
    uint16_t port = 0;
    int32_t got = 0;
    int32_t len = 0;
    while ((len = tt_try_receive(&node, buf, sizeof(buf), &ip, &port)) >= 0) {
        got += len == RX_DATAGRAM_BYTES ? 1 : 0;
    }
    check(got == 1, "tt_try_receive() returns the queued datagram");
#if tt_HAL_FREERTOS_NETCONN
    check(!tt_rx_maybe_ready(&node), "netconn: tt_rx_maybe_ready() is false once it is read");
#endif
}

static void tickle_selftest_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Context_create(&node);
    if (ret != tt_RET_OK) {
        printf("tickle/freertos: tt_Context_create failed: %d\n", ret);
        failed = true;
        finish();
    }
    printf("tickle/freertos: node created, id=%u, rx path %s\n", node.id,
           tt_HAL_FREERTOS_NETCONN ? "netconn" : "socket");

    for (int i = 0; i < SELFTEST_POLL_COUNT; i++) {
        tt_ret_t poll_ret = tt_Context_poll(&node, SELFTEST_POLL_TIMEOUT_NS);
        printf("tickle/freertos: poll[%d] -> %d\n", i, poll_ret);
        check(poll_ret == tt_RET_OK || poll_ret == tt_RET_TIMEOUT, "poll returns OK or TIMEOUT");
    }

    check_interrupt();
    check_rx_cost();
    check_rx_hint();
    finish();
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

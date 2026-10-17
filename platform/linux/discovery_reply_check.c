// Copyright (c) 2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// A discovery reply never holds the polling thread on a full send buffer, and discovery still converges when the reply
// is left unsent. Against the real Linux HAL, run by discovery_reply_check.sh (test-linux), which makes the link.
//
// Why (2026-10-17): every datagram leaves through the data socket, discovery included. A large sample's fragments fill
// its send buffer, and the polling thread's discovery replies (process_announce() -> reply_with_own_announce(),
// process_acknack() -> answer_discovery_request()) were blocking sends made under the state lock: each waited until
// half the buffer had drained, ~220 ms behind test_large_publish_block's 100 Mbit/s link, and every rmw_publish()
// waited on the lock as long (38 such blocks in 80 runs, 10 publishes over 100 ms). A reply now goes only if there is
// room (tickle.c send_datagram_unless_full()); the peer pulls our list later.
//
// The link (discovery_reply_check.sh): A's context in this process's namespace, B's in another (B_NETNS, entered with
// setns() for B's sockets only), joined by a veth whose A end tbf shapes to RATE so that A's queue drains slowly.
// One thread polls both contexts. A has no endpoints, so the only things its polling thread sends are its summary
// once a second and the two replies under test. A's data socket is the one every send uses; this process keeps it
// full by sending FLOOD_BYTES datagrams on it to B's address at SINK_PORT (B's ingress drops them, so they cost B
// nothing) until the kernel says EAGAIN, before every poll of A - except around A's summary (PAUSE_NS before it is
// due until it has gone), which is a blocking broadcast this change leaves as it is and which would otherwise wait
// in both arms.
//
// Run:
//   1. warm-up - no flood; A and B learn each other's lists (WARMUP_NS at most, else FAIL).
//   2. flood   - FLOOD_NS long, starting just after B's own summary went (so B's next one is a second away). At its
//                start B creates a Publisher: B broadcasts its changed announce (A must reply: reply_with_own_announce)
//                and marks A's list to be acted on again (update_reprocess), so B asks for A's list (A must answer:
//                answer_discovery_request) when A's next summary reaches it, unless A's reply came first.
//   3. settle  - no flood; CONVERGE_NS for B to hold A's current list.
//
// The rules, enforced below (written before the first run):
//   FAIL (exit 1): warm-up did not converge - discovery failed with no load at all. (VOID in the first version; the
//     first mutant sweep showed a reply that never goes fails here, before any flood, and that is a failure.)
//   VOID (exit 3) unless: the buffer filled (EAGAIN) before every flood-window poll of A outside
//     the summary pauses; and A acted on B's changed announce in a flood-window poll that began with its buffer full -
//     the reply's treatment. A's answers to B's requests are counted the same way (answered_full), and reported.
//   FAIL (exit 1): any poll of A in the flood window took longer than BOUND_NS (50 ms). On the parent the reply waited
//     for half the buffer to drain: 163 ms at RATE.
//   FAIL (exit 1): B does not hold A's current list CONVERGE_NS after the flood ends - discovery did not converge with
//     the replies left unsent (a change that dropped them for good fails here).
//   Convergence is exercised only if B did not already hold A's list when the flood ended (the replies were skipped):
//     the line says which, and with the change in place exercised=1 is required (A skipped at least one reply), so a
//     run in which nothing was skipped cannot pass as one that converged after skipping.
// Exit 0 pass, 1 fail, 2 setup failed, 3 void.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier,readability-identifier-naming) - setns()
#endif
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507
#endif
#ifndef tt_LARGE_SAMPLES
#define tt_LARGE_SAMPLES 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <sched.h> // setns(), CLONE_NEWNET
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

#include "UInt64.h"

#define NS_PER_MS 1000000ULL
#define NS_PER_US 1000L
#define BOUND_NS (50ULL * NS_PER_MS) // the longest a poll of A may take in the flood window
#define WARMUP_NS (5000ULL * NS_PER_MS)
#define FLOOD_NS (2500ULL * NS_PER_MS) // two of A's summaries at least, so B asks while A is full
#define CONVERGE_NS (3000ULL * NS_PER_MS)
#define PAUSE_NS (30ULL * NS_PER_MS) // no refill this long before A's summary is due, until it has gone
#define SNDBUF_BYTES 65536 // A's data socket, set small so a half-drain is ~0.2 s at RATE (the kernel doubles it)
#define FLOOD_BYTES 1400   // one datagram of the flood: below the link's MTU
#define FLOOD_MAX 100000   // datagrams a refill sends at most before it calls the buffer unfillable
#define SINK_PORT 9        // B's ingress drops this port (discovery_reply_check.sh)
#define IDLE_US 200        // between rounds of polls
#define A_ID 1
#define B_ID 2

static struct tt_Context g_a;
static struct tt_Context g_b;
static struct tt_Publisher g_b_pub;

static uint64_t now_ns(void) {
    return tt_get_ns(); // the HAL's monotonic clock, the one core's own times (next_summary_ns) are read on
}

static void idle(void) {
    struct timespec ts = {0, IDLE_US * NS_PER_US};
    nanosleep(&ts, NULL);
}

// Whether `holder` holds `peer`'s current list: seen, applied at the generation `peer` is at now, nothing to redo.
static bool holds_list(const struct tt_Context* holder, const struct tt_Context* peer) {
    return holder->update_seen[peer->id] && !holder->update_reprocess[peer->id] &&
           holder->update_generation[peer->id] == (uint32_t)peer->last_modified;
}

// Fills A's data socket until the kernel refuses (EAGAIN): true when it did, false if it never filled.
static bool refill(const struct sockaddr_in* sink) {
    static uint8_t datagram[FLOOD_BYTES];
    for (int i = 0; i < FLOOD_MAX; i++) {
        if (sendto(g_a.hal.data_sock, datagram, sizeof(datagram), MSG_DONTWAIT, (const struct sockaddr*)sink,
                   sizeof(*sink)) < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
    }
    return false;
}

static bool create_in(struct tt_Context* node, uint8_t id, int netns_fd, int home_fd) {
    if (netns_fd >= 0 && setns(netns_fd, CLONE_NEWNET) != 0) {
        fprintf(stderr, "discovery_reply_check: setns: %s\n", strerror(errno));
        return false;
    }
    _tt_CONFIG.context_id = id;
    bool created = tt_Context_create(node) == tt_RET_OK;
    if (netns_fd >= 0 && setns(home_fd, CLONE_NEWNET) != 0) {
        fprintf(stderr, "discovery_reply_check: setns back: %s\n", strerror(errno));
        return false;
    }
    return created;
}

// The flood window's account of A (rules above).
struct flood_result {
    uint64_t slowest;
    uint32_t polls;
    uint32_t over_bound;
    uint32_t unfilled;
    bool reply_treated;     // A acted on B's changed announce in a poll that began full
    uint32_t answered_full; // B's requests A took in a poll that began full
};

static void poll_both(void) {
    (void)tt_Context_poll(&g_a, 0);
    (void)tt_Context_poll(&g_b, 0);
    idle();
}

static double to_ms(uint64_t nanoseconds) {
    return (double)nanoseconds / (double)NS_PER_MS;
}

// 1. warm-up: true once each holds the other's list, false after WARMUP_NS. Then on to just after B's summary went, so
// B's announce (the flood's first step) reaches A ahead of B's next summary.
static bool warm_up(void) {
    uint64_t start = now_ns();
    while (!(holds_list(&g_a, &g_b) && holds_list(&g_b, &g_a))) {
        if (now_ns() - start > WARMUP_NS) {
            printf("discovery_reply_check: FAIL - warm-up did not converge (A holds B %d, B holds A %d)\n",
                   holds_list(&g_a, &g_b), holds_list(&g_b, &g_a));
            return false;
        }
        poll_both();
    }
    uint64_t b_summary = g_b.summary_sent_ns;
    while (g_b.summary_sent_ns == b_summary || g_b.tx_tail != sizeof(struct tt_Header)) {
        poll_both();
    }
    return true;
}

// One poll of A in the flood window, refilled first unless `paused`, and what it showed.
static void flood_poll(const struct sockaddr_in* sink, bool paused, uint32_t b_generation_before, uint64_t flood_start,
                       struct flood_result* result) {
    bool full = false;
    if (!paused) {
        full = refill(sink);
        result->unfilled += full ? 0U : 1U;
    }
    uint32_t generation = g_a.update_generation[B_ID];
    uint64_t reply_tick = g_a.discovery_reply_tick;
    uint8_t reply_count = g_a.discovery_reply_count;
    uint64_t before = now_ns();
    (void)tt_Context_poll(&g_a, 0);
    uint64_t took = now_ns() - before;
    result->polls++;
    result->slowest = took > result->slowest ? took : result->slowest;
    if (took > BOUND_NS) {
        result->over_bound++;
        printf("discovery_reply_check: A's poll took %.1f ms at +%.0f ms (full=%d paused=%d)\n", to_ms(took),
               to_ms(before - flood_start), full, paused);
    }
    if (full && generation == b_generation_before && g_a.update_generation[B_ID] != b_generation_before) {
        result->reply_treated = true;
    }
    if (full && (g_a.discovery_reply_tick != reply_tick || g_a.discovery_reply_count != reply_count)) {
        result->answered_full++;
    }
}

// 2. flood: A's buffer kept full for FLOOD_NS but around A's summaries (B has just created its Publisher).
static void flood(const struct sockaddr_in* sink, uint32_t b_generation_before, struct flood_result* result) {
    uint64_t flood_start = now_ns();
    uint64_t a_summary = g_a.summary_sent_ns;
    bool paused = false;
    for (uint64_t now = flood_start; now - flood_start < FLOOD_NS; now = now_ns()) {
        if (!paused && now + PAUSE_NS >= g_a.next_summary_ns) {
            paused = true; // A's summary is due: let it find room
        }
        if (paused && g_a.summary_sent_ns != a_summary && g_a.tx_tail == sizeof(struct tt_Header)) {
            paused = false; // it has gone
            a_summary = g_a.summary_sent_ns;
        }
        flood_poll(sink, paused, b_generation_before, flood_start, result);
        (void)tt_Context_poll(&g_b, 0);
        idle();
    }
}

// 3. settle: how long after the flood B came to hold A's list, or UINT64_MAX if not within CONVERGE_NS.
static uint64_t settle(void) {
    uint64_t flood_end = now_ns();
    while (now_ns() - flood_end < CONVERGE_NS) {
        if (holds_list(&g_b, &g_a)) {
            return now_ns() - flood_end;
        }
        poll_both();
    }
    return holds_list(&g_b, &g_a) ? now_ns() - flood_end : UINT64_MAX;
}

static int judge(const struct flood_result* result, bool b_held_at_flood_end, uint64_t converged_after) {
#ifndef CHECK_ON_PARENT // the parent has no such counter: built with this defined, the line says -1
    long long skipped = (long long)g_a.large.discovery_replies_skipped;
#else
    long long skipped = -1;
#endif
    bool converged = converged_after != UINT64_MAX;
    printf("discovery_reply_check: polls=%u slowest_ms=%.1f over_bound=%u unfilled=%u reply_treated=%d "
           "answered_full=%u replies_skipped=%lld b_held_at_flood_end=%d converged=%d converged_after_ms=%.0f\n",
           result->polls, to_ms(result->slowest), result->over_bound, result->unfilled, result->reply_treated,
           result->answered_full, skipped, b_held_at_flood_end, converged, converged ? to_ms(converged_after) : -1.0);
    int verdict = 0;
    if (result->unfilled != 0 || !result->reply_treated) {
        printf("discovery_reply_check: VOID - the treatment did not apply (unfilled=%u reply_treated=%d)\n",
               result->unfilled, result->reply_treated);
        verdict = 3;
    }
    if (result->over_bound != 0) {
        printf("discovery_reply_check: FAIL - %u poll(s) of A over %llu ms, the slowest %.1f ms\n", result->over_bound,
               BOUND_NS / NS_PER_MS, to_ms(result->slowest));
        verdict = 1;
    }
    if (!converged) {
        printf("discovery_reply_check: FAIL - B did not hold A's list %llu ms after the flood\n",
               CONVERGE_NS / NS_PER_MS);
        verdict = 1;
    }
#ifndef CHECK_ON_PARENT
    if (verdict == 0 && (b_held_at_flood_end || skipped == 0)) {
        printf(
            "discovery_reply_check: VOID - convergence not exercised (b_held_at_flood_end=%d replies_skipped=%lld)\n",
            b_held_at_flood_end, skipped);
        verdict = 3;
    }
#endif
    return verdict;
}

// Both contexts up, A's in this namespace and B's in the one at `b_netns`, and A's data socket's buffer made small.
static bool set_up(const char* b_netns) {
    int home_fd = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    int b_fd = open(b_netns, O_RDONLY | O_CLOEXEC);
    if (home_fd < 0 || b_fd < 0) {
        fprintf(stderr, "discovery_reply_check: cannot open the namespaces: %s\n", strerror(errno));
        return false;
    }
    if (!create_in(&g_a, A_ID, -1, home_fd) || !create_in(&g_b, B_ID, b_fd, home_fd)) {
        fprintf(stderr, "discovery_reply_check: context create failed\n");
        return false;
    }
    (void)close(home_fd);
    (void)close(b_fd);
    int sndbuf = SNDBUF_BYTES;
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET/SO_SNDBUF: <sys/socket.h>, included above
    if (setsockopt(g_a.hal.data_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) != 0) {
        fprintf(stderr, "discovery_reply_check: SO_SNDBUF: %s\n", strerror(errno));
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s B_NETNS_PATH BROADCAST B_ADDRESS PORT\n", argv[0]);
        return 2;
    }
    _tt_CONFIG.broadcast = argv[2];
    _tt_CONFIG.port = (int)strtol(argv[4], NULL, 10);
    struct sockaddr_in sink = {.sin_family = AF_INET, .sin_port = htons(SINK_PORT)};
    if (inet_pton(AF_INET, argv[3], &sink.sin_addr) != 1) {
        fprintf(stderr, "discovery_reply_check: bad address %s\n", argv[3]);
        return 2;
    }
    if (!set_up(argv[1])) {
        return 2;
    }
    if (!warm_up()) {
        return 1;
    }
    if (!refill(&sink)) {
        printf("discovery_reply_check: VOID - A's send buffer never filled\n");
        return 3;
    }
    if (tt_Context_create_publisher(&g_b, &g_b_pub, &UInt64Topic, "discovery_reply_check") != tt_RET_OK) {
        fprintf(stderr, "discovery_reply_check: publisher create failed\n");
        return 2;
    }
    struct flood_result result = {0};
    flood(&sink, g_a.update_generation[B_ID], &result);
    bool b_held_at_flood_end = holds_list(&g_b, &g_a);
    uint64_t converged_after = b_held_at_flood_end ? 0 : settle();
    int verdict = judge(&result, b_held_at_flood_end, converged_after);
    tt_Context_destroy(&g_b);
    tt_Context_destroy(&g_a);
    if (verdict == 0) {
        printf("discovery_reply_check: PASS\n");
    }
    return verdict;
}

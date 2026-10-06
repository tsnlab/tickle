// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// No lost wake-up between a same-host writer and a reader asleep on its segment, against the real Linux HAL, run by
// test_samehost.sh. The mock HAL has no segment and no bell, so no unit test can see this.
//
// The race it exists for: a reader says it is about to sleep (reader_waiting), drains its ring once more, and sleeps;
// a writer publishes a record, then reads reader_waiting, and rings if it is set. Take away the second drain, or let
// either side do its two steps in the other order, and a record can land in the gap with nobody coming for it - the
// reader sleeps on top of it until something else wakes it.
//
// How it hits the race rather than hoping to: a ping-pong across two processes in which only the reader ever sleeps.
//   parent (the reader under test) publishes a ping from inside the callback that took the last pong, then goes back
//          to tt_Context_poll(-1), whose wait ends only on a ring, a datagram or a scheduler entry.
//   child  (the writer) busy-polls, so it sees each ping within a microsecond or two, waits a random delay drawn
//          uniformly from [0, the last ping-to-ping period], and publishes the pong into the parent's ring. The delay
//          sweeps the pong across the parent's whole path from publishing its ping to being asleep, so the few hundred
//          nanoseconds where a broken protocol loses the record are hit on a fixed fraction of round trips, on any
//          hardware - the range is measured from the run, not set.
//
// How it sees a lost wake-up without a clock: the child watches each pong until the parent consumes it, and reads the
// parent's segment header while it does (watch_pong()). A round-trip threshold cannot do this job - core's own
// scheduler wakes an idle context every tt_CONTEXT_CYCLE, so a lost wake-up costs about a millisecond, which the
// first version of this check, counting round trips over 10 ms, saw once in 100,000 against a protocol that lost one
// on every unlucky round trip.
//
// It also proves it exercised what it claims (each a FAIL if not, never a pass): the parent slept and was rung through
// the FIFO for most round trips, its records came through the segment, and the child watched them.
//
// Exit 0 pass, 1 fail, 3 setup failed. Usage: bell_wake_check [round_trips]
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "UInt64.h"

#define ROUND_TRIPS 100000U                            // measured round trips; past 65,536 rings, a full default pipe
#define WATCHDOG_NS (100ULL * 1000ULL * 1000ULL)       // re-sends the first ping, which discovery may drop
#define SETUP_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // to the first pong: discovery and segment attach
#define RUN_NS (30ULL * 1000ULL * 1000ULL * 1000ULL)   // the whole run, a backstop for a hang (healthy: ~3 s)
#define NS_PER_MS 1e6
#define NS_PER_S 1000000000ULL
#define REAP_TRIES 50 // the child is given REAP_TRIES x REAP_PAUSE_US to exit after the stop
#define REAP_PAUSE_US 100000U
#define STOP_VALUE UINT64_MAX // the ping that ends the child
#define PARENT_ID 61
#define CHILD_ID 62
#define BROADCAST "127.255.255.255" // loopback only, as test_samehost.sh

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_SETUP_FAILED = 3 };

// What the child rang, written by the child into memory shared across the fork for the parent's verdict: the check
// is about the FIFO bell, and a writer that found no bell to open rings over UDP instead, which would make every
// claim about the bell vacuous.
struct child_report {
    uint64_t doorbells_sent;
    uint64_t bells_rung;
    uint64_t watched; // pongs whose consumption the child watched (watch_pong())
    uint64_t lost;    // of those, a wake-up lost: see watch_pong()
    uint64_t stuck;   // of those, not consumed within RUN_NS at all
};
static struct child_report* g_report;

// ---- child: the writer, which never sleeps ----

static struct tt_Publisher g_child_pub;
static struct tt_Context* g_child_node;
static volatile bool g_child_stop = false;
static uint64_t g_last_ping_ns = 0;
static uint64_t g_period_ns = 0; // the last ping-to-ping period: the range the delay is drawn from

// Watches the pong just published until the parent consumes it, and decides from the parent's own segment header -
// not from a clock - whether a wake-up was lost on the way.
//
// The parent, healthy, consumes a record that was in its ring before it slept in the second drain, which moves
// read_index BEFORE it clears reader_waiting; a record that arrives after it published generation G is rung for G by
// this writer (struct tt_SegmentPeer.doorbell_generation). So reader_waiting going from G back to 0 with this record
// still unread and G never rung has exactly one cause: the parent slept on top of the record, and something other than
// the bell - its own scheduler, every tt_CONTEXT_CYCLE - woke it. That is a lost wake-up even when the stall it causes
// is a millisecond, short enough to pass for noise in a round-trip time. read_index is loaded after reader_waiting, and
// the parent stores them in the other order, so a 0 that follows a healthy drain is always seen with the drain's
// read_index.
static void watch_pong(void) {
    struct tt_SegmentHeader* header = g_child_node->segment_peers[PARENT_ID].mapping;
    if (header == NULL) {
        return; // not attached yet: this pong went over UDP, and there is no ring to watch
    }
    g_report->watched++;
    g_report->doorbells_sent = g_child_node->segment_doorbells_sent;
    g_report->bells_rung = g_child_node->segment_bells_rung;
    uint32_t written = __atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE); // this child is the only writer
    uint32_t seen_asleep = 0;
    uint64_t start = tt_get_ns();
    for (;;) {
        uint32_t waiting = __atomic_load_n(&header->reader_waiting, __ATOMIC_SEQ_CST);
        uint32_t read = __atomic_load_n(&header->read_index, __ATOMIC_ACQUIRE);
        if ((int32_t)(read - written) >= 0) {
            return; // consumed
        }
        if (waiting != 0) {
            seen_asleep = waiting;
        } else if (seen_asleep != 0 && seen_asleep != g_child_node->segment_peers[PARENT_ID].doorbell_generation) {
            g_report->lost++;
            printf("bell_wake_check: lost wake-up: generation %u never rung (last rung %u), read_index %u, record %u, "
                   "after %.3f ms\n",
                   seen_asleep, g_child_node->segment_peers[PARENT_ID].doorbell_generation, read, written,
                   (double)(tt_get_ns() - start) / NS_PER_MS);
            return;
        }
        if (tt_get_ns() - start > RUN_NS) {
            g_report->stuck++;
            return;
        }
    }
}

static void child_ping(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct UInt64Data* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    if (data->data == STOP_VALUE) {
        g_child_stop = true;
        return;
    }
    uint64_t now = tt_get_ns();
    if (g_last_ping_ns != 0) {
        g_period_ns = now - g_last_ping_ns;
    }
    g_last_ping_ns = now;
    uint64_t delay = g_period_ns > 0 ? (uint64_t)random() % g_period_ns : 0;
    while (tt_get_ns() - now < delay) {
    }
    struct UInt64Data pong = {.data = data->data};
    (void)tt_Publisher_publish(&g_child_pub, (struct tt_Data*)&pong);
    watch_pong();
}

static int child_main(void) {
    _tt_CONFIG.broadcast = BROADCAST;
    _tt_CONFIG.context_id = CHILD_ID;
    srandom((unsigned)getpid());
    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        return CHECK_SETUP_FAILED;
    }
    g_child_node = &node;
    struct tt_Subscriber sub;
    if (tt_Context_create_publisher(&node, &g_child_pub, &UInt64Topic, "bell_wake_pong") != tt_RET_OK ||
        tt_Context_create_subscriber(&node, &sub, &UInt64Topic, "bell_wake_ping", (tt_SUBSCRIBER_CALLBACK)child_ping) !=
            tt_RET_OK) {
        return CHECK_SETUP_FAILED;
    }
    uint64_t start = tt_get_ns();
    while (!g_child_stop && tt_get_ns() - start < RUN_NS) {
        (void)tt_Context_poll(&node, 0); // never waits: the child must see each ping at once
    }
    tt_Context_destroy(&node);
    return CHECK_PASS;
}

// ---- parent: the reader under test ----

static struct tt_Publisher g_parent_pub;
static uint64_t g_seq = 0;      // the ping in flight
static uint64_t g_sent_ns = 0;  // when it was published
static uint64_t g_measured = 0; // round trips completed since the first pong
static uint64_t g_worst_ns = 0; // the longest
static uint64_t g_first_pong_ns = 0;
static uint64_t g_last_rtt_ns = 0; // the range the parent's own delay is drawn from
static uint32_t g_target = ROUND_TRIPS;
static bool g_done = false;

static void parent_send(void) {
    g_seq++;
    g_sent_ns = tt_get_ns();
    struct UInt64Data ping = {.data = g_seq};
    (void)tt_Publisher_publish(&g_parent_pub, (struct tt_Data*)&ping);
}

static void parent_pong(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct UInt64Data* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    if (data->data != g_seq || g_done) {
        return; // a stale pong, from a ping the watchdog gave up on during setup
    }
    uint64_t now = tt_get_ns();
    if (g_first_pong_ns == 0) {
        g_first_pong_ns = now;
    } else {
        uint64_t rtt = now - g_sent_ns;
        g_last_rtt_ns = rtt;
        g_measured++;
        if (rtt > g_worst_ns) {
            g_worst_ns = rtt;
        }
    }
    if (g_measured >= g_target) {
        g_done = true;
        return;
    }
    parent_send(); // from inside the callback: the parent goes from here towards its sleep
    // And not at once: a delay drawn uniformly from [0, the last round trip] before it does. The child cannot answer
    // sooner than it takes to see the ping, and without this the parent was always past its window by then - the
    // first version of this check, delaying only the child, hit the race 3 times in 100,000 against a protocol broken
    // for every hit. With both sides delayed over the same measured range, the pong falls anywhere relative to the
    // parent's path to sleep.
    uint64_t delay = g_last_rtt_ns > 0 ? (uint64_t)random() % g_last_rtt_ns : 0;
    while (tt_get_ns() - now < delay) {
    }
}

// Before the first pong, re-sends the ping, which discovery may have dropped. After it, a ping is never re-sent - the
// pong is in the ring or on its way, and resending would muddle exactly what watch_pong() is watching.
static void parent_watchdog(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    if (g_first_pong_ns == 0 && time - g_sent_ns >= WATCHDOG_NS) {
        parent_send();
    }
    (void)tt_Context_schedule(node, time + WATCHDOG_NS, parent_watchdog, NULL);
}

static enum check_exit verdict(const struct tt_Context* node) {
    uint64_t sleeps = node->segment_sleep_generation;
    uint64_t rung = node->segment_doorbells_received;
    uint64_t via_shm = node->rx_datagrams_by_transport[tt_TRANSPORT_SHM];
    printf("bell_wake_check: %" PRIu64 " round trips (worst %.3f ms), %" PRIu64 " watched by the writer, %" PRIu64
           " wake-ups lost, %" PRIu64 " never consumed; parent slept %" PRIu64 " times, woken by the bell %" PRIu64
           " times; %" PRIu64 " records through the segment; the writer rang %" PRIu64 " times, %" PRIu64
           " through the FIFO\n",
           g_measured, (double)g_worst_ns / NS_PER_MS, g_report->watched, g_report->lost, g_report->stuck, sleeps, rung,
           via_shm, g_report->doorbells_sent, g_report->bells_rung);
    if (g_measured < g_target) {
        printf("bell_wake_check: FAIL - only %" PRIu64 " of %u round trips completed\n", g_measured, g_target);
        return CHECK_FAIL;
    }
    if (g_report->lost > 0 || g_report->stuck > 0) {
        printf("bell_wake_check: FAIL - %" PRIu64 " times the reader slept on a record nobody rang for, and %" PRIu64
               " records were never consumed\n",
               g_report->lost, g_report->stuck);
        return CHECK_FAIL;
    }
    // The run must have been the race it claims to be: a reader asleep, rung through the segment's bell, most times,
    // with the writer watching every pong it put in the ring.
    if (sleeps < g_target / 2 || via_shm < g_target || g_report->watched < g_target) {
        printf("bell_wake_check: FAIL - the run did not exercise the wake-up: %" PRIu64 " sleeps, %" PRIu64
               " segment records and %" PRIu64 " watched pongs for %u round trips\n",
               sleeps, via_shm, g_report->watched, g_target);
        return CHECK_FAIL;
    }
    // Every ring went through the FIFO: a writer that opened no bell rings over UDP, and then nothing here is about the
    // bell at all. doorbells_received counts the rings tt_receive() hands back as zero-length datagrams.
    if (g_report->bells_rung != g_report->doorbells_sent) {
        printf("bell_wake_check: FAIL - the writer rang %" PRIu64 " times and only %" PRIu64 " through the FIFO\n",
               g_report->doorbells_sent, g_report->bells_rung);
        return CHECK_FAIL;
    }
    if (rung < g_target / 2) {
        printf("bell_wake_check: FAIL - the bell woke the parent only %" PRIu64 " times in %u round trips\n", rung,
               g_target);
        return CHECK_FAIL;
    }
    printf("bell_wake_check: PASS\n");
    return CHECK_PASS;
}

static int parent_main(pid_t child) {
    _tt_CONFIG.broadcast = BROADCAST;
    _tt_CONFIG.context_id = PARENT_ID;
    srandom((unsigned)getpid());
    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        return CHECK_SETUP_FAILED;
    }
    struct tt_Subscriber sub;
    if (tt_Context_create_publisher(&node, &g_parent_pub, &UInt64Topic, "bell_wake_ping") != tt_RET_OK ||
        tt_Context_create_subscriber(&node, &sub, &UInt64Topic, "bell_wake_pong",
                                     (tt_SUBSCRIBER_CALLBACK)parent_pong) != tt_RET_OK) {
        return CHECK_SETUP_FAILED;
    }
    uint64_t start = tt_get_ns();
    g_sent_ns = start;
    (void)tt_Context_schedule(&node, start + WATCHDOG_NS, parent_watchdog, NULL);
    while (!g_done) {
        uint64_t now = tt_get_ns();
        if ((g_first_pong_ns == 0 && now - start > SETUP_NS) || now - start > RUN_NS) {
            break;
        }
        (void)tt_Context_poll(&node, -1);
    }
    // Stop the child: a few times, it is BEST_EFFORT, and it exits by itself at RUN_NS regardless.
    for (int i = 0; i < 3; i++) {
        struct UInt64Data stop = {.data = STOP_VALUE};
        (void)tt_Publisher_publish(&g_parent_pub, (struct tt_Data*)&stop);
        (void)tt_Context_poll(&node, 0);
    }
    // The child is reaped here and nowhere else, so its pid is still ours when it is signalled: a pid already reaped
    // could belong to anyone by now.
    int status = 0;
    pid_t reaped = 0;
    for (int tries = 0; tries < REAP_TRIES && reaped == 0; tries++) {
        reaped = waitpid(child, &status, WNOHANG);
        if (reaped == 0) {
            (void)usleep(REAP_PAUSE_US);
        }
    }
    bool child_clean = true;
    if (reaped == 0) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        printf("bell_wake_check: the child did not stop and was killed\n");
        child_clean = false;
    } else if (reaped != child || !WIFEXITED(status) || WEXITSTATUS(status) != CHECK_PASS) {
        printf("bell_wake_check: the child did not exit cleanly (status 0x%x)\n", status);
        child_clean = false;
    }
    // The verdict stands whichever way the child ended - it keeps its counts current in shared memory - and only a
    // pass is withheld when the child did not end cleanly.
    enum check_exit result = CHECK_SETUP_FAILED;
    if (g_first_pong_ns == 0) {
        printf("bell_wake_check: SETUP FAILED - no pong in %llu s\n", SETUP_NS / NS_PER_S);
    } else {
        result = verdict(&node);
        if (result == CHECK_PASS && !child_clean) {
            result = CHECK_SETUP_FAILED;
        }
    }
    tt_Context_destroy(&node);
    return result;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        g_target = (uint32_t)strtoul(argv[1], NULL, 10);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_report = mmap(NULL, sizeof *g_report, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_report == MAP_FAILED) {
        perror("mmap");
        return CHECK_SETUP_FAILED;
    }
    memset(g_report, 0, sizeof *g_report);
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return CHECK_SETUP_FAILED;
    }
    if (child == 0) {
        _exit(child_main());
    }
    return parent_main(child);
}

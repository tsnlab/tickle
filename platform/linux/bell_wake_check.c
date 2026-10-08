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
// reader sleeps on top of it until something else wakes it. The bell adds its own ways to lose one: a bell outside the
// wait set is a ring nobody hears, and an edge-triggered bell that is never read fills its pipe (hal_linux.c,
// bell_join_wait_set()).
//
// How it hits the race rather than hoping to: a ping-pong across two processes in which only the reader ever sleeps.
//   parent (the reader under test) publishes a ping from inside the callback that took the last pong, waits a fixed
//          PARENT_DELAY_NS, and goes back to tt_Context_poll(-1), whose wait ends only on a ring, a datagram or a
//          scheduler entry. The fixed delay puts its announcement (reader_waiting) at a steady time after the ping.
//   child  (the writer) busy-polls, so it sees each ping within a microsecond or two, and answers it one of two ways,
//          alternately (child_ping()):
//          late   - it waits until the parent has announced a sleep and then publishes, so the parent is asleep, or as
//                   good as, and is rung: the half of the run that proves the bell wakes a sleeping reader.
//          aimed  - it publishes at the announcement itself, after a delay that steps 20 ns earlier after each aimed
//                   pong it rang for and 20 ns later after each one it did not. So it settles where half are rung -
//                   where the writer's look at reader_waiting meets the reader's store to it - and a uniform jitter of
//                   +-1 us spreads the pongs around that point. Every broken order loses the record there.
//          Until 2026-10-08 the child drew its delay uniformly from [0, the last ping period], ~20 us. That caught the
//          fence mutant (tests/mutants_bell_wake.py) 1-6 times in 400,000 round trips up to 8c1e6431, and 0 times in
//          3.2 million from cd09e895 on, which encodes the sample straight into its slot between the claim and the
//          publish instead of copying it there just before the publish.
//   clog   a third thread of the child, on a CPU of its own, reads the writer's own stack lines while an aimed pong is
//          out (clog_main()). The race the writer's fence closes needs the store that publishes the record to be
//          still unwritten when the reader, having announced, reads the slot; the reader's path from its announcement
//          to that read is a few hundred nanoseconds of -O0 code. The reading that fits both measurements: once the
//          encode-in-slot write has pulled the slot's line in, the publishing store reaches the cache within a few
//          cycles, and the race all but closes. With the clog, every store the writer makes after its claim, stack
//          spills included, first has to take its line back, and x86 writes stores in order, so the publishing store
//          waits behind them while the writer's unfenced load runs ahead. Measured on the PC: aimed pongs without the
//          clog lost the fence mutant 0-5 wake-ups in 100,000 round trips; with it, 40 runs of 40 lost one within
//          283-20,789 round trips. Off on fewer than three CPUs, where it would only compete with the two contexts.
//
// How it sees a lost wake-up without a clock: the child watches each pong until the parent consumes it, and reads the
// parent's segment header while it does (watch_pong()). A round-trip threshold cannot do this job - core's own
// scheduler wakes an idle context every tt_CONTEXT_CYCLE, so a lost wake-up costs about a millisecond, which the
// first version of this check, counting round trips over 10 ms, saw once in 100,000 against a protocol that lost one
// on every unlucky round trip.
//
// ROUND_TRIPS is past the default pipe's 64 KiB, in rings, so an edge-triggered bell that is never read would be full
// by the end; the bytes left in it are checked against half its capacity. That is checked directly rather than
// through a lost wake-up because on this kernel a ring refused by a full pipe still wakes the reader: pipe_write()
// issues its wake-up after a failed non-blocking write as well. That is not a promise, so the bell is never allowed to
// fill, and this is the check that it is not.
//
// It also proves it exercised what it claims (each a FAIL if not, never a pass): the parent slept and was rung through
// the FIFO for most round trips, its records came through the segment, the child watched them, the aimed pongs
// straddled the announcement (between a quarter and three quarters rung), and - when the kernel let the bell be
// edge-triggered - the bell was read far fewer times than it was rung. The run stops at the first lost wake-up.
//
// Exit 0 pass, 1 fail, 3 setup failed. Usage: bell_wake_check [round_trips]
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming) - F_GETPIPE_SZ
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

#include "UInt64.h"

#define ROUND_TRIPS 100000U                            // measured round trips; past 65,536 rings, a full default pipe
#define WATCHDOG_NS (100ULL * 1000ULL * 1000ULL)       // re-sends the first ping, which discovery may drop
#define SETUP_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // to the first pong: discovery and segment attach
#define RUN_NS (30ULL * 1000ULL * 1000ULL * 1000ULL)   // the whole run, a backstop for a hang (healthy: ~3 s)
#define NS_PER_MS 1e6
#define NS_PER_US 1e3
#define NS_PER_S 1000000000ULL
#define REAP_TRIES 50 // the child is given REAP_TRIES x REAP_PAUSE_US to exit after the stop
#define REAP_PAUSE_US 100000U
#define STOP_VALUE UINT64_MAX    // the ping that ends the child
#define PARENT_DELAY_NS 10000ULL // from publishing a ping to going back to the poll: longer than the child's look
#define LATE_LIMIT_NS 1000000ULL // a late pong waits at most this long for the parent's announcement
#define AIM_STEP_NS 20           // the aim's step after each aimed pong
#define AIM_JITTER_NS 1000U      // and the uniform spread around it
#define CLOG_STACK_BYTES 2048U   // the writer's stack below child_ping()'s frame that the clog reads
#define CLOG_LINE 64U
#define CLOG_MIN_CPUS 3 // parent, child and clog each on a CPU of its own
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
    uint64_t watched;     // pongs whose consumption the child watched (watch_pong())
    uint64_t lost;        // of those, a wake-up lost: see watch_pong()
    uint64_t stuck;       // of those, not consumed within RUN_NS at all
    uint64_t aimed;       // pongs aimed at the parent's announcement (child_ping())
    uint64_t aimed_rung;  // of those, the ones the writer rang for: it saw the announcement
    int64_t aim_ns;       // where the aim settled, after the ping
    uint32_t parent_done; // set by the parent when it stops polling: nothing the child still watches will be consumed
};
static struct child_report* g_report;

// ---- child: the writer, which never sleeps ----

static struct tt_Publisher g_child_pub;
static struct tt_Context* g_child_node;
static volatile bool g_child_stop = false;
static int64_t g_aim_ns = -1; // from seeing a ping to publishing its pong; -1 until the first late pong measures it

// The CPUs the three are pinned to, or -1 for none (fewer than CLOG_MIN_CPUS allowed): the clog is then not started.
static int g_parent_cpu = -1;
static int g_child_cpu = -1;
static int g_clog_cpu = -1;
static const volatile uint8_t* volatile g_clog_lines = NULL; // what the clog reads; NULL while no aimed pong is out
static volatile bool g_clog_stop = false;
static volatile uint64_t g_clog_sink = 0; // what the clog read, kept so the reads are not optimized away

static void pin_to(int cpu) {
    if (cpu < 0) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)sched_setaffinity(0, sizeof set, &set); // this thread only; unpinned is slower to catch, never wrong
}

// The clog: reads CLOG_STACK_BYTES of the writer's stack, one load a line, for as long as an aimed pong is being
// published, so each of the writer's stores there has to take its line back first (the header says why). Only reads,
// and only the writer's own memory: nothing it does can change what the protocol does, only when its stores land.
static void* clog_main(void* arg) {
    (void)arg;
    pin_to(g_clog_cpu);
    uint64_t sink = 0;
    while (!g_clog_stop) {
        const volatile uint8_t* lines = __atomic_load_n(&g_clog_lines, __ATOMIC_ACQUIRE);
        if (lines == NULL) {
            (void)sched_yield();
            continue;
        }
        for (uint32_t offset = 0; offset < CLOG_STACK_BYTES; offset += CLOG_LINE) {
            sink += lines[offset];
        }
    }
    g_clog_sink = sink;
    return NULL;
}

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
        if (__atomic_load_n(&g_report->parent_done, __ATOMIC_ACQUIRE) != 0) {
            return; // the parent stopped - at a lost wake-up, or at the end - and will consume nothing more
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
    struct UInt64Data pong = {.data = data->data};
    struct tt_SegmentHeader* header = g_child_node->segment_peers[PARENT_ID].mapping;
    if (header != NULL && ((data->data & 1U) == 0 || g_aim_ns < 0)) {
        // Late: after the parent announces a sleep it has not announced before. The generation it shows now may be one
        // still standing from a pong its last drain took, inside whose callback this ping was published.
        const uint32_t standing = __atomic_load_n(&header->reader_waiting, __ATOMIC_SEQ_CST);
        uint64_t seen = now;
        for (;;) {
            uint32_t waiting = __atomic_load_n(&header->reader_waiting, __ATOMIC_SEQ_CST);
            if ((waiting != 0 && waiting != standing) || seen - now > LATE_LIMIT_NS ||
                __atomic_load_n(&g_report->parent_done, __ATOMIC_ACQUIRE) != 0) {
                break;
            }
            seen = tt_get_ns();
        }
        // The first guess at the aim, from the first late pong that saw an announcement in time; the steps below find
        // it from there. One that waited out LATE_LIMIT_NS saw none, and starting from there the aim took the whole run
        // to walk down to the announcement at AIM_STEP_NS a pong.
        if (g_aim_ns < 0 && seen - now <= 2U * PARENT_DELAY_NS) {
            g_aim_ns = (int64_t)(seen - now);
        }
    } else if (header != NULL) {
        // Aimed: at the announcement, with the clog on (the header).
        volatile uint8_t frame = 0;
        if (g_clog_cpu >= 0) {
            __atomic_store_n(&g_clog_lines, (const volatile uint8_t*)&frame - CLOG_STACK_BYTES, __ATOMIC_RELEASE);
        }
        int64_t delay = g_aim_ns + (int64_t)((uint64_t)random() % (2U * AIM_JITTER_NS + 1U)) - (int64_t)AIM_JITTER_NS;
        while ((int64_t)(tt_get_ns() - now) < delay) {
        }
        uint64_t rang_before = g_child_node->segment_doorbells_sent;
        (void)tt_Publisher_publish(&g_child_pub, (struct tt_Data*)&pong);
        bool rang = g_child_node->segment_doorbells_sent != rang_before;
        __atomic_store_n(&g_clog_lines, NULL, __ATOMIC_RELEASE);
        (void)frame;
        g_aim_ns += rang ? -AIM_STEP_NS : AIM_STEP_NS;
        if (g_aim_ns < 0) {
            g_aim_ns = 0;
        }
        g_report->aimed++;
        g_report->aimed_rung += rang ? 1U : 0U;
        g_report->aim_ns = g_aim_ns;
        watch_pong();
        return;
    }
    (void)tt_Publisher_publish(&g_child_pub, (struct tt_Data*)&pong);
    watch_pong();
}

static int child_main(void) {
    _tt_CONFIG.broadcast = BROADCAST;
    _tt_CONFIG.context_id = CHILD_ID;
    srandom((unsigned)getpid());
    pin_to(g_child_cpu);
    pthread_t clog; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    bool clogging = g_clog_cpu >= 0 && pthread_create(&clog, NULL, clog_main, NULL) == 0;
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
    g_clog_stop = true;
    if (clogging) {
        (void)pthread_join(clog, NULL);
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
    // And not at once: the child cannot answer sooner than it takes to see the ping, and without a delay the parent
    // was always past its window by then. Fixed, so that the announcement the child aims at comes at a steady time
    // after the ping (the header).
    while (tt_get_ns() - now < PARENT_DELAY_NS) {
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
    int unread = -1;
    int capacity = -1;
    if (node->hal.bell_fd_plus1 > 0) {
        (void)ioctl(node->hal.bell_fd_plus1 - 1, FIONREAD, &unread); // NOLINT(misc-include-cleaner)
        capacity = fcntl(node->hal.bell_fd_plus1 - 1, F_GETPIPE_SZ);
    }
    printf("bell_wake_check: %" PRIu64 " round trips (worst %.3f ms), %" PRIu64 " watched by the writer, %" PRIu64
           " wake-ups lost, %" PRIu64 " never consumed; parent slept %" PRIu64 " times, woken by the bell %" PRIu64
           " times, read it %" PRIu64 " times (every %u generations, %d of %d bytes left); %" PRIu64
           " records through the segment; the writer rang %" PRIu64 " times, %" PRIu64 " through the FIFO; %" PRIu64
           " pongs aimed at the announcement, %" PRIu64 " of them rung, aim %.3f us after the ping; clog %s\n",
           g_measured, (double)g_worst_ns / NS_PER_MS, g_report->watched, g_report->lost, g_report->stuck, sleeps, rung,
           node->hal.bell_drains, (unsigned)node->hal.bell_drain_every, unread, capacity, via_shm,
           g_report->doorbells_sent, g_report->bells_rung, g_report->aimed, g_report->aimed_rung,
           (double)g_report->aim_ns / NS_PER_US, g_clog_cpu >= 0 ? "on" : "off (fewer than 3 CPUs)");
    // A lost wake-up first: the run stops at the first one (parent_main()), so it is also why a run ends short.
    if (g_report->lost > 0 || g_report->stuck > 0) {
        printf("bell_wake_check: FAIL - %" PRIu64 " times the reader slept on a record nobody rang for, and %" PRIu64
               " records were never consumed\n",
               g_report->lost, g_report->stuck);
        return CHECK_FAIL;
    }
    if (g_measured < g_target) {
        printf("bell_wake_check: FAIL - only %" PRIu64 " of %u round trips completed\n", g_measured, g_target);
        return CHECK_FAIL;
    }
    // The aim must straddle the announcement: rung for about half. All rung means every aimed pong came after it - a
    // child that saw its pings too late to aim, or a parent whose delay no longer covers that - and none rung means
    // every one came before it. Either way the race was not where the pongs were.
    if (g_report->aimed < g_target / 4 || g_report->aimed_rung * 4U < g_report->aimed ||
        g_report->aimed_rung * 4U > g_report->aimed * 3U) {
        printf("bell_wake_check: FAIL - the aim did not straddle the announcement: %" PRIu64 " of %" PRIu64
               " aimed pongs rung\n",
               g_report->aimed_rung, g_report->aimed);
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
    if (node->hal.bell_drain_every > 0) {
        if (node->hal.bell_drains * 8U > rung) {
            printf("bell_wake_check: FAIL - an edge-triggered bell was read %" PRIu64 " times for %" PRIu64 " rings\n",
                   node->hal.bell_drains, rung);
            return CHECK_FAIL;
        }
        if (unread < 0 || capacity <= 0 || unread > capacity / 2) {
            printf("bell_wake_check: FAIL - the edge-triggered bell holds %d unread bytes of %d\n", unread, capacity);
            return CHECK_FAIL;
        }
    }
    printf("bell_wake_check: PASS (%s bell)\n", node->hal.bell_drain_every > 0 ? "edge-triggered" : "level-triggered");
    return CHECK_PASS;
}

static int parent_main(pid_t child) {
    _tt_CONFIG.broadcast = BROADCAST;
    _tt_CONFIG.context_id = PARENT_ID;
    srandom((unsigned)getpid());
    pin_to(g_parent_cpu);
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
        // One lost wake-up fails the run, and each costs the parent a watchdog period before anything wakes it.
        if (__atomic_load_n(&g_report->lost, __ATOMIC_RELAXED) > 0) {
            break;
        }
        (void)tt_Context_poll(&node, -1);
    }
    // Stop the child: a few times, it is BEST_EFFORT, and it exits by itself at RUN_NS regardless.
    __atomic_store_n(&g_report->parent_done, 1U, __ATOMIC_RELEASE);
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
    // Three CPUs of those this process may use, in order: one each for the parent, the child and the clog.
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed) == 0 && CPU_COUNT(&allowed) >= CLOG_MIN_CPUS) {
        int* const slots[CLOG_MIN_CPUS] = {&g_parent_cpu, &g_child_cpu, &g_clog_cpu};
        int taken = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE && taken < CLOG_MIN_CPUS; cpu++) {
            if (CPU_ISSET(cpu, &allowed)) {
                *slots[taken++] = cpu;
            }
        }
    }
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

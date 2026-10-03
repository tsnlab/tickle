/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// CPU, peak memory and wire-byte counters for the HIL comparison, in one header shared by all
// three harnesses (OPTIMIZATION_PLAN.md section 8) - the same arrangement CpuPlace.h already
// uses, included by relative path from the CycloneDDS and FastDDS trees and compiled as both C
// and C++.
//
// **Why shared rather than per harness.** Instrumenting TickLE alone, or more accurately, turns a
// comparison into a claim about the instrument. The precedent is CPU pinning (9f70d9b4): applied
// to one harness it would have handed TickLE about 15% a quarter of the time. Nothing here is
// framework-specific, so there is no reason for three versions of it to exist and drift.
//
// Everything is read from the kernel, not from the middleware's own statistics, for the same
// reason: /proc/net/dev counts what actually left the interface, including retransmits,
// heartbeats, ACKNACKs and discovery, while a middleware's own counter reports what that
// middleware thinks it sent.
//
// **A zero is an instrument failure, not a measurement.** Idle eth0 on the rig was measured (2026-
// 09-25) to be byte- and packet-identical over 20 s: the link is not merely quiet, it is silent.
// So a zero delta during a run cannot be a quiet link, and a zero CPU counter cannot be a free
// process. Those cases set `instrument=fail:...` rather than reporting the zero. The flag is a
// separate field from the numbers, so a parser never has to decide whether a zero is a number.

#pragma once

#include <dirent.h> // NOLINT(misc-include-cleaner) - DIR, opendir/readdir/closedir for /proc/self/task
#include <stdio.h>
#include <string.h>

#include <sys/resource.h> // NOLINT(misc-include-cleaner) - declares struct rusage via bits/types/

#ifdef __cplusplus
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#else
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#endif

// Which direction's packet count is the meaningful one for this process. A publisher's own
// transmitted packets are what the payload-boundary gate reads (section 9); a subscriber's
// received ones are its counterpart.
// Wide enough for every field below with room to spare; snprintf truncates rather than
// overflowing if that ever stops being true. Raised from 1024 on 2026-09-28 for sched_by_thread=,
// whose length grows with the process's own thread count (BENCH_STATS_THREADS_MAX of them, each a
// name of up to 15 characters and a time).
#define BENCH_STATS_FIELDS_MAX 2048

// A TickLE or DDS harness process runs a handful of threads - the application's own, the
// middleware's receive and event threads. 16 leaves room for a framework that spawns more, and any
// beyond that are counted in sched_threads_over= rather than silently dropped.
#define BENCH_STATS_THREADS_MAX 16

#define BENCH_ROLE_SENDER 0
#define BENCH_ROLE_RECEIVER 1

struct BenchStatsCounters {
    uint64_t rx_bytes;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t tx_packets;
    int valid;
};

// One thread's own CPU time. `name` is /proc/<tid>/comm, which is TASK_COMM_LEN (16) including its
// terminator - so it is the thread name a debugger and `top -H` show, and matching on it is how a
// reader tells the application's thread from the middleware's.
struct BenchStatsThread {
    uint64_t tid;
    uint64_t cpu_ns;
    char name[16];
};

struct BenchStatsThreads {
    struct BenchStatsThread thread[BENCH_STATS_THREADS_MAX];
    unsigned int count;
    unsigned int over; // threads this process has beyond BENCH_STATS_THREADS_MAX
    int valid;
};

// The per-transport datagram counts a TickLE harness copies out of its own tt_Context (the send/receive seam maintains
// them - SHM_PLAN.md's S2 contract). Kept as plain integers rather than as the enum-indexed array itself, so this
// header stays free of tickle.h and the two DDS harnesses that share it need no TickLE type. Two entries, UDP then SHM:
// a third transport means editing this header, which is the price of that independence and is stated here so it is not
// a surprise.
#define BENCH_TRANSPORT_MAX 2

struct BenchStats {
    struct BenchStatsCounters net_begin;
    struct BenchStatsCounters net_end;
    struct BenchStatsThreads threads_begin;
    struct BenchStatsThreads threads_end;
    double cpu_begin_s;
    uint64_t tx_by_transport[BENCH_TRANSPORT_MAX];
    uint64_t rx_by_transport[BENCH_TRANSPORT_MAX];
    uint64_t udp_broadcast;
    uint64_t udp_oversize;
    uint64_t udp_unattached;
    uint64_t shm_full_dropped;
    int transport_valid;      // 0 until a harness calls bench_stats_set_transport(): the DDS harnesses never do
    int fallbacks_valid;      // likewise for bench_stats_set_fallbacks()
    uint64_t attach_attempts; // every tt_segment_attach() the context made, by any outcome
    uint64_t attach_ok;
    uint64_t attach_absent;
    int attach_valid; // likewise for bench_stats_set_attach()
    // Three core counters that were collected and had nowhere to go. This repository's own
    // principle - an instrument that is collected but not on the RESULT line is not an instrument -
    // was being broken by the very counters added to make shared-memory faults visible, which is
    // the 2026-10-03 defect one layer up: a field written and never read looks exactly like a
    // working system that reports nothing.
    uint64_t span_absorbed;      // seq positions a whole record's span covered beyond its own
    uint64_t head_stalls;        // head claimed but not yet published - a race, not a stall (see below)
    uint64_t stall_warnings;     // the head stayed claimed for tt_SEGMENT_STALL_PASSES: a REAL stall
    uint64_t shm_only_on_socket; // a segment-only record arrived from the network
    int shm_diag_valid;          // likewise for bench_stats_set_shm_diagnostics()
    char iface[32];
};

// The interface the bench traffic actually uses. eth0 on the rig is the direct 192.168.10.x pair;
// the control plane (SSH, orchestration) is on wlan0 and so is excluded by construction. Settable
// for a host wired differently, because a wrong interface reads as a zero delta, which this
// header reports as a failure rather than as a result.
static inline const char* bench_stats_iface(void) {
    const char* env = getenv("BENCH_IFACE");
    return (env != NULL && env[0] != '\0') ? env : "eth0";
}

// Note for anyone reading cpu_s_per_Msample on a SHORT row: getrusage(RUSAGE_SELF) is cumulative for the whole process,
// and this file has always divided the cumulative total by the samples of the run. Over a 20 s throughput run the
// startup cost is negligible; over a 100-sample latency row it is most of it, so those rows' CPU-per-sample figures are
// dominated by node creation and discovery rather than by the per-sample path. That is a property of every published
// row on every framework, so the comparison between frameworks stands - but it is not a per-sample cost, and
// sched_cpu_s_per_Msample (a begin-to-end delta) is the figure that is.
static inline double bench_stats_cpu_seconds(void) {
    struct rusage usage; // NOLINT(misc-include-cleaner)
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1.0;
    }
    return (double)usage.ru_utime.tv_sec + ((double)usage.ru_utime.tv_usec / 1e6) + (double)usage.ru_stime.tv_sec +
           ((double)usage.ru_stime.tv_usec / 1e6);
}

// VmHWM is the kernel's own high-water mark for resident set size, so it does not depend on this
// process sampling itself at the right moment - which a peak read from VmRSS at exit would.
static inline uint64_t bench_stats_peak_rss_kb(void) {
    FILE* file = fopen("/proc/self/status", "r");
    char line[256];
    uint64_t hwm_kb = 0;
    if (file == NULL) {
        return 0;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            if (sscanf(line + 6, "%" SCNu64, &hwm_kb) != 1) {
                hwm_kb = 0;
            }
            break;
        }
    }
    fclose(file);
    return hwm_kb;
}

static inline void bench_stats_read_net(const char* iface, struct BenchStatsCounters* out) {
    FILE* file = fopen("/proc/net/dev", "r");
    char line[512];
    char want[40];
    size_t want_len;

    memset(out, 0, sizeof(*out));
    if (file == NULL) {
        return;
    }
    snprintf(want, sizeof(want), "%s:", iface);
    want_len = strlen(want);

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char* p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, want, want_len) != 0) {
            continue;
        }
        p += want_len;
        // Receive: bytes packets errs drop fifo frame compressed multicast
        // Transmit: bytes packets errs drop fifo colls carrier compressed
        if (sscanf(p, "%" SCNu64 " %" SCNu64 " %*u %*u %*u %*u %*u %*u %" SCNu64 " %" SCNu64, &out->rx_bytes,
                   &out->rx_packets, &out->tx_bytes, &out->tx_packets) == 4) {
            out->valid = 1;
        }
        break;
    }
    fclose(file);
}

// Per-thread CPU time at nanosecond resolution. Field 1 of /proc/<tid>/schedstat is that task's own
// se.sum_exec_runtime in ns, which the kernel maintains unconditionally; field 2, run-queue wait, is
// the one that needs CONFIG_SCHEDSTATS' sysctl and reads 0 without it (checked on the rig
// 2026-09-28: /proc/sys/kernel/sched_schedstats is 0 there, field 1 still counts, so nothing here
// reads field 2 and no zero can be mistaken for "this thread never waited").
//
// **Why this exists beside getrusage.** ru_utime/ru_stime come from tick-based accounting, so a
// 20 s run's ~2 s of CPU is quantised at the tick - and the p1 client question WIRE_PLAN section 10
// left open is a 0.5-0.7% difference, which is inside that quantisation. An instrument that cannot
// resolve an effect cannot attribute it either. sum_exec_runtime is in ns, and it is per thread,
// which is what turns "the process got slower" into "this thread got slower" - the next thing to
// know once the syscall counts have come back identical.
//
// A thread that appears only in the end snapshot has its whole lifetime counted (its begin is 0),
// which is the truth for a thread started inside the run. A thread that exits before the end
// snapshot is lost entirely, and sched_threads_gone= says how many, so a total that does not add up
// has a stated reason rather than a quiet one.
static inline void bench_stats_read_threads(struct BenchStatsThreads* out) {
    DIR* dir = opendir("/proc/self/task"); // NOLINT(misc-include-cleaner)
    const struct dirent* entry = NULL;     // NOLINT(misc-include-cleaner)

    memset(out, 0, sizeof(*out));
    if (dir == NULL) {
        return;
    }
    while ((entry = readdir(dir)) != NULL) { // NOLINT(misc-include-cleaner)
        char path[64];
        char line[128];
        FILE* file = NULL;
        unsigned long long tid = 0;
        unsigned long long cpu_ns = 0;
        struct BenchStatsThread* slot = NULL;

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue; // "." and ".."
        }
        tid = strtoull(entry->d_name, NULL, 10);
        if (out->count == BENCH_STATS_THREADS_MAX) {
            out->over++;
            continue;
        }
        snprintf(path, sizeof(path), "/proc/self/task/%llu/schedstat", tid);
        file = fopen(path, "r");
        if (file == NULL) {
            continue; // the thread exited between readdir and here - not a failure of the instrument
        }
        if (fgets(line, (int)sizeof(line), file) == NULL || sscanf(line, "%llu", &cpu_ns) != 1) {
            fclose(file);
            continue;
        }
        fclose(file);

        slot = &out->thread[out->count];
        slot->tid = (uint64_t)tid;
        slot->cpu_ns = (uint64_t)cpu_ns;
        snprintf(path, sizeof(path), "/proc/self/task/%llu/comm", tid);
        file = fopen(path, "r");
        if (file != NULL) {
            if (fgets(slot->name, (int)sizeof(slot->name), file) != NULL) {
                char* newline = strchr(slot->name, '\n');
                if (newline != NULL) {
                    *newline = '\0';
                }
            }
            fclose(file);
        }
        if (slot->name[0] == '\0') {
            snprintf(slot->name, sizeof(slot->name), "tid%llu", tid);
        }
        out->count++;
        out->valid = 1;
    }
    closedir(dir); // NOLINT(misc-include-cleaner)
}

// This thread's CPU time in the begin snapshot, or 0 when it was not running then.
static inline uint64_t bench_stats_thread_begin_ns(const struct BenchStatsThreads* begin, uint64_t tid) {
    for (unsigned int i = 0; i < begin->count; i++) {
        if (begin->thread[i].tid == tid) {
            return begin->thread[i].cpu_ns;
        }
    }
    return 0;
}

// Threads in the begin snapshot that are gone from the end one, whose CPU time no delta can reach.
static inline unsigned int bench_stats_threads_gone(const struct BenchStatsThreads* begin,
                                                    const struct BenchStatsThreads* end) {
    unsigned int gone = 0;
    for (unsigned int i = 0; i < begin->count; i++) {
        unsigned int found = 0;
        for (unsigned int j = 0; j < end->count; j++) {
            if (end->thread[j].tid == begin->thread[i].tid) {
                found = 1;
                break;
            }
        }
        gone += (found == 0) ? 1 : 0;
    }
    return gone;
}

static inline void bench_stats_begin(struct BenchStats* stats) {
    memset(stats, 0, sizeof(*stats));
    snprintf(stats->iface, sizeof(stats->iface), "%s", bench_stats_iface());
    stats->cpu_begin_s = bench_stats_cpu_seconds();
    bench_stats_read_net(stats->iface, &stats->net_begin);
    bench_stats_read_threads(&stats->threads_begin);
}

// Called by a TickLE harness with its context's own arrays, before bench_stats_fields(). Absent for the DDS harnesses,
// and the fields are then left OUT of the RESULT line rather than printed as zeros - a zero would read as "no datagrams
// went anywhere", which is exactly the confusion the counters exist to prevent.
static inline void bench_stats_set_transport(struct BenchStats* stats, const uint64_t* sent, const uint64_t* received,
                                             size_t count) {
    size_t kept = (count < (size_t)BENCH_TRANSPORT_MAX) ? count : (size_t)BENCH_TRANSPORT_MAX;
    for (size_t i = 0; i < kept; i++) {
        stats->tx_by_transport[i] = sent[i];
        stats->rx_by_transport[i] = received[i];
    }
    stats->transport_valid = 1;
}

// The four reasons a datagram went by UDP while the shared-memory module was built, for SHM_PLAN.md's S2 assertion:
// tx_udp must equal their sum, so every UDP datagram has a named reason and an *unexplained* fallback fails the test.
// tx_udp == 0 is unachievable - a segment carries unicast to a known peer, and a broadcast has no peer whose name could
// be computed - which is why the assertion is about attribution rather than about zero.
//
// The RESULT-line names and the core field names differ on purpose. Here they are tx_udp_* because a reader checking
// the assertion should see four fields that obviously sum to tx_udp; in struct tt_Context they are segment_*_to_udp
// because there the question is what the segment did. The mapping is this one call site, and these are its two halves:
//   tx_udp_broadcast  <- segment_broadcast_to_udp     tx_udp_oversize    <- segment_oversized_to_udp
//   tx_udp_unattached <- segment_unattached_to_udp    shm_full_dropped   <- segment_full_dropped
//
// Three reasons sum to tx_udp, not four. shm_full_dropped is deliberately NOT one of them: since 2026-09-29 a full
// ring drops the datagram instead of rerouting it, because a rerouted datagram overtakes the records still in the
// ring and makes the reader discard every one of them. It is still printed, and it is still the sizing signal it
// always was - it is simply no longer a fallback, so adding it into tx_udp would make the invariant false.
static inline void bench_stats_set_fallbacks(struct BenchStats* stats, uint64_t broadcast, uint64_t oversize,
                                             uint64_t unattached, uint64_t full_dropped) {
    stats->udp_broadcast = broadcast;
    stats->udp_oversize = oversize;
    stats->udp_unattached = unattached;
    stats->shm_full_dropped = full_dropped;
    stats->fallbacks_valid = 1;
}

// How many times the context asked /dev/shm about a peer, and how those attempts came out. On the RESULT line because
// of what happened without it: from the seam landing until 2026-09-29 a peer with no segment was re-asked for *every*
// datagram - a failed open() about 87,000 times a second - and it cost 54% of cross-host throughput for a day while
// every functional test stayed green. Nothing on the row could have shown it. With these fields it is one comparison on
// the face of every row: shm_attach_absent should be a handful of attempts, not one per datagram, so
// shm_attach_absent next to tx_udp_unattached is the check. The fallback counters were added so a UDP datagram can
// never be uncounted; this is the same argument applied to an attach that is retried.
static inline void bench_stats_set_attach(struct BenchStats* stats, const uint32_t* attach, size_t count,
                                          size_t ok_index, size_t absent_index) {
    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += attach[i];
    }
    stats->attach_attempts = total;
    stats->attach_ok = ok_index < count ? attach[ok_index] : 0;
    stats->attach_absent = absent_index < count ? attach[absent_index] : 0;
    stats->attach_valid = 1;
}

// The shared-memory path's three diagnostics, which say WHY a cell delivered what it delivered
// when the delivery counts alone cannot. Each is zero in the healthy case, so unlike the transport
// counters a zero here is a reading rather than an instrument failure - but they are still printed
// only when a harness supplies them, because a DDS harness has no such path and three zeros would
// claim it was measured.
//
//   span_absorbed       zero while shared-memory records flow means the span is not reaching the
//                       reader. That exact state delivered 1 sample out of 15,401 received records
//                       on 2026-10-03 with every loss counter also reading zero.
//   head_stalls         drain passes that found the ring NOT empty and the head still unreadable.
//                       It does not count empty polls - note_head_stall() compares write_index
//                       against read_index and returns without counting when they are equal. What
//                       it does count is mostly benign: a writer that has claimed the head and not
//                       yet published it looks identical to one that died holding it, and at
//                       600,000 samples a second that race is constant. The first run to carry it
//                       measured 44,627-48,363 on an arm delivering every sample with zero loss,
//                       and 70,153-73,196 on another arm also delivering every sample - so a large
//                       value is normal and the number is a rate, not a verdict.
//   stall_warnings      the verdict. tt_Context.segment_stall_warnings rises when the head stays
//                       claimed for tt_SEGMENT_STALL_PASSES drain passes in a row, which is a
//                       writer that died between its claim and its publish - the ring stops for
//                       good and traffic silently falls back to UDP. Zero means no persistent
//                       stall was ever seen; anything else is one, and it is the clean pass/fail
//                       that head_stalls is not. Head-of-line stalling is invisible in throughput
//                       alone - a stalled reader and a slow writer produce the same rate - so this
//                       is the instrument for it.
//   shm_only_on_socket  a record that only something with write access to a segment could have
//                       built, arriving from the network instead.
static inline void bench_stats_set_shm_diagnostics(struct BenchStats* stats, uint64_t span_absorbed,
                                                   uint64_t head_stalls, uint64_t stall_warnings,
                                                   uint64_t shm_only_on_socket) {
    stats->span_absorbed = span_absorbed;
    stats->head_stalls = head_stalls;
    stats->stall_warnings = stall_warnings;
    stats->shm_only_on_socket = shm_only_on_socket;
    stats->shm_diag_valid = 1;
}

static inline void bench_stats_end(struct BenchStats* stats) {
    bench_stats_read_net(stats->iface, &stats->net_end);
    bench_stats_read_threads(&stats->threads_end);
}

static inline uint64_t bench_stats_delta(uint64_t begin, uint64_t end) {
    // The kernel's counters are 64-bit on every platform this runs on, so a wrap would mean the
    // reads were transposed rather than that the counter rolled over. Reporting 0 hands that to
    // the instrument gate instead of to arithmetic.
    return (end >= begin) ? (end - begin) : 0;
}

// Appends this process's own instrumentation to a RESULT line. `samples` is what this side
// actually delivered (sent for a publisher, received for a subscriber) and `sample_bytes` is the
// CDR size of one sample, which is carried in the line so the payload-boundary gate is checkable
// from the line alone rather than from which directory produced it.
// How TickLE's own core library was optimised, when the build says (examples/perf_hil/tickle/build.sh,
// TICKLE_CORE_BUILD). Printed as core_build= so a reader can refuse a comparison across optimisation
// levels rather than discover it: until 2026-09-26 the TickLE core was built -O0 on the rig without
// anything saying so. The DDS harnesses share this file and define nothing - their libraries are the
// vendors' release packages - so the field is simply absent from their lines.
// build.sh passes the build type as a bare token (release or debug) - a quoted string would need shell
// quoting inside a word-split variable - so it is turned into a string here.
//
// sample_path= says how a sample too large for one datagram was carried (build.sh, TICKLE_P4_PATH):
// frag (TickLE's own fragments), ipfrag (one datagram the OS split) or datagram (no sample needed
// either). Printed for the same reason: p4 rows from two paths must not be read as one series.
#ifdef BENCH_CORE_BUILD
#define BENCH_STRINGIFY_(x) #x
#define BENCH_STRINGIFY(x) BENCH_STRINGIFY_(x)
#define BENCH_CORE_BUILD_FIELD " core_build="
// datagram_bytes= is tt_MAX_BUFFER_LENGTH as this build compiled it (build.sh's fit check fails the
// build if the two disagree), so a row from the TICKLE_DATAGRAM_BYTES diagnostic cannot pass for p4.
#ifndef BENCH_DATAGRAM_BYTES
#error \
    "build.sh defines BENCH_DATAGRAM_BYTES alongside BENCH_CORE_BUILD; a TickLE harness built without it would print no datagram_bytes="
#endif
#define BENCH_CORE_BUILD_VALUE        \
    BENCH_STRINGIFY(BENCH_CORE_BUILD) \
    " sample_path=" BENCH_STRINGIFY(BENCH_SAMPLE_PATH) " datagram_bytes=" BENCH_STRINGIFY(BENCH_DATAGRAM_BYTES)
#else
#define BENCH_CORE_BUILD_FIELD ""
#define BENCH_CORE_BUILD_VALUE ""
#endif

// Whether sched_by_thread= can be read as where the time went. A thread born and reaped inside the window appears in
// neither snapshot, so its CPU belongs to no named thread - which is a caveat on the BREAKDOWN and not a failure of any
// counter. It therefore gets its own field rather than joining instrument=fail:, because every consumer of this line
// voids a row on instrument=fail: and a DDS vendor's short-lived discovery threads would void rows whose latency, CPU
// and wire figures are all perfectly good (2026-09-29: it did, on every latency row of the first campaign to carry
// this instrument).
static inline const char* sched_breakdown_state(double window_cpu_s, double unattributed_s) {
    if (window_cpu_s <= 0.0) {
        return "unknown";
    }
    return (unattributed_s > window_cpu_s / 10.0) ? "partial" : "complete";
}

// What bench_stats_fields() has computed by the time the instrument flags are decided, gathered so
// the check reads from one place.
struct BenchStatsTotals {
    uint64_t wire_bytes;
    uint64_t wire_packets;
    uint64_t peak_rss_kb;
    uint64_t samples;
    uint64_t sched_cpu_ns;
    double cpu_s;
    double sched_unattributed_s;
};

// The instrument= field's contents: one name per counter that did not work, empty when all did.
// Each check names the counter rather than setting a single "bad" flag, because they fail for
// unrelated reasons - a wrong interface name, a kernel without VmHWM, a run that delivered nothing,
// an unreadable /proc/self/task. Its own function so bench_stats_fields() stays inside the project's
// cognitive-complexity limit.
static inline void bench_stats_fail_flags(const struct BenchStats* stats, const struct BenchStatsTotals* totals,
                                          char* fail, size_t fail_len) {
    fail[0] = '\0';
    if (!stats->net_begin.valid || !stats->net_end.valid || totals->wire_bytes == 0 || totals->wire_packets == 0) {
        snprintf(fail + strlen(fail), fail_len - strlen(fail), "%snet", fail[0] != '\0' ? "," : "");
    }
    if (totals->cpu_s <= 0.0) {
        snprintf(fail + strlen(fail), fail_len - strlen(fail), "%scpu", fail[0] != '\0' ? "," : "");
    }
    if (totals->peak_rss_kb == 0) {
        snprintf(fail + strlen(fail), fail_len - strlen(fail), "%srss", fail[0] != '\0' ? "," : "");
    }
    if (totals->samples == 0) {
        // Not a counter failure: the run genuinely delivered nothing. Every per-sample figure is then
        // 0 by construction rather than measured, and saying so is the honest report.
        snprintf(fail + strlen(fail), fail_len - strlen(fail), "%ssamples", fail[0] != '\0' ? "," : "");
    }
    if (!stats->threads_begin.valid || !stats->threads_end.valid || totals->sched_cpu_ns == 0) {
        // A process cannot run for a measured interval on zero nanoseconds of any thread, so this is
        // /proc/self/task being unreadable rather than a free run - the same rule as above.
        snprintf(fail + strlen(fail), fail_len - strlen(fail), "%ssched", fail[0] != '\0' ? "," : "");
    }
}

// The CPU each thread alive at the end spent over the run, as a `name:seconds` list in `out`,
// returning the total. Sorted by that time so the most expensive thread is first and the field reads
// the same way from run to run, which readdir's order does not. Its own function rather than part of
// bench_stats_fields() so that function stays inside the project's cognitive-complexity limit.
static inline uint64_t bench_stats_sched_by_thread(const struct BenchStats* stats, char* out, size_t out_len) {
    uint64_t thread_ns[BENCH_STATS_THREADS_MAX];
    unsigned int order[BENCH_STATS_THREADS_MAX];
    uint64_t total_ns = 0;
    size_t len = 0;

    out[0] = '\0';
    for (unsigned int i = 0; i < stats->threads_end.count; i++) {
        const struct BenchStatsThread* thread = &stats->threads_end.thread[i];
        thread_ns[i] =
            bench_stats_delta(bench_stats_thread_begin_ns(&stats->threads_begin, thread->tid), thread->cpu_ns);
        total_ns += thread_ns[i];
        order[i] = i;
    }
    // Selection sort: at most BENCH_STATS_THREADS_MAX entries, and no allocation.
    for (unsigned int i = 0; i + 1 < stats->threads_end.count; i++) {
        for (unsigned int j = i + 1; j < stats->threads_end.count; j++) {
            if (thread_ns[order[j]] > thread_ns[order[i]]) {
                unsigned int swap = order[i];
                order[i] = order[j];
                order[j] = swap;
            }
        }
    }
    for (unsigned int i = 0; i < stats->threads_end.count; i++) {
        int written = snprintf(out + len, out_len - len, "%s%s:%.6f", len > 0 ? "," : "",
                               stats->threads_end.thread[order[i]].name, (double)thread_ns[order[i]] / 1e9);
        if (written <= 0 || (size_t)written >= out_len - len) {
            break; // truncated: the scalar fields still hold, and this one is the readable extra
        }
        len += (size_t)written;
    }
    return total_ns;
}

static inline const char* bench_stats_fields(struct BenchStats* stats, int role, uint64_t samples,
                                             uint64_t sample_bytes, char* buf, size_t buf_len) {
    struct rusage usage; // NOLINT(misc-include-cleaner)
    double utime_s = 0.0;
    double stime_s = 0.0;
    double cpu_s = 0.0;
    uint64_t rx_bytes = bench_stats_delta(stats->net_begin.rx_bytes, stats->net_end.rx_bytes);
    uint64_t rx_packets = bench_stats_delta(stats->net_begin.rx_packets, stats->net_end.rx_packets);
    uint64_t tx_bytes = bench_stats_delta(stats->net_begin.tx_bytes, stats->net_end.tx_bytes);
    uint64_t tx_packets = bench_stats_delta(stats->net_begin.tx_packets, stats->net_end.tx_packets);
    uint64_t wire_bytes_total = rx_bytes + tx_bytes;
    uint64_t wire_packets_total = rx_packets + tx_packets;
    uint64_t role_packets = (role == BENCH_ROLE_SENDER) ? tx_packets : rx_packets;
    uint64_t peak_rss_kb = bench_stats_peak_rss_kb();
    double megabytes = (double)samples * (double)sample_bytes / 1e6;
    uint64_t sched_cpu_ns = 0;
    double sched_unattributed_s = 0.0;
    unsigned int sched_gone = bench_stats_threads_gone(&stats->threads_begin, &stats->threads_end);
    char by_thread[512];
    char fail[80];
    char transport[96];
    char shmdiag[224];
    char fallbacks[320];

    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        utime_s = (double)usage.ru_utime.tv_sec + ((double)usage.ru_utime.tv_usec / 1e6);
        stime_s = (double)usage.ru_stime.tv_sec + ((double)usage.ru_stime.tv_usec / 1e6);
        cpu_s = utime_s + stime_s;
    }

    sched_cpu_ns = bench_stats_sched_by_thread(stats, by_thread, sizeof(by_thread));
    // SHM_PLAN's S2 assertion reads these: for a same-host pair with the segment in use, tx_udp must be 0 for the shape
    // under test. Printed only when a harness supplied them, per bench_stats_set_transport()'s own comment.
    fallbacks[0] = '\0';
    if (stats->fallbacks_valid != 0) {
        snprintf(fallbacks, sizeof(fallbacks),
                 " tx_udp_broadcast=%" PRIu64 " tx_udp_oversize=%" PRIu64 " tx_udp_unattached=%" PRIu64
                 " shm_full_dropped=%" PRIu64,
                 stats->udp_broadcast, stats->udp_oversize, stats->udp_unattached, stats->shm_full_dropped);
    }
    if (stats->attach_valid != 0) {
        size_t used = strlen(fallbacks);
        snprintf(fallbacks + used, sizeof(fallbacks) - used,
                 " shm_attach_attempts=%" PRIu64 " shm_attach_ok=%" PRIu64 " shm_attach_absent=%" PRIu64,
                 stats->attach_attempts, stats->attach_ok, stats->attach_absent);
    }
    transport[0] = '\0';
    if (stats->transport_valid != 0) {
        snprintf(transport, sizeof(transport),
                 " tx_udp=%" PRIu64 " tx_shm=%" PRIu64 " rx_udp=%" PRIu64 " rx_shm=%" PRIu64, stats->tx_by_transport[0],
                 stats->tx_by_transport[1], stats->rx_by_transport[0], stats->rx_by_transport[1]);
    }
    shmdiag[0] = '\0';
    if (stats->shm_diag_valid != 0) {
        snprintf(shmdiag, sizeof(shmdiag),
                 " rx_span_absorbed=%" PRIu64 " segment_head_stalls=%" PRIu64 " segment_stall_warnings=%" PRIu64
                 " rx_shm_only_on_socket=%" PRIu64,
                 stats->span_absorbed, stats->head_stalls, stats->stall_warnings, stats->shm_only_on_socket);
    }
    // Against the WINDOW's getrusage delta, not against cpu_s. getrusage(RUSAGE_SELF) is cumulative for the whole
    // process, while sched_cpu_ns is a begin-to-end delta, so subtracting one from the other counts every cycle spent
    // before bench_stats_begin() as "unattributed". Over a 20 s throughput run that startup is negligible and the
    // mistake was invisible; on a 100-sample latency run it is most of the process's CPU, and the first campaign to
    // carry this instrument voided every latency row on both DDS vendors and on TickLE (2026-09-29, caught 40 seconds
    // into the run). cpu_begin_s has been captured since this header existed; it just was not used here.
    sched_unattributed_s =
        (stats->cpu_begin_s >= 0.0) ? (cpu_s - stats->cpu_begin_s) - ((double)sched_cpu_ns / 1e9) : 0.0;
    if (sched_unattributed_s < 0.0) {
        sched_unattributed_s = 0.0; // sched_cpu_ns is the finer instrument; the tick can round under it
    }

    {
        const struct BenchStatsTotals totals = {
            wire_bytes_total, wire_packets_total, peak_rss_kb, samples, sched_cpu_ns, cpu_s, sched_unattributed_s};
        bench_stats_fail_flags(stats, &totals, fail, sizeof(fail));
    }

    snprintf(buf, buf_len,
             "sample_bytes=%" PRIu64 " utime_s=%.3f stime_s=%.3f cpu_s_per_Msample=%.3f cpu_s_per_MB=%.6f "
             "sched_cpu_s=%.6f sched_cpu_s_per_Msample=%.3f sched_unattributed_s=%.6f sched_breakdown=%s "
             "sched_threads=%u sched_threads_gone=%u sched_threads_over=%u peak_rss_kb=%" PRIu64
             " wire_rx_bytes=%" PRIu64 " wire_rx_packets=%" PRIu64 " wire_tx_bytes=%" PRIu64 " wire_tx_packets=%" PRIu64
             " wire_bytes_total=%" PRIu64 " wire_packets_total=%" PRIu64
             " wire_bytes_per_sample=%.1f wire_packets_per_sample=%.3f wire_role_packets_per_sample=%.3f "
             "iface=%s instrument=%s%s%s%s sched_by_thread=%s%s%s%s",
             sample_bytes, utime_s, stime_s, samples > 0 ? cpu_s * 1e6 / (double)samples : 0.0,
             megabytes > 0.0 ? cpu_s / megabytes : 0.0, (double)sched_cpu_ns / 1e9,
             samples > 0 ? (double)sched_cpu_ns / 1e3 / (double)samples : 0.0, sched_unattributed_s,
             sched_breakdown_state(cpu_s - stats->cpu_begin_s, sched_unattributed_s), stats->threads_end.count,
             sched_gone, stats->threads_end.over, peak_rss_kb, rx_bytes, rx_packets, tx_bytes, tx_packets,
             wire_bytes_total, wire_packets_total, samples > 0 ? (double)wire_bytes_total / (double)samples : 0.0,
             samples > 0 ? (double)wire_packets_total / (double)samples : 0.0,
             samples > 0 ? (double)role_packets / (double)samples : 0.0, stats->iface, fail[0] != '\0' ? "fail:" : "ok",
             fail, BENCH_CORE_BUILD_FIELD, BENCH_CORE_BUILD_VALUE, by_thread, transport, fallbacks, shmdiag);
    return buf;
}

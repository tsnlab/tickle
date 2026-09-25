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
// overflowing if that ever stops being true.
#define BENCH_STATS_FIELDS_MAX 1024

#define BENCH_ROLE_SENDER 0
#define BENCH_ROLE_RECEIVER 1

struct BenchStatsCounters {
    uint64_t rx_bytes;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t tx_packets;
    int valid;
};

struct BenchStats {
    struct BenchStatsCounters net_begin;
    struct BenchStatsCounters net_end;
    double cpu_begin_s;
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

static inline void bench_stats_begin(struct BenchStats* stats) {
    memset(stats, 0, sizeof(*stats));
    snprintf(stats->iface, sizeof(stats->iface), "%s", bench_stats_iface());
    stats->cpu_begin_s = bench_stats_cpu_seconds();
    bench_stats_read_net(stats->iface, &stats->net_begin);
}

static inline void bench_stats_end(struct BenchStats* stats) {
    bench_stats_read_net(stats->iface, &stats->net_end);
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
#ifdef BENCH_CORE_BUILD
#define BENCH_STRINGIFY_(x) #x
#define BENCH_STRINGIFY(x) BENCH_STRINGIFY_(x)
#define BENCH_CORE_BUILD_FIELD " core_build="
#define BENCH_CORE_BUILD_VALUE BENCH_STRINGIFY(BENCH_CORE_BUILD)
#else
#define BENCH_CORE_BUILD_FIELD ""
#define BENCH_CORE_BUILD_VALUE ""
#endif

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
    char fail[64];

    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        utime_s = (double)usage.ru_utime.tv_sec + ((double)usage.ru_utime.tv_usec / 1e6);
        stime_s = (double)usage.ru_stime.tv_sec + ((double)usage.ru_stime.tv_usec / 1e6);
        cpu_s = utime_s + stime_s;
    }

    // Each check names the counter that failed rather than a single "bad" flag, because the three
    // fail for unrelated reasons: a wrong interface name, a kernel without VmHWM, a run that
    // delivered nothing. Only the last of those is a property of the run.
    fail[0] = '\0';
    if (!stats->net_begin.valid || !stats->net_end.valid || wire_bytes_total == 0 || wire_packets_total == 0) {
        snprintf(fail + strlen(fail), sizeof(fail) - strlen(fail), "%snet", fail[0] != '\0' ? "," : "");
    }
    if (cpu_s <= 0.0) {
        snprintf(fail + strlen(fail), sizeof(fail) - strlen(fail), "%scpu", fail[0] != '\0' ? "," : "");
    }
    if (peak_rss_kb == 0) {
        snprintf(fail + strlen(fail), sizeof(fail) - strlen(fail), "%srss", fail[0] != '\0' ? "," : "");
    }
    if (samples == 0) {
        // Not a counter failure: the run genuinely delivered nothing. Every per-sample figure below
        // is then 0 by construction rather than measured, and saying so is the honest report.
        snprintf(fail + strlen(fail), sizeof(fail) - strlen(fail), "%ssamples", fail[0] != '\0' ? "," : "");
    }

    snprintf(buf, buf_len,
             "sample_bytes=%" PRIu64 " utime_s=%.3f stime_s=%.3f cpu_s_per_Msample=%.3f cpu_s_per_MB=%.3f "
             "peak_rss_kb=%" PRIu64 " wire_rx_bytes=%" PRIu64 " wire_rx_packets=%" PRIu64 " wire_tx_bytes=%" PRIu64
             " wire_tx_packets=%" PRIu64 " wire_bytes_total=%" PRIu64 " wire_packets_total=%" PRIu64
             " wire_bytes_per_sample=%.1f wire_packets_per_sample=%.3f wire_role_packets_per_sample=%.3f "
             "iface=%s instrument=%s%s%s%s",
             sample_bytes, utime_s, stime_s, samples > 0 ? cpu_s * 1e6 / (double)samples : 0.0,
             megabytes > 0.0 ? cpu_s / megabytes : 0.0, peak_rss_kb, rx_bytes, rx_packets, tx_bytes, tx_packets,
             wire_bytes_total, wire_packets_total, samples > 0 ? (double)wire_bytes_total / (double)samples : 0.0,
             samples > 0 ? (double)wire_packets_total / (double)samples : 0.0,
             samples > 0 ? (double)role_packets / (double)samples : 0.0, stats->iface, fail[0] != '\0' ? "fail:" : "ok",
             fail, BENCH_CORE_BUILD_FIELD, BENCH_CORE_BUILD_VALUE);
    return buf;
}

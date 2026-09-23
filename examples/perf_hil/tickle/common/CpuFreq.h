/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Samples the CPU frequency during a run, so a measured rate can be read next to the P-state that
// produced it.
//
// Why this exists (2026-09-23): reliable_throughput came back bimodal at 0% injected loss, about
// 95 and about 112 Mbps with nothing in between, across five reps. Every other field of the
// RESULT line was identical between the two modes - same elapsed_s, write_fail 0, throttle_lag 0,
// drained=acked, peer_acks 1/1 - so the sender was never blocked and never throttled; it simply
// got through about 19% fewer loop iterations in the same ten seconds. Both rig hosts run the
// ondemand governor over a 1.5-2.4 GHz range, which makes a discrete P-state change the obvious
// suspect, and 2.4/2.0 is very close to the measured 1.194 ratio. Reporting the frequency next to
// the rate is what turns "consistent with" into "measured", and it explains the numbers already
// published rather than only fixing the ones taken from here on.
//
// Header-only and plain C on purpose: every scenario's build compiles exactly one shared .c file
// (common/Bench.c), and the CycloneDDS and FastDDS harnesses are built by their own scripts, so a
// new translation unit would mean editing three build scripts to answer one question. Including
// this from a C++ harness works as-is.
//
// Reads cpu0 only. On the rig's Raspberry Pi 5 there is a single cpufreq policy whose related_cpus
// is "0 1 2 3", so cpu0's scaling_cur_freq is the frequency of the whole package and there is no
// need to chase the measuring thread across cores. On a machine with per-core policies this would
// report the wrong core's state, which is why it is stated here rather than assumed.

#pragma once

#include <stdint.h>
#include <stdio.h>

// Overridable so the parsing and min/max/mean bookkeeping can be exercised against a fixture file
// on a host that has no cpufreq sysfs at all - which is the case on the development machine, where
// the real path is absent and only the graceful-degradation branch would otherwise ever run.
#ifndef BENCH_CPUFREQ_PATH
#define BENCH_CPUFREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#endif

struct BenchCpuFreq {
    uint32_t min_khz;
    uint32_t max_khz;
    uint64_t sum_khz;
    uint32_t samples;
    uint64_t next_sample_ns;
};

static inline void BenchCpuFreq_init(struct BenchCpuFreq* freq) {
    freq->min_khz = UINT32_MAX;
    freq->max_khz = 0;
    freq->sum_khz = 0;
    freq->samples = 0;
    freq->next_sample_ns = 0;
}

// Call from the send or receive path; it reads sysfs at most once per period_ns and returns
// immediately otherwise, so putting it in a hot loop costs a comparison. At the default 100ms
// against a ten-second run that is a hundred opens, against nearly two million publishes - far
// below the effect being measured, but not free, which is why it is rate-limited rather than
// sampled per iteration.
static inline void BenchCpuFreq_sample(struct BenchCpuFreq* freq, uint64_t now_ns, uint64_t period_ns) {
    if (now_ns < freq->next_sample_ns) {
        return;
    }
    freq->next_sample_ns = now_ns + period_ns;

    FILE* stream = fopen(BENCH_CPUFREQ_PATH, "re");
    if (stream == NULL) {
        return; // not a cpufreq-capable host; samples stays 0 and the report says so
    }
    unsigned long khz = 0;
    if (fscanf(stream, "%lu", &khz) == 1 && khz > 0) {
        if ((uint32_t)khz < freq->min_khz) {
            freq->min_khz = (uint32_t)khz;
        }
        if ((uint32_t)khz > freq->max_khz) {
            freq->max_khz = (uint32_t)khz;
        }
        freq->sum_khz += khz;
        freq->samples++;
    }
    fclose(stream);
}

static inline double BenchCpuFreq_mean_mhz(const struct BenchCpuFreq* freq) {
    return freq->samples > 0 ? (double)freq->sum_khz / (double)freq->samples / 1000.0 : -1.0;
}

static inline double BenchCpuFreq_min_mhz(const struct BenchCpuFreq* freq) {
    return freq->samples > 0 ? (double)freq->min_khz / 1000.0 : -1.0;
}

static inline double BenchCpuFreq_max_mhz(const struct BenchCpuFreq* freq) {
    return freq->samples > 0 ? (double)freq->max_khz / 1000.0 : -1.0;
}

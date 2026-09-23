/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Records which CPU the measuring thread actually ran on, so a rate can be read next to the core
// that produced it.
//
// Why this exists (2026-09-23), and what it is NOT a repeat of: reliable_throughput is bimodal at
// 0% loss, about 95 and about 112 Mbit/s with nothing between, and every other field of the
// RESULT line is identical between the modes - same elapsed_s, write_fail 0, throttle_lag 0. The
// first explanation was CPU frequency, and CpuFreq.h measured it and killed it: both modes report
// cpu_mhz_mean 2391.0, 99.6% of the 2400 MHz maximum, on 12 reps. Same clock, both hosts, both
// modes. Two discrete states at identical frequency fit *placement* rather than speed - which
// core the sender lands on relative to the core taking the NIC interrupt - and that is a
// different quantity, which needs a different instrument rather than a re-reading of the old one.
//
// This matters for CpuFreq.h too: that header samples cpu0, which is the whole package only
// because the rig's Pi 5 has a single cpufreq policy covering cpu0-3. "The package was at full
// clock" says nothing about the core the thread was on, and if placement turns out to matter then
// that distinction stops being a footnote.
//
// Uses the getcpu syscall directly rather than sched_getcpu(), which needs _GNU_SOURCE defined
// before any libc header - not something a header included partway down a translation unit can
// arrange for itself.

#pragma once

#include <stdint.h>
#include <unistd.h>

#include <sys/syscall.h>

#define BENCH_CPU_PLACE_MAX_CPUS 32

struct BenchCpuPlace {
    uint32_t samples_on_cpu[BENCH_CPU_PLACE_MAX_CPUS];
    uint32_t samples;
    uint32_t migrations; // times the observed cpu differed from the previous sample
    int last_cpu;
    uint64_t next_sample_ns;
};

static inline void BenchCpuPlace_init(struct BenchCpuPlace* place) {
    for (int i = 0; i < BENCH_CPU_PLACE_MAX_CPUS; i++) {
        place->samples_on_cpu[i] = 0;
    }
    place->samples = 0;
    place->migrations = 0;
    place->last_cpu = -1;
    place->next_sample_ns = 0;
}

// Rate-limited exactly like BenchCpuFreq_sample(), and for the same reason: an instrument that
// perturbs the thing it measures is worse than no instrument. getcpu is a vDSO call on this
// platform, so it is cheaper than the sysfs read, but sampling it per publish would still be
// self-defeating at nearly two million publishes in ten seconds.
static inline void BenchCpuPlace_sample(struct BenchCpuPlace* place, uint64_t now_ns, uint64_t period_ns) {
    if (now_ns < place->next_sample_ns) {
        return;
    }
    place->next_sample_ns = now_ns + period_ns;

    unsigned cpu = 0;
    if (syscall(SYS_getcpu, &cpu, NULL, NULL) != 0) {
        return; // no getcpu here; samples stays 0 and the report says so
    }
    if (cpu < BENCH_CPU_PLACE_MAX_CPUS) {
        place->samples_on_cpu[cpu]++;
    }
    if (place->last_cpu >= 0 && (int)cpu != place->last_cpu) {
        place->migrations++;
    }
    place->last_cpu = (int)cpu;
    place->samples++;
}

// The cpu this thread spent most of its samples on, or -1 if nothing was sampled. Reported
// alongside the migration count rather than instead of it: a thread pinned to one core and a
// thread bouncing between two are different situations, and a single "main cpu" hides that.
static inline int BenchCpuPlace_main_cpu(const struct BenchCpuPlace* place) {
    int best = -1;
    uint32_t best_count = 0;
    for (int i = 0; i < BENCH_CPU_PLACE_MAX_CPUS; i++) {
        if (place->samples_on_cpu[i] > best_count) {
            best_count = place->samples_on_cpu[i];
            best = i;
        }
    }
    return best;
}

// Fraction of samples spent on the main cpu, 0.0 to 1.0, or -1.0 if nothing was sampled.
static inline double BenchCpuPlace_main_share(const struct BenchCpuPlace* place) {
    int main_cpu = BenchCpuPlace_main_cpu(place);
    if (main_cpu < 0 || place->samples == 0) {
        return -1.0;
    }
    return (double)place->samples_on_cpu[main_cpu] / (double)place->samples;
}

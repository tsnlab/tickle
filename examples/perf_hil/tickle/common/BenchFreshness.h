/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// KEEP_LAST freshness (the user's decision D3, 2026-10-18): in a KEEP_LAST cell freshness is the primary score and
// goodput a diagnostic. Freshness is
//   (1) the AGE of a sample at the moment the application takes it: take time - the sample's creation time, p50, p99;
//   (2) the NEWEST SHARE: the share of the sender's bursts whose newest sample the application took.
// A sample superseded under KEEP_LAST is not lost: only the newest of each burst is owed.
//
// Opt-in: `--fresh` on client and server (run_scenario.sh and s6_transport_cells.sh hand the same arguments to both).
// Without it nothing here runs, nothing is written into the payload and no field is printed, so every existing cell,
// RSS figure included, is the run it was. Header-only plain C, included by all three frameworks' benches like
// CreationTime.h, so the three cannot drift apart. Read by examples/perf_hil/experiments/freshness_summary.py.
// `--fresh-dump` is --fresh plus every kept age with its take time on the server's RESULT line (fresh_dump=), for the
// cross-host cells, whose reader corrects each age by the clock offset at its own take time.
//
// CREATION TIME. Where the sender has a schedule (a paced writer: history_depth_burst_loss, a -i throughput client)
// the creation time is the burst's DUE time, when the schedule said to write it (docs/TESTING.md section 4, "Creation
// latency"); where it has none (a max-rate throughput client) it is the write's own stamp. The sender says which
// (fresh_creation=due|stamp). The time is CLOCK_REALTIME, the clock clock_offset_probe.py measures between two hosts:
// CLOCK_MONOTONIC has an unrelated origin on each host and cannot be corrected. A due time the sender keeps on
// CLOCK_MONOTONIC is carried over with the realtime-minus-monotonic difference read at the write, so a slew between
// the start of the run and the write does not shift it. The take time is CLOCK_REALTIME read when the application has
// the sample in hand (TickLE: in the subscriber callback; DDS: after dds_take()/take_next_sample() returned it).
//
// The age printed is RAW: the receiver's clock minus the sender's. On one host that is the age; across hosts the
// summary subtracts the offset clock_offset_probe.py measured (receiver - sender) before and after the run, and voids
// the run when the offset's uncertainty exceeds the age being read.
//
// BURST, defined by the sender's schedule. `--burst B` (default 1) makes the sender write B samples back to back per
// scheduled slot: burst k is seqs (k-1)B+1 .. kB, all with burst k's due time, and seq kB is its newest. Seqs start at
// 1. A max-rate sender has no slots; its burst k is simply the B consecutive writes (k-1)B+1 .. kB, each with its own
// stamp. The sender marks the newest of each burst in the stamp it writes (no seq arithmetic at the receiver), counts
// every burst it scheduled (fresh_bursts=, written or not: a burst whose newest write failed is still owed), and the
// receiver counts the distinct newest samples it took (fresh_newest=). newest share = fresh_newest / fresh_bursts.
//
// WINDOW. A throughput sender has a warm-up and a cool-down (BenchWindow.h); only bursts whose newest sample falls in
// the measured window [start + warm-up, start + warm-up + duration] of the sender's own schedule are counted, and the
// receiver reads ages only from samples the sender flagged as in the window - so no freshness figure is a start-up or
// teardown transient (docs/TESTING.md section 5, rule 9). A sender with no window (history_depth_burst_loss) flags
// every sample.
//
// THE STAMP: the first 16 bytes of the sample's payload (every shape P1-P4 has at least 64), native byte order (every
// pair in this comparison is one architecture): magic, burst size, flags, created_wall_ns. A sample without the magic
// is counted in fresh_no_stamp= and never read as age 0; any no_stamp voids the run.
//
// --fresh-skew-us S (sender only): adds S us to every creation time it stamps. A fault arm for the summary's clock
// control, which must then fail - a deliberate wrong clock is how the control is shown to be able to fail.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_FRESH_MAGIC 0x46524553U // "SERF" little-endian; anything else is not a stamp
#define BENCH_FRESH_STAMP_BYTES 16U
#define BENCH_FRESH_FLAG_IN_WINDOW 1U
#define BENCH_FRESH_FLAG_NEWEST 2U
// Ages kept for the quantiles. Past this the store halves itself and keeps every 2nd, 4th, ... take, so the kept
// ages stay an even sample of the whole run (fresh_kept= says how many); min and max are exact over every take.
#ifndef BENCH_FRESH_MAX
#define BENCH_FRESH_MAX 65536U
#endif
#define BENCH_FRESH_FIELDS_MAX 448

struct bench_fresh_stamp {
    uint32_t magic;
    uint16_t burst;
    uint16_t flags;
    int64_t created_wall_ns;
};

static inline int64_t bench_fresh_ts_ns(const struct timespec* ts) {
    const int64_t ns_per_s = 1000000000LL;
    return ((int64_t)ts->tv_sec * ns_per_s) + (int64_t)ts->tv_nsec;
}

static inline int64_t bench_fresh_wall_ns(void) {
    struct timespec ts;
    // POSIX <time.h> provides it; glibc defines it in bits/time.h, which include-cleaner asks for instead.
    clock_gettime(CLOCK_REALTIME, &ts); // NOLINT(misc-include-cleaner)
    return bench_fresh_ts_ns(&ts);
}

// A CLOCK_MONOTONIC time (a due time on the sender's schedule) on CLOCK_REALTIME, with the two clocks' difference read
// now.
static inline int64_t bench_fresh_mono_to_wall(uint64_t mono_ns) {
    struct timespec ts;
    int64_t wall = bench_fresh_wall_ns();
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - as above
    return (int64_t)mono_ns + (wall - bench_fresh_ts_ns(&ts));
}

// ---------------------------------------------------------------------------------------------------- sender ----

struct bench_fresh_sender {
    bool on;
    uint32_t burst;     // --burst B
    int64_t skew_ns;    // --fresh-skew-us S
    uint32_t in_burst;  // writes so far in the current burst
    uint64_t bursts;    // bursts whose newest falls in the window: what the receiver is owed
    uint64_t stamped;   // samples stamped
    bool due_schedule;  // creation = due time (true) or the write's stamp (false)
    uint64_t win_lo_ns; // the measured window on the sender's CLOCK_MONOTONIC; 0/UINT64_MAX: no window
    uint64_t win_hi_ns;
};

static inline void bench_fresh_sender_init(struct bench_fresh_sender* sender) {
    memset(sender, 0, sizeof(*sender));
    sender->burst = 1;
    sender->win_hi_ns = UINT64_MAX;
}

// --fresh, --burst B, --fresh-skew-us S. Returns true if argv[*idx] was one of them (its value consumed).
static inline bool bench_fresh_sender_parse_arg(struct bench_fresh_sender* sender, int argc, char** argv, int* idx) {
    const long burst_max = 65535;
    const double ns_per_us = 1e3;
    if (strcmp(argv[*idx], "--fresh") == 0 || strcmp(argv[*idx], "--fresh-dump") == 0) { // the dump is the receiver's
        sender->on = true;
        return true;
    }
    if (*idx + 1 >= argc) {
        return false;
    }
    if (strcmp(argv[*idx], "--burst") == 0) {
        long burst = strtol(argv[++*idx], NULL, 10);
        sender->burst = burst >= 1 && burst <= burst_max ? (uint32_t)burst : sender->burst;
        return true;
    }
    if (strcmp(argv[*idx], "--fresh-skew-us") == 0) {
        sender->skew_ns = (int64_t)(strtod(argv[++*idx], NULL) * ns_per_us);
        return true;
    }
    return false;
}

// The measured window on the sender's CLOCK_MONOTONIC (a throughput sender); without it every sample is in it.
static inline void bench_fresh_sender_window(struct bench_fresh_sender* sender, uint64_t lo_ns, uint64_t hi_ns) {
    sender->win_lo_ns = lo_ns;
    sender->win_hi_ns = hi_ns;
}

// Stamps one write into payload. created_wall_ns is the burst's due time on CLOCK_REALTIME (or the write's stamp),
// sched_mono_ns the same instant on CLOCK_MONOTONIC, which the window is judged by. Call once per write attempt, in
// seq order, whether or not the write then succeeds.
static inline void bench_fresh_stamp(struct bench_fresh_sender* sender, uint8_t* payload, size_t len,
                                     int64_t created_wall_ns, uint64_t sched_mono_ns) {
    if (!sender->on || len < BENCH_FRESH_STAMP_BYTES) {
        return;
    }
    struct bench_fresh_stamp stamp;
    stamp.magic = BENCH_FRESH_MAGIC;
    stamp.burst = (uint16_t)sender->burst;
    stamp.flags = 0;
    stamp.created_wall_ns = created_wall_ns + sender->skew_ns;
    bool in_window = sched_mono_ns >= sender->win_lo_ns && sched_mono_ns <= sender->win_hi_ns;
    if (in_window) {
        stamp.flags |= BENCH_FRESH_FLAG_IN_WINDOW;
    }
    if (++sender->in_burst >= sender->burst) {
        sender->in_burst = 0;
        stamp.flags |= BENCH_FRESH_FLAG_NEWEST;
        if (in_window) {
            sender->bursts++;
        }
    }
    memcpy(payload, &stamp, sizeof(stamp));
    sender->stamped++;
}

// The sender's RESULT-line fields, with a leading space; "" when --fresh was not given, so a run without it prints the
// line it always printed, byte for byte (print it as "...%s\n", with no space before it).
static inline const char* bench_fresh_sender_fields(const struct bench_fresh_sender* sender, char* buf, size_t len) {
    const double ns_per_us = 1e3;
    if (!sender->on) {
        buf[0] = '\0';
        return buf;
    }
    (void)snprintf(buf, len,
                   " fresh=on fresh_burst=%u fresh_bursts=%llu fresh_stamped=%llu fresh_creation=%s "
                   "fresh_skew_us=%.1f fresh_window=%s",
                   (unsigned)sender->burst, (unsigned long long)sender->bursts, (unsigned long long)sender->stamped,
                   sender->due_schedule ? "due" : "stamp", (double)sender->skew_ns / ns_per_us,
                   sender->win_hi_ns == UINT64_MAX ? "all" : "sender");
    return buf;
}

// -------------------------------------------------------------------------------------------------- receiver ----

struct bench_fresh_receiver {
    bool on;
    uint64_t taken;       // samples taken
    uint64_t no_stamp;    // samples without a stamp (must stay 0)
    uint64_t in_window;   // samples taken that the sender flagged as in the window: the ones whose ages are read
    uint64_t newest;      // distinct newest-of-burst samples taken, in the window
    uint64_t newest_late; // a newest sample taken after a later burst's newest (not counted in newest)
    uint32_t last_newest_seq;
    uint32_t burst;  // the burst size the stamps carried (0: none seen; 65535: they disagreed)
    uint64_t seen;   // in-window ages offered to the store
    uint64_t stride; // keep every stride-th of them
    uint32_t kept;
    int64_t min_ns;
    int64_t max_ns;
    bool dump; // --fresh-dump: every kept (take time, age) pair goes on the RESULT line too
    double age_us[BENCH_FRESH_MAX];
    int64_t take_ns[BENCH_FRESH_MAX]; // the take time of each kept age, CLOCK_REALTIME
};

// The struct is meant to be a static (zeroed) object, as BenchWindow: its array's pages stay unmapped until used.
static inline void bench_fresh_receiver_init(struct bench_fresh_receiver* receiver) {
    receiver->stride = 1;
}

static inline bool bench_fresh_receiver_parse_arg(struct bench_fresh_receiver* receiver, const char* arg) {
    if (strcmp(arg, "--fresh") == 0) {
        receiver->on = true;
        return true;
    }
    // --fresh-dump (implies --fresh): the RESULT line also carries every kept age with its take time, so a cross-host
    // reader can correct each age by the clock offset AT its take time - interpolated between the probes before and
    // after the run - instead of by one offset for the whole run, whose drift between the two probes would otherwise
    // count in full against the age (freshness_summary.py, R1). For runs of up to BENCH_FRESH_MAX takes (the history
    // cells); about 15 bytes per take.
    if (strcmp(arg, "--fresh-dump") == 0) {
        receiver->on = true;
        receiver->dump = true;
        return true;
    }
    return false;
}

static inline void bench_fresh_keep(struct bench_fresh_receiver* receiver, double age_us, int64_t take_ns) {
    if (receiver->seen++ % receiver->stride != 0) {
        return;
    }
    if (receiver->kept == BENCH_FRESH_MAX) {
        for (uint32_t i = 0; i < BENCH_FRESH_MAX / 2; i++) {
            receiver->age_us[i] = receiver->age_us[(size_t)2 * i];
            receiver->take_ns[i] = receiver->take_ns[(size_t)2 * i];
        }
        receiver->kept = BENCH_FRESH_MAX / 2;
        receiver->stride *= 2;
        if ((receiver->seen - 1) % receiver->stride != 0) {
            return;
        }
    }
    receiver->take_ns[receiver->kept] = take_ns;
    receiver->age_us[receiver->kept++] = age_us;
}

// One sample the application has just taken: its seq, its payload, and the take time on CLOCK_REALTIME.
static inline void bench_fresh_take(struct bench_fresh_receiver* receiver, uint32_t seq, const uint8_t* payload,
                                    size_t len, int64_t take_wall_ns) {
    const double ns_per_us = 1e3;
    const uint32_t burst_disagree = 65535;
    if (!receiver->on) {
        return;
    }
    receiver->taken++;
    struct bench_fresh_stamp stamp;
    if (len < BENCH_FRESH_STAMP_BYTES) {
        receiver->no_stamp++;
        return;
    }
    memcpy(&stamp, payload, sizeof(stamp));
    if (stamp.magic != BENCH_FRESH_MAGIC) {
        receiver->no_stamp++;
        return;
    }
    if (receiver->burst == 0) {
        receiver->burst = stamp.burst;
    } else if (receiver->burst != stamp.burst) {
        receiver->burst = burst_disagree;
    }
    if ((stamp.flags & BENCH_FRESH_FLAG_IN_WINDOW) == 0) {
        return;
    }
    receiver->in_window++;
    if ((stamp.flags & BENCH_FRESH_FLAG_NEWEST) != 0) {
        if (seq > receiver->last_newest_seq) {
            receiver->newest++;
            receiver->last_newest_seq = seq;
        } else {
            receiver->newest_late++;
        }
    }
    int64_t age_ns = take_wall_ns - stamp.created_wall_ns;
    if (receiver->in_window == 1 || age_ns < receiver->min_ns) {
        receiver->min_ns = age_ns;
    }
    if (receiver->in_window == 1 || age_ns > receiver->max_ns) {
        receiver->max_ns = age_ns;
    }
    bench_fresh_keep(receiver, (double)age_ns / ns_per_us, take_wall_ns);
}

static inline int bench_fresh_compare(const void* left, const void* right) {
    double lhs = *(const double*)left;
    double rhs = *(const double*)right;
    return (lhs > rhs) - (lhs < rhs);
}

// Nearest-rank quantile of the kept ages, as RttQuantiles.h. Sorts in place: call after the run.
static inline double bench_fresh_quantile(struct bench_fresh_receiver* receiver, double q) {
    const double round_up = 0.999999;
    qsort(receiver->age_us, receiver->kept, sizeof(receiver->age_us[0]), bench_fresh_compare);
    uint32_t rank = (uint32_t)((q * (double)receiver->kept) + round_up);
    rank = rank < 1 ? 1 : rank;
    rank = rank > receiver->kept ? receiver->kept : rank;
    return receiver->age_us[rank - 1];
}

// The fields without the dump, into buf. Sorts the kept ages: after it the pairs with take_ns no longer hold.
static inline const char* bench_fresh_receiver_fields_only(struct bench_fresh_receiver* receiver, char* buf, size_t len,
                                                           double p50, double p99, double ns_per_us) {
    int head = snprintf(buf, len,
                        " fresh=on fresh_taken=%llu fresh_no_stamp=%llu fresh_in_window=%llu fresh_burst=%u "
                        "fresh_newest=%llu fresh_newest_late=%llu fresh_kept=%u ",
                        (unsigned long long)receiver->taken, (unsigned long long)receiver->no_stamp,
                        (unsigned long long)receiver->in_window, (unsigned)receiver->burst,
                        (unsigned long long)receiver->newest, (unsigned long long)receiver->newest_late,
                        (unsigned)receiver->kept);
    if (head < 0 || (size_t)head >= len) {
        return buf;
    }
    if (receiver->kept == 0) {
        (void)snprintf(buf + head, len - (size_t)head,
                       "fresh_age_p50_us=none fresh_age_p99_us=none fresh_age_min_us=none fresh_age_max_us=none");
        return buf;
    }
    double age_p50 = bench_fresh_quantile(receiver, p50);
    double age_p99 = bench_fresh_quantile(receiver, p99);
    (void)snprintf(buf + head, len - (size_t)head,
                   "fresh_age_p50_us=%.1f fresh_age_p99_us=%.1f fresh_age_min_us=%.1f fresh_age_max_us=%.1f", age_p50,
                   age_p99, (double)receiver->min_ns / ns_per_us, (double)receiver->max_ns / ns_per_us);
    return buf;
}

// " fresh_dump=<t0>:<dt>/<age>,<dt>/<age>,...": t0 the first kept take time (ns, CLOCK_REALTIME), each dt the take
// time after it in whole us, each age the raw age in us. Written before the quantiles sort the ages. NULL when the
// memory is not there (the line then says fresh_dump=unavailable).
static inline char* bench_fresh_dump_text(const struct bench_fresh_receiver* receiver) {
    const size_t per_take = 32;
    const size_t head_bytes = 48; // " fresh_dump=" and t0
    const int64_t ns_per_us = 1000;
    size_t cap = ((size_t)receiver->kept * per_take) + head_bytes;
    char* text = (char*)malloc(cap);
    if (text == NULL) {
        return NULL;
    }
    size_t used = 0;
    int wrote = snprintf(text, cap, " fresh_dump=%lld:", (long long)(receiver->kept > 0 ? receiver->take_ns[0] : 0));
    used = wrote > 0 ? (size_t)wrote : 0;
    for (uint32_t i = 0; i < receiver->kept && used < cap; i++) {
        wrote = snprintf(text + used, cap - used, "%s%lld/%.1f", i > 0 ? "," : "",
                         (long long)((receiver->take_ns[i] - receiver->take_ns[0]) / ns_per_us), receiver->age_us[i]);
        used += wrote > 0 ? (size_t)wrote : 0;
    }
    return text;
}

// The receiver's RESULT-line fields, with a leading space as the sender's; "" without --fresh. Ages in us, RAW
// (receiver clock - sender clock); with no age read the quantiles print "none" - an age can be negative across hosts,
// so no number can stand for "nothing". With --fresh-dump the returned text is a larger buffer of its own (the fields
// in buf, then the dump), which lives until the process exits.
static inline const char* bench_fresh_receiver_fields(struct bench_fresh_receiver* receiver, char* buf, size_t len) {
    const double ns_per_us = 1e3;
    const double p50 = 0.50;
    const double p99 = 0.99;
    if (!receiver->on) {
        buf[0] = '\0';
        return buf;
    }
    char* dump = receiver->dump ? bench_fresh_dump_text(receiver) : NULL;
    if (dump != NULL) {
        size_t dump_len = strlen(dump);
        size_t both_len = len + dump_len + 1;
        char* both = (char*)malloc(both_len);
        if (both != NULL) {
            const char* fields = bench_fresh_receiver_fields_only(receiver, buf, len, p50, p99, ns_per_us);
            (void)snprintf(both, both_len, "%s%s", fields, dump);
            free(dump);
            return both;
        }
        free(dump);
    }
    const char* fields = bench_fresh_receiver_fields_only(receiver, buf, len, p50, p99, ns_per_us);
    if (receiver->dump) {
        size_t used = strlen(buf);
        (void)snprintf(buf + used, len - used, " fresh_dump=unavailable");
    }
    return fields;
}

/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The adaptive rate control's estimator (tt_LinkOptions.rate_control = tt_RATE_CONTROL_ADAPTIVE;
// examples/perf_hil/experiments/adaptive_pacing/DESIGN.md): from what comes back of a paced send - bytes delivered,
// and now and then a round trip time - it finds the rate the link delivers and keeps the queue short. Plain C with no
// dependency on the rest of the library, so a test can drive it with a synthetic link (tests/test_adaptive_pacing.c).
//
// One ap_pacer per LINK, however many readers are on it: the caller pools the readers' delivered bytes into one
// cumulative count and feeds every reader's round trip samples to the one min filter. All times are ns, all rates bits
// per second, all sizes bytes. Not thread-safe: the caller is the node's own thread.

#ifndef TT_PACING_ADAPTIVE_H
#define TT_PACING_ADAPTIVE_H

#include <stdbool.h>
#include <stdint.h>
#ifdef tt_RATE_TRACE
#include <stdio.h>
#endif

// (initial) constants: starting choices, to be fixed by the model and the tc runs, not results.
#define AP_BUCKETS 8U                   // buckets in a windowed filter
#define AP_BW_BUCKET_ROUNDS 1U          // the delivery-rate max filter: one bucket per active round, AP_BUCKETS rounds
#define AP_RTT_WINDOW_NS 10000000000ULL // the round-trip min filter spans this long of wall time
#define AP_START_GROWTH_PCT 125U        // a START round grows the delivery rate by at least this, else it is a stall
#define AP_START_STALL_ROUNDS 3U        // stalls in a row that end START
#define AP_PROBE_GAIN_PCT 125U          // CRUISE probes at this x BtlBw for one round, then drains at the next
#define AP_PROBE_RUN_GAIN_PCT 200U      // the next probe after one that raised the delivery rate: the link got faster
#define AP_DRAIN_GAIN_PCT 75U           // the round after a probe, and while the queue is over its target
#define AP_PROBE_RISE_PCT 105U          // a probe that raised the delivery rate by this much is followed by another
#define AP_MIN_SAMPLE_BYTES \
    24000U // a delivery-rate sample spans at least this much delivered, so that a
           // unit more or less is a small part of it (the BDP is a few frames)
#define AP_MIN_ROUND_NS \
    1000000ULL                 // and a round is never shorter than this, whatever the round trip: on a loopback it is
                               // microseconds, a round then holds a frame or two and its rate is noise
#define AP_START_LOSSY_PCT 25U // START does not grow in a round whose repairs are above this share of its bytes
#define AP_START_DROP_REPAIR_PCT 50U      // START ends on a loss storm: repairs above this share of a round's bytes
#define AP_MAX_ROUND_RTTS 8U              // but a round is never longer than this many RTprop
#define AP_PROBE_EVERY_ROUNDS 8U          // and does so every this many rounds
#define AP_QUEUE_TARGET_MIN_NS 2000000ULL // queue-delay target: at least this, else RTprop / AP_QUEUE_TARGET_RTT_DIV
#define AP_QUEUE_TARGET_RTT_DIV 4U
#define AP_QUEUE_TARGET_UNITS \
    2U // and at least this many data units of send time at the pacing rate: a unit is the resolution of a sample
#define AP_QUEUE_TRIPS 2U                // queue-delay samples over the target in a row that trip the guard
#define AP_DROP_REPAIR_PCT 50U           // repairs / bytes sent over a round above this trip the rate-drop guard
#define AP_APP_LIMITED_PCT 80U           // a round that sent less than this of its allowance is application-limited
#define AP_IDLE_RESTART_NS 1000000000ULL // quiet this long: the next send starts in START at the remembered rate
#define AP_INFLIGHT_GAIN_PCT 200U        // data in flight is held to this x BtlBw x RTprop
#define AP_CAP_UNSET_BPS 10000000000ULL  // a cap at or above this is no cap
#define AP_START_CAP_DIV 16U             // START begins at this fraction of a set cap ...
#define AP_START_MAX_BPS 8000000ULL      // ... but not above this (a 1 Gbit/s cap is no sign of the link)
#define AP_START_RATE_BPS 1000000ULL     // the rate a send starts at, clamped to [floor, cap], before any is measured
#define AP_NS_PER_SECOND 1000000000ULL   // time and rate arithmetic
#define AP_PERCENT 100U
#define AP_BITS_PER_BYTE 8U
#define AP_RECENT_RTTS 2U // the queue guard reads the least of this many latest round trip samples
#define AP_SOLICIT_UNITS \
    4U // solicitations are at least this many data units of send time apart (a few per cent of the rate)
#define AP_DGRAM_EWMA_DIV 16U // the average data datagram moves a sixteenth of the way to each
#define AP_SAMPLE_CAP_PCT \
    150U // a round's delivery rate is read at most this share of the rate it was sent at: delivery cannot
         // outrun the sends but for what was in flight, and a watermark that moves at a repair's arrival
         // or a jump read as a round that delivered four times the link
#define AP_RTT_MIN_ADVANCE_UNITS \
    4U // a round trip sample is taken only where the watermark moved by at most this many units, or two
       // data datagrams (a unit it moved to by a repair was sent long before it was shown: Karn)
#define AP_START_HOLD_PCT \
    125U // when START finds the link's rate it holds this share of the best round, not another doubling
#define AP_DRAIN_MAX_ROUNDS \
    4U                 // a drain (the queue over its target) lasts at most this many rounds: the evidence that ends it
                       // is a round trip sample, and a send with few of them would otherwise drain for good
#define AP_STAMPS 256U // send times kept, one per data unit sent: the round trip of a unit shown delivered
#define AP_STALL_RTPROPS 8U            // the stall window is at least this many RTprop
#define AP_PRIOR_MAX_BPS 1000000000ULL // a cap below the 1 Gbit/s default is the user's statement of the link: a prior
#define AP_PRIOR_CEIL_PCT 95U          // with a prior the rate starts at, and never goes above, this share of it
#define AP_PRIOR_FLOOR_PCT 25U         // while the prior is believed the rate never goes below this share of it
#define AP_PRIOR_VERIFY_PCT 60U // a round that delivered less than this share of what the round before it sent is low
#define AP_ANCHOR_VERIFY_ROUNDS 4U    // the first rounds: two low ones in a row mean the prior is wrong
#define AP_ANCHOR_QUEUE_ROUNDS 2U     // rounds in a row with a queue trip: congestion
#define AP_ANCHOR_DELIVERY_ROUNDS 3U  // later: this many low rounds in a row are congestion
#define AP_ANCHOR_DECREASE_PCT 80U    // a decrease takes the rate to this share of itself ...
#define AP_ANCHOR_RECOVER_PCT 110U    // ... and a clean round gives back this much, up to the ceiling
#define AP_ANCHOR_STEP_PCT 105U       // a wrong prior steps down to the measured delivery times this
#define AP_DAMPED_PROBE_GAIN_PCT 110U // after a stall restart the probes are gentle ...
#define AP_DAMPED_PROBE_ROUNDS 16U    // ... for this many rounds
#define AP_BIG_ROUND_BYTES 196608U    // a round that delivered this much at once is read over its active time
#define AP_PROBE_EVERY_NS \
    8000000000ULL // with a round that delivered a whole sample at once a probe is due after this long, once two rounds
                  // have passed
#define AP_PROBE_CARRY_ROUNDS 3U // a probe that no full round has judged is carried over this many quiet rounds
#define AP_STALL_QUIET_NS \
    200000000ULL // a send after this long quiet starts the silence afresh: a stall is silence while sending
#define AP_STALL_NS 1000000000ULL // sending this long with nothing delivered restarts the search
#define AP_BW_RESET_FRACTION \
    2U // a guard trip lowers BtlBw to at most this fraction of it in one step (half)     // a guard trip lowers BtlBw
       // to at most this fraction of it in one step
#define AP_MIN_INFLIGHT_BYTES 3000U // the least the in-flight cap is, two full frames and a little
#define AP_SEED_RTT_NS 10000000ULL  // RTprop until a first sample, when the caller has no seed

#define AP_START_CEIL_PCT 200U // START never doubles past this share of the best delivery rate it has seen
#define AP_STALL_MIN_UNITS \
    8U // a stall (sending, nothing delivered) needs this many data units sent in the round: repairs and control are not
       // data
#define AP_RTT_MAX_NS 1000000000ULL // a round trip sample above this is a repair or a queue, not the path
// The packet train (adaptive_pacing/DESIGN.md 28): the arrival span of the first K datagrams of a large sample sent
// back to back is the link's capacity sample. What is believed of it:
#define AP_TRAIN_MIN_SPAN_NS \
    16000ULL // below this - two 8 us units - the clock cannot resolve the span (probe: +-1-2 us)
#define AP_TRAIN_MAX_SPAN_NS 2000000000ULL // a train that took longer than this was not one
#define AP_TRAIN_LINE_PCT 125U     // a sample above 1.25 x the cap (the 1 Gbit/s default's) is not a measurement
#define AP_TRAIN_MAX_JUMP_X 16U    // nor one above 16 x the last estimate (a 4 -> 30 Mbit/s rise is 7.5 x)
#define AP_TRAIN_RISE_PCT 150U     // a rise of more than this over the last estimate needs a second train to agree
#define AP_TRAIN_RATE_PCT 95U      // the rate is this share of the capacity
#define AP_TRAIN_BAD_TRIPS 2U      // guard trips (queue + drop) in the sample after a train: the train read too high
#define AP_TRAIN_BAD_SAMPLES 2U    // that many such samples in a row block rises ...
#define AP_TRAIN_BLOCK_SAMPLES 16U // ... for this many samples
#define AP_TRAIN_FRESH_NS \
    12000000000ULL       // probes are held back for this long after a train (they find what it just measured)
#define AP_TRAIN_HIST 3U // the trains the median reads
#define AP_RTT_OUTLIER_X \
    16U // and so is one above this many times the estimate, once there are three (a queue of a few times the path
        // is the signal the queue guard reads and stays)

struct ap_filter {
    uint64_t value[AP_BUCKETS];
    uint64_t bucket_len; // in the filter's own clock: ns for the min filter, active rounds for the max filter
    uint64_t bucket_start;
    uint32_t head;
    bool max; // max filter (empty = 0), else min filter (empty = UINT64_MAX)
};

enum ap_state { AP_START, AP_CRUISE, AP_ANCHOR }; // ANCHOR: the user's prior is believed (95 % of it, no search)
enum ap_phase { AP_PH_CRUISE, AP_PH_PROBE, AP_PH_DRAIN };

struct ap_pacer {
    uint64_t cap_bps; // never above (the link's link_rate or the user's pacing_rate)
    uint64_t min_bps; // never below
    uint64_t rate_bps;
    uint64_t queue_cfg_ns; // 0 = max(AP_QUEUE_TARGET_MIN_NS, RTprop / AP_QUEUE_TARGET_RTT_DIV)
    uint64_t seed_rtt_ns;
    uint64_t prior_bps;           // the user's statement of the link (a cap below AP_PRIOR_MAX_BPS), 0 = none
    uint32_t anchor_rounds;       // ANCHOR: full rounds seen
    bool anchor_qtrip;            // ... the queue guard tripped in this round
    uint32_t anchor_qrounds;      // ... rounds in a row it did
    uint32_t anchor_low;          // ... low ones in a row
    uint64_t anchor_prev_bps;     // ... the rate the previous full round sent at (0: none, or it was not full)
    uint64_t anchor_ewma_bps;     // ... the delivery rate, smoothed over rounds
    uint64_t prev_sent_bytes;     // ... what it sent, in all
    uint64_t prev_repair_bytes;   // repairs sent in the previous round ...
    uint64_t prev_round_ns;       // ... and how long that round was
    uint64_t last_repair_ns;      // when a repair last went out
    uint64_t round_first_send_ns; // when the round's first data unit left (0: none yet)
    uint64_t last_probe_ns;       // when a probe last began
    uint64_t round_end_ns;        // the end of the round being scored
    uint32_t probe_carried;       // quiet rounds the running probe has been carried over
    uint64_t last_cruise_bps;     // BtlBw at the last CRUISE round: what a stall restart resumes from
    uint32_t damp_probe_rounds;
    uint64_t
        announce_rtt_ns; // the idle round trip to a reader (tt_Context.announce_rtt_ns), 0 = none: a floor for RTprop
    uint64_t unit_bytes; // a data unit on the wire
    uint64_t
        dgram_bytes; // the average data datagram on the wire: one unit, or a run of them (the resolution of a report)
    struct ap_filter bw;  // delivery rate, max over active rounds (idle time does not age it)
    struct ap_filter rtt; // round trip, min over wall time
    enum ap_state state;
    uint64_t round_start_ns;
    uint64_t round_delivered; // cumulative delivered bytes at the round's start
    uint64_t delivered;       // cumulative delivered bytes, as last reported
    uint64_t round_sent;      // bytes sent in this round
    uint64_t round_units0;    // units_sent at the round\'s start
    uint64_t round_repair;    // of them, repairs
    uint64_t prev_round_bps;  // START: the last round's delivery rate
    uint32_t stalls;
    uint32_t rounds_since_probe;
    enum ap_phase phase;
    uint32_t probe_gain_pct;             // this probe round's gain over BtlBw
    uint64_t probe_base_bps;             // the BtlBw a probe round started from
    bool draining;                       // the queue is over its target: run below the delivery rate until it is not
    uint32_t drain_rounds;               // rounds this drain has lasted
    uint32_t queue_over;                 // consecutive rounds over the queue target
    uint64_t recent_rtt[AP_RECENT_RTTS]; // the last round trip samples, newest overwriting oldest
    uint32_t recent_n;                   // samples taken, saturating at AP_RECENT_RTTS
    uint64_t units_sent;                 // data units sent, originals only (a repair is not a new unit)
    uint64_t mark_units; // the watermarks, pooled, less jump_units (cumulative): what is certainly received
    uint64_t held_units; // data units shown received, pooled, less jump_units (cumulative)
    uint64_t
        jump_units; // units a watermark moved over that were never sent: not delivery (an evicted or abandoned sample)
    uint64_t stall_start_ns;      // when the current round began with nothing delivered, 0: not stalled
    uint64_t stamp_ns[AP_STAMPS]; // send time of unit n, at n % AP_STAMPS
    uint32_t low_rounds;          // consecutive rounds below half the BtlBw
    uint64_t last_active_ns;      // when this pacer last sent
    uint64_t rounds;              // active rounds so far, the max filter's clock
    // The packet train: the last estimate it gave (0: none), the per-round samples the median reads, the round of the
    // newest, the guard trips when it was applied, and the bookkeeping that holds rises back when a train reads too
    // high.
    uint64_t train_est_bps;
    uint64_t train_hist[AP_TRAIN_HIST];
    uint32_t train_hist_n;
    uint32_t train_round;
    uint32_t train_bad;
    uint32_t train_block;
    uint64_t train_trips0;
    uint64_t train_fresh_until_ns;
    uint64_t train_samples;
    uint64_t train_rejected;
    uint64_t train_applied;
    // counters
    uint64_t samples;
    uint64_t queue_trips;
    uint64_t drop_trips;
    uint64_t stall_restarts;
    uint64_t jump_events; // reports that moved the watermark past what was sent
    uint64_t rtt_samples; // round trip samples taken
};

static inline uint64_t ap_min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}
static inline uint64_t ap_max_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

static inline void ap_filter_init(struct ap_filter* flt, bool max, uint64_t bucket_len) {
    flt->max = max;
    flt->bucket_len = bucket_len < 1 ? 1 : bucket_len;
    flt->bucket_start = 0;
    flt->head = 0;
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        flt->value[i] = max ? 0 : UINT64_MAX;
    }
}

// Empties the filter and puts `value` in, as of `now`.
static inline void ap_filter_reset(struct ap_filter* flt, uint64_t value, uint64_t now) {
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        flt->value[i] = flt->max ? 0 : UINT64_MAX;
    }
    flt->head = 0;
    flt->bucket_start = now;
    flt->value[0] = value;
}

// Moves the filter's current bucket up to `now`, clearing the buckets it passes.
static inline void ap_filter_advance(struct ap_filter* flt, uint64_t now) {
    if (now < flt->bucket_start) {
        return;
    }
    uint64_t steps = (now - flt->bucket_start) / flt->bucket_len;
    if (steps == 0) {
        return;
    }
    if (steps >= AP_BUCKETS) {
        for (uint32_t i = 0; i < AP_BUCKETS; i++) {
            flt->value[i] = flt->max ? 0 : UINT64_MAX;
        }
        flt->head = 0;
    } else {
        for (uint64_t idx = 0; idx < steps; idx++) {
            flt->head = (flt->head + 1U) % AP_BUCKETS;
            flt->value[flt->head] = flt->max ? 0 : UINT64_MAX;
        }
    }
    flt->bucket_start += steps * flt->bucket_len;
}

static inline void ap_filter_put(struct ap_filter* flt, uint64_t value, uint64_t now) {
    ap_filter_advance(flt, now);
    uint64_t* slot = &flt->value[flt->head];
    *slot = flt->max ? ap_max_u64(*slot, value) : ap_min_u64(*slot, value);
}

// The filter's estimate; 0 for an empty max filter, UINT64_MAX for an empty min filter.
static inline uint64_t ap_filter_get(const struct ap_filter* flt) {
    uint64_t best = flt->max ? 0 : UINT64_MAX;
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        best = flt->max ? ap_max_u64(best, flt->value[i]) : ap_min_u64(best, flt->value[i]);
    }
    return best;
}

// RTprop never above the announce round trip to a reader when one is known: that is a path with nothing queued on it.
// The samples of a send start in the queue the START rate builds (Pi quick tests 6 and 7: 142-153 ms on a 10-40 ms path
// on S1 and S2) and, with the watermark advancing by many units a report, no clean one follows to lower them.
static inline uint64_t ap_floor_rtt(const struct ap_pacer* ctl, uint64_t rtt_ns) {
    return ctl->announce_rtt_ns != 0 ? ap_min_u64(rtt_ns, ctl->announce_rtt_ns) : rtt_ns;
}

// The path's round trip: the median of the minima of the buckets that have a sample - a single early bad sample poisons
// one bucket of the filter, not the estimate; with fewer than three buckets, the least.
static inline uint64_t ap_rtprop_ns(const struct ap_pacer* ctl) {
    uint64_t minima[AP_BUCKETS];
    uint32_t count = 0;
    for (uint32_t idx = 0; idx < AP_BUCKETS; idx++) {
        if (ctl->rtt.value[idx] != UINT64_MAX) {
            minima[count++] = ctl->rtt.value[idx];
        }
    }
    if (count == 0) {
        return ap_floor_rtt(ctl, ctl->seed_rtt_ns);
    }
    for (uint32_t outer = 1; outer < count; outer++) { // insertion sort of at most AP_BUCKETS values
        uint64_t value = minima[outer];
        uint32_t pos = outer;
        while (pos > 0 && minima[pos - 1U] > value) {
            minima[pos] = minima[pos - 1U];
            pos--;
        }
        minima[pos] = value;
    }
    return ap_floor_rtt(ctl, count < 3U ? minima[0] : minima[count / 2U]);
}

static inline uint64_t ap_btlbw_bps(const struct ap_pacer* ctl) {
    return ap_filter_get(&ctl->bw);
}

// The time the pacing rate takes to send what a report cannot resolve finer than: AP_SOLICIT_UNITS units, or one data
// datagram (a run of units is one datagram), whichever is more. Reports are no closer than this, so a round trip sample
// - the time from a unit's send to the report that shows it - is longer than the path's by up to this, and the queue
// target is above it.
static inline uint64_t ap_report_resolution_ns(const struct ap_pacer* ctl) {
    if (ctl->rate_bps == 0) {
        return 0;
    }
    uint64_t bytes = ap_max_u64(AP_SOLICIT_UNITS * ctl->unit_bytes, ctl->dgram_bytes);
    return (bytes * AP_BITS_PER_BYTE * AP_NS_PER_SECOND) / ctl->rate_bps;
}

static inline uint64_t ap_queue_target_ns(const struct ap_pacer* ctl) {
    if (ctl->queue_cfg_ns != 0) {
        return ctl->queue_cfg_ns;
    }
    uint64_t target_ns = ap_max_u64(AP_QUEUE_TARGET_MIN_NS, ap_rtprop_ns(ctl) / AP_QUEUE_TARGET_RTT_DIV);
    return ap_max_u64(target_ns, 2U * ap_report_resolution_ns(ctl));
}

static inline uint64_t ap_clamp_rate(const struct ap_pacer* ctl, uint64_t rate) {
    uint64_t floor_bps = ctl->min_bps;
    if (ctl->state == AP_ANCHOR) {
        floor_bps = ap_max_u64(floor_bps, ctl->prior_bps * AP_PRIOR_FLOOR_PCT / AP_PERCENT);
    }
    return ap_min_u64(ap_max_u64(rate, floor_bps), ctl->cap_bps);
}

// Where a search starts: a sixteenth of the link rate or cap when one is set, at most AP_START_MAX_BPS (a 30 Mbit/s
// link at 1 Mbit/s needed five doublings of 100-200 ms each, longer than a 128 KiB sample), else AP_START_RATE_BPS.
static inline uint64_t ap_start_rate_bps(const struct ap_pacer* ctl) {
    if (ctl->prior_bps != 0) {
        return ctl->prior_bps * AP_PRIOR_CEIL_PCT / AP_PERCENT;
    }
    bool capped = ctl->cap_bps != 0 && ctl->cap_bps < AP_CAP_UNSET_BPS;
    return capped ? ap_min_u64(ap_max_u64(AP_START_RATE_BPS, ctl->cap_bps / AP_START_CAP_DIV), AP_START_MAX_BPS)
                  : AP_START_RATE_BPS;
}

// seed_rtt_ns: a first round trip to the peer (tt_Context.announce_rtt_ns), 0 = AP_SEED_RTT_NS.
static inline void ap_init(struct ap_pacer* ctl, uint64_t min_bps, uint64_t cap_bps, uint64_t queue_cfg_ns,
                           uint64_t seed_rtt_ns, uint64_t unit_bytes) {
    *ctl = (struct ap_pacer) {0};
    ctl->min_bps = min_bps;
    ctl->cap_bps = cap_bps;
    ctl->prior_bps = cap_bps != 0 && cap_bps < AP_PRIOR_MAX_BPS ? cap_bps : 0;
    if (ctl->prior_bps != 0) {
        ctl->cap_bps =
            ctl->prior_bps * AP_PRIOR_CEIL_PCT / AP_PERCENT; // the user says the bandwidth, the library takes 95 %
    }
    ctl->queue_cfg_ns = queue_cfg_ns;
    ctl->unit_bytes = unit_bytes;
    ctl->dgram_bytes = unit_bytes;
    ctl->seed_rtt_ns = seed_rtt_ns != 0 ? seed_rtt_ns : AP_SEED_RTT_NS;
    ap_filter_init(&ctl->bw, true, AP_BW_BUCKET_ROUNDS);
    ap_filter_init(&ctl->rtt, false, AP_RTT_WINDOW_NS / AP_BUCKETS);
    ctl->state = ctl->prior_bps != 0 ? AP_ANCHOR : AP_START;
    ctl->probe_gain_pct = AP_PROBE_GAIN_PCT;
    ctl->rate_bps = ap_clamp_rate(ctl, ap_start_rate_bps(ctl));
}

// The announce round trip to a reader, ns (0: not measured).
static inline void ap_note_announce_rtt(struct ap_pacer* ctl, uint64_t rtt_ns) {
    ctl->announce_rtt_ns = rtt_ns;
}

// The rate to pace the link at, bits/s.
static inline uint64_t ap_rate_bps(const struct ap_pacer* ctl) {
    return ctl->rate_bps;
}

// How long a writer waits between solicitations (the HEARTBEATs that draw a report from a clean stream): the least of a
// round (the round trip, at least AP_MIN_ROUND_NS) and AP_SOLICIT_UNITS data units of send time at the pacing rate.
// Self-clocked at the report rate (a report asks the next solicitation) it sent a few thousand a second on a fast path,
// 156 bytes on the wire each, and the control traffic charged into the bucket took the whole rate of a slow start - the
// data starved, the estimate read nothing delivered, and the floor it fell to was less than the solicitations alone.
static inline uint64_t ap_solicit_gap_ns(const struct ap_pacer* ctl) {
    uint64_t round_ns = ap_max_u64(ap_rtprop_ns(ctl), AP_MIN_ROUND_NS);
    return ap_max_u64(round_ns, ap_report_resolution_ns(ctl));
}

// How many bytes may be in flight (sent, not yet delivered); UINT64_MAX until a rate is known.
static inline uint64_t ap_inflight_limit(const struct ap_pacer* ctl) {
    uint64_t bandwidth = ap_btlbw_bps(ctl);
    if (bandwidth == 0) {
        return UINT64_MAX;
    }
    uint64_t bdp = (bandwidth / AP_BITS_PER_BYTE) * ap_rtprop_ns(ctl) / AP_NS_PER_SECOND;
    return ap_max_u64(bdp * AP_INFLIGHT_GAIN_PCT / AP_PERCENT, AP_MIN_INFLIGHT_BYTES);
}

// A new round starts at `now`.
static inline void ap_round_begin(struct ap_pacer* ctl, uint64_t now) {
    ctl->round_first_send_ns = 0;
    ctl->prev_repair_bytes = ctl->round_repair;
    ctl->prev_sent_bytes = ctl->round_sent;
    ctl->prev_round_ns = ctl->round_start_ns != 0 && now > ctl->round_start_ns ? now - ctl->round_start_ns : 0;
    ctl->round_start_ns = now;
    ctl->round_delivered = ctl->delivered;
    ctl->round_sent = 0;
    ctl->round_repair = 0;
    ctl->round_units0 = ctl->units_sent;
}

// Quiet for AP_IDLE_RESTART_NS: the remembered rate may be stale. The send starts again from it, keeping the filters
// (the max filter ages in active rounds, so idle time does not empty it). With a remembered rate it starts there in
// CRUISE and probes upward after its first round (a link that got slower meanwhile is caught by the queue guard);
// without one it starts in START.
static inline void ap_restart_after_idle(struct ap_pacer* ctl, uint64_t now) {
    if (ctl->state == AP_ANCHOR) {
        ctl->anchor_prev_bps = 0;
        ap_round_begin(ctl, now);
        return; // the prior holds through a quiet spell
    }
    uint64_t bandwidth = ap_btlbw_bps(ctl);
    ctl->state = bandwidth != 0 ? AP_CRUISE : AP_START;
    ctl->stalls = 0;
    ctl->prev_round_bps = 0;
    ctl->phase = AP_PH_CRUISE;
    ctl->draining = false;
    ctl->rounds_since_probe = AP_PROBE_EVERY_ROUNDS - 1U;
    ap_round_begin(ctl, now);
    ctl->rate_bps = ap_clamp_rate(ctl, bandwidth != 0 ? bandwidth : ap_start_rate_bps(ctl));
}

// The caller sent `bytes` (a repair or new data) at `now`.
static inline void ap_restart_search(struct ap_pacer* ctl, uint64_t now);

// No ACKNACK at all, for a while, with units in flight: the path is dead or the estimate far too high. Any ACKNACK that
// arrives is proof of a live path and restarts the clock (ap_on_feedback), however little it shows delivered - a round
// whose reports said nothing new used to be read as a stall though the reports were coming (Pi v7: 19 stall restarts in
// 7 runs of 1 MiB S2). Repairs sent in the last two round trips are waiting for their answer, not a stall.
static inline void ap_check_stall(struct ap_pacer* ctl, uint64_t now) {
    if (ctl->stall_start_ns == 0 || ctl->units_sent - ctl->held_units < AP_STALL_MIN_UNITS) {
        return;
    }
    uint64_t rtprop = ap_rtprop_ns(ctl);
    uint64_t window = ap_max_u64(AP_STALL_NS, AP_STALL_RTPROPS * rtprop);
    uint64_t silent = now - ctl->stall_start_ns;
    if (silent < window) {
        return;
    }
    if (ctl->last_repair_ns != 0 && now - ctl->last_repair_ns < 2U * rtprop && silent < 4U * window) {
        return;
    }
    ap_restart_search(ctl, now);
}

static inline void ap_note_send(struct ap_pacer* ctl, uint64_t now, uint64_t bytes, bool repair) {
    if (ctl->last_active_ns != 0 && now - ctl->last_active_ns > AP_IDLE_RESTART_NS) {
        ap_restart_after_idle(ctl, now);
    }
    if (ctl->round_start_ns == 0) {
        ctl->round_start_ns = now;
        ctl->round_delivered = ctl->delivered;
    }
    if (ctl->last_active_ns != 0 && now - ctl->last_active_ns > AP_STALL_QUIET_NS) {
        ctl->stall_start_ns = ctl->units_sent > ctl->held_units ? now : 0; // a quiet spell is no silence of the path
    }
    ctl->last_active_ns = now;
    ctl->round_sent += bytes;
    if (repair) {
        ctl->round_repair += bytes;
        ctl->last_repair_ns = now;
    }
    ap_check_stall(ctl, now);
}

// `bytes` of what was sent in this round were repairs (a retransmission, counted into the bytes sent by
// ap_note_send()).
static inline void ap_note_repair(struct ap_pacer* ctl, uint64_t now, uint64_t bytes) {
    ctl->round_repair += bytes;
    ctl->last_repair_ns = now; // (the send path does not say "repair" to ap_note_send(): this is where it is known)
}

// The units of repairs probably in flight: sent in the last round trip, at the rate of the last two rounds. A reader
// leaves out of the bitmap of its answer the units it named within the last round trip and a half (their repairs are
// on the way), and the count of units held took them as received: at 70 % loss most of a sample was "delivered" the
// moment its repairs were asked, and a link of 4 Mbit/s read 28 (Pi quick test 10, a 30 Mbit/s prior on S2: the prior
// was never seen to be wrong).
static inline uint64_t ap_repairs_in_flight_units(const struct ap_pacer* ctl, uint64_t now) {
    uint64_t elapsed = ctl->round_start_ns != 0 && now > ctl->round_start_ns ? now - ctl->round_start_ns : 0;
    uint64_t span = elapsed + ctl->prev_round_ns;
    if (ctl->state != AP_ANCHOR || span == 0 || ctl->unit_bytes == 0) {
        return 0; // (only on a believed prior, where a wrong one has to be seen: the no-prior search reads as it did)
    }
    uint64_t bytes = ctl->round_repair + ctl->prev_repair_bytes;
    // Only where repairs are a good part of what is sent (a loss storm): there the omitted units are most of the count.
    // Where they are few the count is right within the noise, and taking them off under-read a reader that leaves
    // nothing out (the model's multi-reader runs: BtlBw too low, a rise of 4.5 s instead of 2.7).
    if (bytes < AP_MIN_SAMPLE_BYTES ||
        bytes * AP_PERCENT <= (ctl->round_sent + ctl->prev_sent_bytes) * AP_START_LOSSY_PCT) {
        return 0;
    }
    uint64_t window = ap_min_u64(ap_max_u64(ap_rtprop_ns(ctl), AP_MIN_ROUND_NS), span);
    return bytes * window / span / ctl->unit_bytes;
}

// A data datagram of `units` units (an original send, not a repair) of `wire` bytes left at `now`.
static inline void ap_note_data_sent(struct ap_pacer* ctl, uint64_t now, uint64_t wire, uint32_t units) {
    if (ctl->held_units >= ctl->units_sent) {
        ctl->stall_start_ns = now; // sending into an empty pipe: the stall clock starts here, not at the round start
    }
    if (ctl->round_first_send_ns == 0) {
        ctl->round_first_send_ns = now;
    }
    for (uint32_t unit = 0; unit < units; unit++) {
        ctl->stamp_ns[ctl->units_sent % AP_STAMPS] = now;
        ctl->units_sent++;
    }
    ctl->dgram_bytes = ((ctl->dgram_bytes * (AP_DGRAM_EWMA_DIV - 1U)) + wire) / AP_DGRAM_EWMA_DIV;
}

// A data unit (an original send, not a repair) left at `now`: its send time is kept, so that the round trip of a unit a
// reader later shows received can be read off it - the only round trip sample, since a report cannot be matched to the
// solicitation it answers, and one sent for another reason would read as a round trip shorter than the path.
static inline void ap_note_unit_sent(struct ap_pacer* ctl, uint64_t now) {
    ctl->stamp_ns[ctl->units_sent % AP_STAMPS] = now;
    ctl->units_sent++;
}

// One finished round, as the estimator reads it.
struct ap_round {
    uint64_t sample_bps; // delivered over the round, bits per second
    bool app_limited;    // the pacer sent less than AP_APP_LIMITED_PCT of its allowance
    bool idle;           // it sent nothing
    bool big;            // it delivered a whole sample at once: AP_BIG_ROUND_BYTES or more
    bool quiet;          // it held a quiet stretch: more than AP_MAX_ROUND_RTTS round trips
};

// The rate-drop guard: most of what was sent was a repair, or the link delivers under half of what it did, twice.
// Returns the BtlBw to go on with: the round's own sample after a trip - at least a quarter of what it was, so that one
// noisy round cannot take the estimate to nothing - else `bandwidth`.
static inline uint64_t ap_drop_guard(struct ap_pacer* ctl, const struct ap_round* round, uint64_t bandwidth) {
    if (round->idle) {
        ctl->low_rounds = 0;
        return bandwidth;
    }
    bool drop = ctl->round_repair * AP_PERCENT > ctl->round_sent * AP_DROP_REPAIR_PCT && ctl->state == AP_CRUISE;
    if (ctl->state == AP_START && ctl->round_repair * AP_PERCENT > ctl->round_sent * AP_START_DROP_REPAIR_PCT &&
        ctl->rate_bps > 2U * round->sample_bps && round->sample_bps != 0 && ctl->round_repair >= AP_MIN_SAMPLE_BYTES) {
        // START overshot: the rate is more than twice what the round delivered and the round went on repairs - at least
        // a sample's worth: the tail of a 128 KiB sample is a few kB of repairs and a tiny reading, and ended START at
        // 5-10 Mbit/s of 30 on M2 (Pi quick test 7; 128 KiB M2 p50 86 -> 188 ms) (Pi quick
        // test 6, 1 MiB M2: 180 Mbit/s on a 30 Mbit/s link, 70 % of every sample lost, START held because every round
        // was application-limited). The search is over; BtlBw comes down.
        drop = true;
        ctl->state = AP_CRUISE;
        ctl->stalls = 0;
        ctl->prev_round_bps = 0;
    }
    if (!round->app_limited && bandwidth != 0 && round->sample_bps * 2U < bandwidth) {
        ctl->low_rounds++;
        drop = drop || ctl->low_rounds >= 2U;
    } else {
        ctl->low_rounds = 0;
    }
    if (!drop) {
        return bandwidth;
    }
    ctl->drop_trips++;
    uint64_t lowered = ap_max_u64(round->sample_bps, bandwidth / AP_BW_RESET_FRACTION);
    ap_filter_reset(&ctl->bw, lowered, ctl->rounds);
    ctl->low_rounds = 0;
    ctl->phase = AP_PH_CRUISE;
    ctl->rounds_since_probe = 0;
    return lowered;
}

// Search again from the start rate: nothing is known of the link.
static inline void ap_restart_search(struct ap_pacer* ctl, uint64_t now) {
    ctl->stall_restarts++;
    ctl->stall_start_ns = 0;
    if (ctl->state == AP_ANCHOR) {
        // Silence on a believed prior: the path was down. Back to the floor of the prior and up again as the queue
        // allows.
        ctl->anchor_low = 0;
        ctl->anchor_prev_bps = 0;
        ctl->rate_bps = ap_clamp_rate(ctl, 0);
        ap_round_begin(ctl, now);
        return;
    }
    uint64_t resume = ap_max_u64(ctl->last_cruise_bps, ap_btlbw_bps(ctl));
    if (resume != 0) {
        // A search that found the link once does not start over from the bottom: it resumes at half what the link last
        // gave, with gentle probes (the doubling START of every restart overshot, and the next restart followed).
        resume = ap_clamp_rate(ctl, resume / 2U);
        ap_filter_reset(&ctl->bw, resume, ctl->rounds);
        ctl->state = AP_CRUISE;
        ctl->stalls = 0;
        ctl->prev_round_bps = 0;
        ctl->phase = AP_PH_CRUISE;
        ctl->draining = false;
        ctl->low_rounds = 0;
        ctl->queue_over = 0;
        ctl->rounds_since_probe = 0;
        ctl->damp_probe_rounds = AP_DAMPED_PROBE_ROUNDS;
        ctl->rate_bps = resume;
        ap_round_begin(ctl, now);
        return;
    }
    uint64_t quarter = ap_btlbw_bps(ctl) / 4U; // what was known is not forgotten: START resumes from a quarter of it
    ap_filter_reset(&ctl->bw, quarter, ctl->rounds);
    ctl->state = AP_START;
    ctl->stalls = 0;
    ctl->prev_round_bps = 0;
    ctl->phase = AP_PH_CRUISE;
    ctl->draining = false;
    ctl->low_rounds = 0;
    ctl->queue_over = 0;
    ctl->rate_bps = ap_clamp_rate(ctl, ap_max_u64(ap_start_rate_bps(ctl), quarter));
    ap_round_begin(ctl, now);
}

// START: the rate doubles while the delivery rate still grows, and BtlBw is found when it stops growing.
static inline void ap_start_round(struct ap_pacer* ctl, const struct ap_round* round, uint64_t bandwidth) {
    if (round->idle) {
        return;
    }
    if (round->app_limited) {
        // Nothing to send, or less than the rate allows: no evidence about the link. The rate is held - it used to
        // double here, and between samples (control traffic only) ran to the cap in a handful of rounds (Pi quick test
        // 4: 81 of 84 rounds at more than 8 x the link were app-limited), so the next sample and its repairs went out
        // at 1 Gbit/s.
        return;
    }
    bool grew = ctl->prev_round_bps == 0 || round->sample_bps * AP_PERCENT >= ctl->prev_round_bps * AP_START_GROWTH_PCT;
    // A round that went on repairs (a quarter of its bytes, at least a sample's worth) is the link losing what is sent
    // at this rate, whatever its delivery reading says: the readings are lumpy (the watermark waits at a hole and
    // passes the units held above it when the repair arrives - 23 Mbit/s read on S2's 4 Mbit/s link) and doubled on
    // them to 46 Mbit/s.
    bool lossy = ctl->round_repair >= AP_MIN_SAMPLE_BYTES &&
                 ctl->round_repair * AP_PERCENT > ctl->round_sent * AP_START_LOSSY_PCT;
    grew = grew && !lossy;
    ctl->stalls = grew ? 0U : ctl->stalls + 1U;
    ctl->prev_round_bps = round->sample_bps;
    if (ctl->stalls >= AP_START_STALL_ROUNDS) {
        ctl->state = AP_CRUISE;
        ctl->rate_bps = ap_clamp_rate(ctl, bandwidth);
        ctl->rounds_since_probe = 0;
    } else if (ctl->stalls != 0) {
        // The first round that did not grow: the link's rate is about here. Hold at the best round so far, not another
        // doubling (three doublings past a 4 Mbit/s link are 32 Mbit/s of loss).
        uint64_t hold = bandwidth * AP_START_HOLD_PCT / AP_PERCENT;
        ctl->rate_bps = ap_clamp_rate(ctl, lossy ? ap_min_u64(hold, ctl->rate_bps) : hold);
    } else {
        uint64_t doubled = ctl->rate_bps * 2U;
        if (bandwidth != 0) {
            doubled = ap_min_u64(doubled,
                                 bandwidth * AP_START_CEIL_PCT / AP_PERCENT); // never past twice the best delivery seen
        }
        ctl->rate_bps = ap_clamp_rate(ctl, doubled);
    }
}

// CRUISE: BtlBw, a probe round now and then, and a drain round after a probe that found nothing.
static inline void ap_cruise_round(struct ap_pacer* ctl, const struct ap_round* round, uint64_t bandwidth) {
    if (ctl->draining && ++ctl->drain_rounds >= AP_DRAIN_MAX_ROUNDS) {
        ctl->draining = false;
    }
    ctl->rounds_since_probe++;
    if (ctl->phase == AP_PH_PROBE) {
        // A probe that raised the delivery rate is followed by another at once (the link got faster); one that did
        // not is followed by a drain round for whatever queue it made. A round that was application-limited - the
        // tail of a small sample, or the quiet stretch before the next one - has no say in a probe that found
        // nothing: it read what the application gave it, not what the link takes. It used to, and a 128 KiB sample
        // every 4 s lost every probe at the quiet round after it (the rate fell to 0.75 BtlBw, so the run began again
        // at each sample: 36 s to follow a step up, Pi rate-step run).
        if (round->sample_bps * AP_PERCENT >= ctl->probe_base_bps * AP_PROBE_RISE_PCT) {
            ctl->probe_base_bps = bandwidth;
            ctl->probe_gain_pct = ctl->damp_probe_rounds != 0 ? AP_PROBE_GAIN_PCT : AP_PROBE_RUN_GAIN_PCT;
            ctl->probe_carried = 0;
        } else if (round->app_limited && round->quiet && ctl->probe_carried < AP_PROBE_CARRY_ROUNDS) {
            ctl->probe_carried++; // carried over: the next full round judges it
        } else {
            ctl->phase = AP_PH_DRAIN;
            ctl->probe_gain_pct = AP_PROBE_GAIN_PCT;
            ctl->probe_carried = 0;
        }
    } else if (ctl->phase == AP_PH_DRAIN) {
        ctl->phase = AP_PH_CRUISE;
        ctl->rounds_since_probe = 0;
    } else {
        // A probe is due every AP_PROBE_EVERY_ROUNDS rounds, and - when a round delivers a whole sample at once, so
        // that eight rounds are eight samples - after AP_PROBE_EVERY_NS (1 MiB at 4.3 Mbit/s: one round a sample, 32 s
        // between probes, and none at all while every round counted as application-limited).
        bool due = ctl->rounds_since_probe >= AP_PROBE_EVERY_ROUNDS ||
                   (round->big && ctl->rounds_since_probe >= 2U &&
                    ctl->round_end_ns - ctl->last_probe_ns >= AP_PROBE_EVERY_NS);
        if (due && !ctl->draining && !round->app_limited && ctl->round_end_ns >= ctl->train_fresh_until_ns) {
            ctl->phase = AP_PH_PROBE;
            ctl->probe_base_bps = bandwidth;
            ctl->probe_gain_pct = ctl->damp_probe_rounds != 0 ? AP_DAMPED_PROBE_GAIN_PCT : AP_PROBE_GAIN_PCT;
            ctl->probe_carried = 0;
            ctl->last_probe_ns = ctl->round_end_ns;
        }
    }
    if (ctl->damp_probe_rounds != 0) {
        ctl->damp_probe_rounds--;
    }
    if (bandwidth != 0) {
        ctl->last_cruise_bps = bandwidth;
    }
    uint64_t rate = bandwidth;
    if (ctl->draining || ctl->phase == AP_PH_DRAIN) {
        rate = bandwidth * AP_DRAIN_GAIN_PCT / AP_PERCENT;
    } else if (ctl->phase == AP_PH_PROBE) {
        rate = bandwidth * ctl->probe_gain_pct / AP_PERCENT;
    }
    ctl->rate_bps = ap_clamp_rate(ctl, rate);
}

// What the round delivered, in bits per second, over `elapsed` ns.
static inline uint64_t ap_delivery_rate_bps(const struct ap_pacer* ctl, uint64_t elapsed) {
    uint64_t bytes = ctl->delivered - ctl->round_delivered;
    return elapsed != 0 ? (bytes * AP_BITS_PER_BYTE * AP_NS_PER_SECOND) / elapsed : 0;
}

// The queue guard, at each round trip sample: the least of the latest AP_RECENT_RTTS samples over the least ever seen
// is the queue. (One sample is not read: a unit shown delivered by a repair that closed a gap was sent long before it
// was shown, and reads as a long round trip; the least of a few does not.) Over the target AP_QUEUE_TRIPS evaluations
// in a row: the real-time term - stop probing, take the rate the link is delivering now instead of the older maximum,
// and drain below it.
static inline void ap_queue_guard(struct ap_pacer* ctl, uint64_t now) {
    if (ctl->recent_n < AP_RECENT_RTTS) {
        return;
    }
    uint64_t least_recent = UINT64_MAX;
    for (uint32_t idx = 0; idx < AP_RECENT_RTTS; idx++) {
        least_recent = ap_min_u64(least_recent, ctl->recent_rtt[idx]);
    }
    uint64_t floor_ns = ap_rtprop_ns(ctl);
    uint64_t target_ns = ap_queue_target_ns(ctl);
    uint64_t queue_ns = least_recent > floor_ns ? least_recent - floor_ns : 0;
    if (queue_ns <= target_ns) {
        ctl->queue_over = 0;
        if (queue_ns < target_ns / 2U) {
            ctl->draining = false; // the queue is back under half its target
        }
        return;
    }
    ctl->queue_over++;
    if (ctl->queue_over < AP_QUEUE_TRIPS) {
        return;
    }
    ctl->queue_trips++;
    ctl->queue_over = 0;
    if (ctl->state == AP_ANCHOR) {
        ctl->anchor_qtrip =
            true; // a queue on a believed prior: ap_anchor_round() decides, when it lasts into a second round
        return;
    }
    ctl->phase = AP_PH_CRUISE;
    ctl->draining = true;
    ctl->drain_rounds = 0;
    if (ctl->state == AP_START) {
        ctl->state = AP_CRUISE;
    }
    uint64_t elapsed = now - ctl->round_start_ns;
    uint64_t sample = ap_delivery_rate_bps(ctl, elapsed);
    uint64_t bandwidth = ap_btlbw_bps(ctl);
    if (sample != 0 && sample < bandwidth && elapsed >= ap_rtprop_ns(ctl) / 2U) {
        ap_filter_reset(&ctl->bw, ap_max_u64(sample, bandwidth / AP_BW_RESET_FRACTION), ctl->rounds);
        bandwidth = ap_btlbw_bps(ctl);
    }
    ctl->rate_bps = ap_clamp_rate(ctl, (bandwidth != 0 ? bandwidth : ctl->rate_bps) * AP_DRAIN_GAIN_PCT / AP_PERCENT);
    ctl->rounds_since_probe = 0;
}

// The prior is wrong: the link carries less than was said (or more congestion than a prior allows for). The measured
// delivery is BtlBw, the search goes on from it as CRUISE under the ceiling of the prior, with gentle probes.
static inline void ap_anchor_wrong(struct ap_pacer* ctl, uint64_t measured) {
    measured = ap_max_u64(measured, ctl->min_bps);
    ctl->state = AP_CRUISE;
    ctl->anchor_low = 0;
    ap_filter_reset(&ctl->bw, measured, ctl->rounds);
    ctl->phase = AP_PH_CRUISE;
    ctl->draining = false;
    ctl->low_rounds = 0;
    ctl->queue_over = 0;
    ctl->rounds_since_probe = 0;
    ctl->damp_probe_rounds = AP_DAMPED_PROBE_ROUNDS;
    ctl->prev_round_bps = 0;
    ctl->rate_bps = ap_clamp_rate(ctl, measured * AP_ANCHOR_STEP_PCT / AP_PERCENT);
}

// One round on a believed prior. The rate is 95 % of it and stays, with three exceptions, none of them loss alone (loss
// on a wireless link is not congestion, and the repairs have their own budget): a queue (the queue guard, above) and
// persistently low delivery step the rate down by a fifth, and it recovers by a tenth a round while the queue is clean;
// a prior that is wrong - two low rounds among the first few, a loss storm (repairs above half the bytes at more than
// twice the delivery), or the floor reached with the congestion still there - steps down to what was measured.
static inline void ap_anchor_round(struct ap_pacer* ctl, const struct ap_round* round, uint64_t elapsed) {
    if (round->idle) {
        return;
    }
    if (round->app_limited) {
        ctl->anchor_prev_bps = 0; // nothing learned of the link, and the next round has nothing to compare with
        return;
    }
    ctl->anchor_qrounds = ctl->anchor_qtrip ? ctl->anchor_qrounds + 1U : 0U;
    ctl->anchor_qtrip = false;
    uint64_t sample = round->sample_bps;
    ctl->anchor_ewma_bps = ctl->anchor_ewma_bps == 0 ? sample : (3U * ctl->anchor_ewma_bps + sample) / 4U;
    ctl->anchor_rounds++;
    // A round delivers what the one before it sent (a round trip lags): low is under 60 % of that.
    uint64_t prev = ctl->anchor_prev_bps;
    ctl->anchor_prev_bps = ctl->round_sent * AP_BITS_PER_BYTE * AP_NS_PER_SECOND / ap_max_u64(elapsed, 1U);
    bool low = prev != 0 && sample * AP_PERCENT < prev * AP_PRIOR_VERIFY_PCT;
    ctl->anchor_low = low ? ctl->anchor_low + 1U : 0U;
    bool storm = ctl->round_repair >= AP_MIN_SAMPLE_BYTES &&
                 ctl->round_repair * AP_PERCENT > ctl->round_sent * AP_START_DROP_REPAIR_PCT && sample != 0 &&
                 ctl->rate_bps > 2U * sample;
    uint64_t floor_bps = ctl->prior_bps * AP_PRIOR_FLOOR_PCT / AP_PERCENT;
    bool at_floor = ctl->rate_bps <= floor_bps;
    if (storm || (low && ctl->anchor_low >= 2U && ctl->anchor_rounds <= AP_ANCHOR_VERIFY_ROUNDS) ||
        (at_floor && low && ctl->anchor_low >= 2U)) {
        ap_anchor_wrong(ctl, storm ? ap_min_u64(sample, ctl->anchor_ewma_bps) : ctl->anchor_ewma_bps);
    } else if (ctl->anchor_low >= AP_ANCHOR_DELIVERY_ROUNDS || ctl->anchor_qrounds >= AP_ANCHOR_QUEUE_ROUNDS) {
        ctl->rate_bps = ap_clamp_rate(ctl, ctl->rate_bps * AP_ANCHOR_DECREASE_PCT / AP_PERCENT);
    } else if (!low && ctl->anchor_qrounds == 0 && ctl->rate_bps < ctl->cap_bps) {
        ctl->rate_bps = ap_clamp_rate(ctl, ctl->rate_bps * AP_ANCHOR_RECOVER_PCT / AP_PERCENT);
    }
}

// The packet train (adaptive_pacing/DESIGN.md 28). A train is the first K datagrams of a large sample sent back to
// back; the reader reports the span between the first and the last to arrive, so bytes of the K - 1 after the first
// over that span is what the path carried them at: the capacity when the path was the narrowest part, and what a
// burst-forming link (slots, aggregation) released in a burst when not. Samples are taken, per round (one per large
// sample: every reader's train of it), as the least over the readers; three rounds are kept.
//
//  - implausible samples (no span, one the clock cannot resolve, above 1.25 x the cap or 16 x the last estimate) are
//  not read;
//  - a sample below the last estimate is taken at once (an overestimate is the dangerous one: a queue); one above it is
//  taken
//    up to 1.5 x the last estimate, and past that only as far as the median of the last three rounds agrees (two of
//    three);
//  - when the guards trip (queue or drop, twice) in the sample after one was applied, the train read too high - the
//    burst of a slotted link: after two such samples in a row rises are not taken for AP_TRAIN_BLOCK_SAMPLES samples,
//    and the rate is what the estimator finds by itself;
//  - the estimate becomes BtlBw at AP_TRAIN_RATE_PCT of it and the state CRUISE (a train ends a START search), and
//  probes
//    wait AP_TRAIN_FRESH_NS (the train has just said what they would find). Everything else - the queue guard, the drop
//    guard, the stall restart - runs as before and can lower it again; without trains nothing here ever runs.
static inline uint64_t ap_train_median(const struct ap_pacer* ctl) {
    const uint64_t* hist = ctl->train_hist;
    if (ctl->train_hist_n >= 3U) {
        uint64_t low = ap_min_u64(hist[0], hist[1]);
        uint64_t high = ap_max_u64(hist[0], hist[1]);
        return ap_max_u64(low, ap_min_u64(high, hist[2]));
    }
    return ctl->train_hist_n == 2U ? ap_min_u64(hist[0], hist[1]) : hist[0];
}

static inline void ap_train_apply(struct ap_pacer* ctl, uint64_t now, uint64_t estimate) {
    uint64_t bandwidth = ap_clamp_rate(ctl, estimate * AP_TRAIN_RATE_PCT / AP_PERCENT);
    ap_filter_reset(&ctl->bw, bandwidth, ctl->rounds);
    ctl->state = AP_CRUISE;
    ctl->phase = AP_PH_CRUISE;
    ctl->draining = false;
    ctl->drain_rounds = 0;
    ctl->queue_over = 0;
    ctl->low_rounds = 0;
    ctl->stalls = 0;
    ctl->prev_round_bps = 0;
    ctl->rounds_since_probe = 0;
    ctl->probe_carried = 0;
    ctl->last_cruise_bps = bandwidth;
    ctl->rate_bps = bandwidth;
    ctl->train_est_bps = estimate;
    ctl->train_trips0 = ctl->queue_trips + ctl->drop_trips;
    ctl->train_fresh_until_ns = now + AP_TRAIN_FRESH_NS;
    ctl->train_applied++;
    ap_round_begin(ctl, now);
}

// Whether a sample is a measurement at all.
static inline bool ap_train_plausible(const struct ap_pacer* ctl, uint64_t sample_bps, uint64_t span_ns) {
    uint64_t line = ctl->cap_bps != 0 && ctl->cap_bps < AP_CAP_UNSET_BPS ? ctl->cap_bps : AP_PRIOR_MAX_BPS;
    if (span_ns < AP_TRAIN_MIN_SPAN_NS || span_ns > AP_TRAIN_MAX_SPAN_NS || sample_bps == 0) {
        return false;
    }
    return sample_bps <= line * AP_TRAIN_LINE_PCT / AP_PERCENT &&
           (ctl->train_est_bps == 0 || sample_bps <= ctl->train_est_bps * AP_TRAIN_MAX_JUMP_X);
}

// A new round (the first reader's train of a large sample): did the rate the last train gave hold - guards that tripped
// since say it did not - and the sample joins the three kept.
static inline void ap_train_new_round(struct ap_pacer* ctl, uint32_t round, uint64_t sample_bps) {
    if (ctl->train_est_bps != 0) {
        uint64_t trips = ctl->queue_trips + ctl->drop_trips;
        if (ctl->train_block != 0) {
            ctl->train_block--;
        } else {
            ctl->train_bad = trips - ctl->train_trips0 >= AP_TRAIN_BAD_TRIPS ? ctl->train_bad + 1U : 0U;
            if (ctl->train_bad >= AP_TRAIN_BAD_SAMPLES) {
                ctl->train_block = AP_TRAIN_BLOCK_SAMPLES;
                ctl->train_bad = 0;
            }
        }
    }
    if (ctl->train_hist_n == AP_TRAIN_HIST) {
        for (uint32_t i = 1; i < AP_TRAIN_HIST; i++) {
            ctl->train_hist[i - 1U] = ctl->train_hist[i];
        }
        ctl->train_hist_n--;
    }
    ctl->train_hist[ctl->train_hist_n++] = sample_bps;
    ctl->train_round = round;
}

// The estimate to take from this round's value: lower at once, higher by at most 1.5 x or as far as the median of three
// agrees.
static inline uint64_t ap_train_choose(const struct ap_pacer* ctl, uint64_t candidate) {
    uint64_t prev = ctl->train_est_bps;
    if (prev == 0 || candidate <= prev) {
        return candidate;
    }
    return ap_min_u64(candidate, ap_max_u64(prev * AP_TRAIN_RISE_PCT / AP_PERCENT, ap_train_median(ctl)));
}

// One reader's train of `round`: `sample_bps` on the wire over a span of `span_ns`. True when it was read.
static inline bool ap_train_sample(struct ap_pacer* ctl, uint64_t now, uint32_t round, uint64_t sample_bps,
                                   uint64_t span_ns) {
    ctl->train_samples++;
    if (!ap_train_plausible(ctl, sample_bps, span_ns)) {
        ctl->train_rejected++;
        return false;
    }
    if (ctl->state == AP_ANCHOR) {
        // The user's prior is believed: a train only says when it is wrong (a link a good deal below it).
        if (sample_bps * AP_PERCENT < ctl->prior_bps * AP_PRIOR_VERIFY_PCT) {
            ap_anchor_wrong(ctl, sample_bps);
            ctl->train_applied++;
        }
        return true;
    }
    if (ctl->train_hist_n == 0 || round != ctl->train_round) {
        ap_train_new_round(ctl, round, sample_bps);
    } else {
        uint64_t* newest = &ctl->train_hist[ctl->train_hist_n - 1U];
        *newest = ap_min_u64(*newest, sample_bps); // the least over the round's readers
    }
    uint64_t candidate = ctl->train_hist[ctl->train_hist_n - 1U];
    if (ctl->train_block != 0) {
        // The trains have read too high (a burst-forming link): what they say is read only to go lower than the
        // estimator's own bandwidth, which it finds without them; the estimate they gave is forgotten for what it
        // found.
        uint64_t own = ap_btlbw_bps(ctl) * AP_PERCENT / AP_TRAIN_RATE_PCT;
        ctl->train_est_bps = own;
        if (candidate < own) {
            ap_train_apply(ctl, now, candidate);
        }
        return true;
    }
    ap_train_apply(ctl, now, ap_train_choose(ctl, candidate));
    return true;
}

static inline void ap_end_round(struct ap_pacer* ctl, uint64_t now) {
    uint64_t elapsed = now - ctl->round_start_ns;
    // A round that delivered a whole sample at once (the reports showed no progress until the last repair: one round
    // per sample, 1 MiB at 4.3 Mbit/s, Pi rate-step run) holds the gap before the sample as well: read over the time
    // the sample was being sent, or its rate is diluted by the gap, it is always "application-limited", no probe can
    // begin and BtlBw stays where it was for ever after the link rises.
    uint64_t span = elapsed;
    if (ctl->state != AP_ANCHOR && ctl->delivered - ctl->round_delivered >= AP_BIG_ROUND_BYTES &&
        ctl->round_first_send_ns > ctl->round_start_ns && now > ctl->round_first_send_ns &&
        now - ctl->round_first_send_ns >= AP_MIN_ROUND_NS) {
        span = now - ctl->round_first_send_ns;
    }
    ctl->round_end_ns = now;
    uint64_t allowance = (ctl->rate_bps / AP_BITS_PER_BYTE) * span / AP_NS_PER_SECOND;
    uint64_t round_bps = ap_delivery_rate_bps(ctl, span);
    struct ap_round round = {.sample_bps = ap_min_u64(round_bps, ctl->rate_bps * AP_SAMPLE_CAP_PCT / AP_PERCENT),
                             .app_limited = ctl->round_sent * AP_PERCENT < allowance * AP_APP_LIMITED_PCT,
                             .idle = ctl->round_sent == 0,
                             .big = ctl->delivered - ctl->round_delivered >= AP_BIG_ROUND_BYTES,
                             .quiet = elapsed > AP_MAX_ROUND_RTTS * ap_max_u64(ap_rtprop_ns(ctl), AP_MIN_ROUND_NS)};
    uint64_t bandwidth = ap_btlbw_bps(ctl);
    ctl->samples++;
    bool quiet = round.quiet; // a gap between samples
    if (!round.idle && (!round.app_limited || round.sample_bps > bandwidth || !quiet)) {
        // The quiet round between two samples (nothing to send, little read) does not age the filter: it did, and a
        // 128 KiB sample every 4 s on S1 left BtlBw 0 in 10 rounds (Pi quick test 6).
        ctl->rounds++;
        ap_filter_put(&ctl->bw, round.sample_bps, ctl->rounds);
        bandwidth = ap_btlbw_bps(ctl);
    }
#ifdef tt_RATE_TRACE
    // One line a round, for a run that has to be read afterwards (the bench build for the Pis sets tt_RATE_TRACE): what
    // the round delivered and sent, the estimates it ended with, and the counts the round trip and the guards stand on.
    fprintf(
        stderr,
        "RATETRACE t_ms=%llu el_ms=%.1f bytes=%llu sample_kbit=%llu sent=%llu allow=%llu app=%d idle=%d bw_kbit=%llu "
        "rate_kbit=%llu st=%d ph=%d rtprop_us=%llu rtt_last_us=%llu held=%llu units_sent=%llu jump=%llu "
        "jump_events=%llu "
        "qover=%u drain=%d\n",
        (unsigned long long)(now / 1000000ULL), (double)elapsed / 1e6,
        (unsigned long long)(ctl->delivered - ctl->round_delivered), (unsigned long long)(round.sample_bps / 1000ULL),
        (unsigned long long)ctl->round_sent, (unsigned long long)allowance, (int)round.app_limited, (int)round.idle,
        (unsigned long long)(bandwidth / 1000ULL), (unsigned long long)(ctl->rate_bps / 1000ULL), (int)ctl->state,
        (int)ctl->phase, (unsigned long long)(ap_rtprop_ns(ctl) / 1000ULL),
        (unsigned long long)(ctl->recent_rtt[(ctl->recent_n + AP_RECENT_RTTS - 1U) % AP_RECENT_RTTS] / 1000ULL),
        (unsigned long long)ctl->held_units, (unsigned long long)ctl->units_sent, (unsigned long long)ctl->jump_units,
        (unsigned long long)ctl->jump_events, (unsigned)ctl->queue_over, (int)ctl->draining);
#endif
    if (ctl->state == AP_ANCHOR) {
        ap_anchor_round(ctl, &round, elapsed);
        ap_round_begin(ctl, now);
        return;
    }
    bandwidth = ap_drop_guard(ctl, &round, bandwidth);
    if (ctl->state == AP_START) {
        ap_start_round(ctl, &round, bandwidth);
    } else if (bandwidth == 0 && !round.idle) {
        ap_restart_search(ctl, now); // CRUISE on an estimate of nothing: never sit at the floor
        return;
    } else {
        ap_cruise_round(ctl, &round, bandwidth);
    }
    ap_round_begin(ctl, now);
}

// A round trip sample from the newest unit the watermark shows received (pooled over the readers): the report cannot
// have been sent before that unit arrived, so now less the unit's send time is never less than the path's round trip.
// Only the watermark is read - a bitmap names units missing that are still in flight (a solicitation's HEARTBEAT tells
// the reader how far the writer has sent), so a count that includes them reads a unit sent a moment ago as received and
// a round trip of microseconds (the Pi stage: 5-12 us on 10 and 40 ms paths). And only where the watermark moved by a
// few units: a repair that closes a gap moves it over units sent long before (Karn's rule: no sample from a unit that
// was repaired).
static inline void ap_note_held_units(struct ap_pacer* ctl, uint64_t now, uint64_t held, uint64_t advance) {
    uint64_t dgram_units = ctl->unit_bytes != 0 ? (ctl->dgram_bytes + ctl->unit_bytes - 1U) / ctl->unit_bytes : 1U;
    // The watermark reaching the last unit sent is a sample whatever its advance: the path is empty then, so the unit
    // was not queued behind others. Without it no sample came where ACKs advanced the watermark by more than a few
    // units (S1 128 KiB, Pi quick test 6): the first one, taken in a queue the START rate had built (152 ms on a 10 ms
    // path), stayed RTprop for the run.
    bool tail = held == ctl->units_sent && held != 0;
    if (advance == 0 || (advance > ap_max_u64(AP_RTT_MIN_ADVANCE_UNITS, 2U * dgram_units) && !tail)) {
        return;
    }
    uint64_t idx = held - 1U;
    if (idx >= ctl->units_sent || ctl->units_sent - idx > AP_STAMPS) {
        return; // a unit that was not sent as one (a small sample's), or too long ago to have a stamp
    }
    uint64_t sent_ns = ctl->stamp_ns[idx % AP_STAMPS];
    if (now <= sent_ns) {
        return;
    }
    uint64_t rtt_ns = now - sent_ns;
    if (rtt_ns > AP_RTT_MAX_NS || (ctl->rtt_samples >= 3U && rtt_ns > AP_RTT_OUTLIER_X * ap_rtprop_ns(ctl))) {
        return; // a unit repaired or held in a queue: Pi quick test 4 took a 2.1 s sample and moved rtprop from 42 to
                // 209 ms
    }
    ap_filter_put(&ctl->rtt, rtt_ns, now);
    ctl->rtt_samples++;
    if (advance > ap_max_u64(AP_RTT_MIN_ADVANCE_UNITS, 2U * dgram_units) && ctl->last_repair_ns != 0 &&
        now - ctl->last_repair_ns < 2U * ap_rtprop_ns(ctl)) {
        return; // a tail sample after a recovery (a repair in the last two round trips): the time of the repair,
                // RTprop's to take but not the queue guard's - on S2 it tripped the guard 18 times with no queue (Pi
                // quick test 10)
    }
    ctl->recent_rtt[ctl->recent_n % AP_RECENT_RTTS] = rtt_ns;
    ctl->recent_n++;
    ap_queue_guard(ctl, now);
}

// Feedback at `now`, pooled over the link's readers (cumulative, in units): `held_units` is what the readers' ACKNACKs
// show held - the watermark and the units inside its bitmap's window that are not named missing - a smooth count of
// delivery, `mark_units` the watermarks alone, every unit below one received for certain. Delivery reads the first (the
// watermark alone moves in jumps at each repair's arrival and the rate estimate followed them); the round trip reads
// the second (a bitmap names units missing that are still in flight, so a count with them reads a unit sent a moment
// ago as received). A watermark can move over units nobody held - a sample the writer evicted or the reader gave up,
// its units passed at once - and a count past what was sent cannot be delivery: the excess is taken as such a jump and
// left out for good, or the estimate reads a megabyte delivered in an instant, runs to its cap and loses it.
static inline void ap_on_feedback(struct ap_pacer* ctl, uint64_t now, uint64_t held_units, uint64_t mark_units) {
    uint64_t held = held_units > ctl->jump_units ? held_units - ctl->jump_units : 0;
    if (held > ctl->units_sent) {
        ctl->jump_units += held - ctl->units_sent;
        ctl->jump_events++;
        held = ctl->units_sent;
    }
    uint64_t mark = mark_units > ctl->jump_units ? mark_units - ctl->jump_units : 0;
    uint64_t in_flight = ap_repairs_in_flight_units(ctl, now);
    held = held > in_flight ? held - in_flight : 0;
    mark = ap_min_u64(mark, held);
    if (held > ctl->held_units) {
        ctl->held_units = held;
        ctl->delivered = held * ctl->unit_bytes;
    }
    ctl->stall_start_ns = ctl->units_sent > ctl->held_units ? now : 0; // any report is a live path
    uint64_t advance = mark > ctl->mark_units ? mark - ctl->mark_units : 0;
    if (advance != 0) {
        ctl->mark_units = mark;
    }
    if (ctl->round_start_ns == 0) {
        ctl->round_start_ns = now;
        ctl->round_delivered = ctl->delivered;
    }
    ap_note_held_units(ctl, now, mark, advance);
    uint64_t elapsed = now - ctl->round_start_ns;
    uint64_t rtprop = ap_rtprop_ns(ctl);
    uint64_t least_ns = ap_max_u64(rtprop, AP_MIN_ROUND_NS);
    uint64_t bytes = ctl->delivered - ctl->round_delivered;
    if (bytes == 0) {
        // Nothing delivered: no sample - a round with no news says nothing of the link, and is no stall: the report
        // that brought it is a live path (ap_check_stall() counts the silence).
        return;
    }
    if (elapsed >= least_ns && (bytes >= AP_MIN_SAMPLE_BYTES || elapsed >= AP_MAX_ROUND_RTTS * least_ns)) {
        ap_end_round(ctl, now);
    }
}

#endif // TT_PACING_ADAPTIVE_H

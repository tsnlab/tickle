/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The adaptive pacer's estimator (src/pacing_adaptive.h) against a synthetic link: a FIFO queue with a byte limit
// behind a rate that changes on a schedule, a one-way delay each way, a reader that reports what arrived and what is
// missing, and a writer that repairs what is missing. A MODEL: no TickLE code runs here but the estimator, and
// nothing below is a performance claim about a real link - the thresholds are what the design needs of the
// estimator on this idealised link (convergence, a short queue, fewer retransmissions than an unpaced send), and
// examples/perf_hil/experiments/adaptive_pacing/DESIGN.md section 7 holds the acceptance criteria for the real thing.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/pacing_adaptive.h"

#define UNIT_WIRE 1538U   // a full frame on the wire: 1472 payload + UDP, IP and Ethernet framing
#define SAMPLE_UNITS 723U // 1 MiB in 1472-byte units
#define SOLICIT_WIRE 90U
#define STEP_NS 100000ULL // the model's clock step
#define MAX_Q 4096U
#define MAX_DELAY 8192U
#define MAX_UNITS 200000U
#define MS 1000000ULL

enum mode { MODE_UNPACED, MODE_FIXED, MODE_ADAPTIVE };

struct pkt {
    uint64_t enq_ns;
    uint32_t wire;
    int64_t unit;        // unit id, -1 for a solicit
    uint64_t solicit_ns; // a solicit's send time
    int64_t last_unit; // a solicit's highest sent unit: its HEARTBEAT last_seq_no, how the reader learns of a lost tail
};

struct flight {
    uint64_t at_ns;
    struct pkt pkt;
};

struct link_cfg {
    uint64_t rate_bps[3]; // rate before step 1, between, after step 2
    uint64_t step1_ns;
    uint64_t step2_ns;
    uint32_t queue_bytes;
    uint64_t one_way_ns;
};

struct result {
    uint64_t sent_units;
    uint64_t repair_units;
    uint64_t dropped;
    uint64_t solicits;
    uint32_t samples_done;
    uint64_t completion_ns[64];
    uint64_t queue_delay_max_ns;
    uint64_t queue_delay_sum_ns;
    uint64_t queue_delay_n;
    uint64_t rate_after_step_ok_ns[3]; // time after each step until BtlBw is within 25 % of the link rate; 0 = never
    uint64_t max_rate_bps;
    uint64_t min_rate_bps;
    uint64_t trips_q, trips_d;
};

static double ms(uint64_t ns) {
    return (double)ns / 1e6;
}

static uint64_t link_rate(const struct link_cfg* cfg, uint64_t now) {
    return now < cfg->step1_ns ? cfg->rate_bps[0] : (now < cfg->step2_ns ? cfg->rate_bps[1] : cfg->rate_bps[2]);
}

// One run: `period_ns` 0 = a new sample as soon as the last completes (bulk); else one sample every period_ns.
static void run(const struct link_cfg* cfg, enum mode mode, uint64_t fixed_bps, uint64_t cap_bps, uint64_t min_bps,
                uint64_t period_ns, uint64_t duration_ns, struct result* out) {
    static struct pkt queue[MAX_Q];
    static struct flight fwd[MAX_DELAY];
    static struct flight rev[MAX_DELAY];
    static uint8_t got[MAX_UNITS];
    static uint64_t last_sent[MAX_UNITS];
    static uint8_t want_repair[MAX_UNITS];
    memset(out, 0, sizeof(*out));
    memset(got, 0, sizeof(got));
    memset(last_sent, 0, sizeof(last_sent));
    memset(want_repair, 0, sizeof(want_repair));
    out->min_rate_bps = UINT64_MAX;
    uint32_t qh = 0, qt = 0, qn = 0, qbytes = 0;
    uint32_t fh = 0, ft = 0, fn = 0, rh = 0, rt = 0, rn = 0;
    int64_t credit = 0; // link credit in bits
    struct ap_pacer ap;
    ap_init(&ap, min_bps, cap_bps, 0, 2 * cfg->one_way_ns);

    int64_t sample_first = 0, sample_next = 0, sample_end = 0; // unit ids of the current sample
    bool sampling = false;
    uint64_t sample_started = 0, next_sample_ns = 0;
    uint64_t next_send_ns = 0, next_solicit_ns = 0;
    int64_t highest = -1;
    uint64_t delivered_units = 0;
    uint64_t srtt = 2 * cfg->one_way_ns;
    uint64_t rate_ok_since[3] = {0, 0, 0};
    uint64_t sample_period_start = 0;
    (void)sample_period_start;

    for (uint64_t now = 0; now < duration_ns; now += STEP_NS) {
        // 1. a sample begins
        if (!sampling && now >= next_sample_ns && sample_end + 1 < (int64_t)MAX_UNITS - (int64_t)SAMPLE_UNITS) {
            sample_first = sample_end + (sample_end == 0 ? 0 : 1);
            sample_next = sample_first;
            sample_end = sample_first + (int64_t)SAMPLE_UNITS - 1;
            sampling = true;
            sample_started = now;
            next_sample_ns = period_ns != 0 ? now + period_ns : 0;
        }
        // 2. the writer sends: repairs ahead of new units, at the pacing rate
        uint64_t rate = mode == MODE_UNPACED ? 1000000000ULL : (mode == MODE_FIXED ? fixed_bps : ap_rate_bps(&ap));
        if (rate > out->max_rate_bps) {
            out->max_rate_bps = rate;
        }
        if (mode == MODE_ADAPTIVE && sampling && rate < out->min_rate_bps) {
            out->min_rate_bps = rate;
        }
        while (sampling && now >= next_send_ns) {
            int64_t unit = -1;
            bool repair = false;
            for (int64_t u = sample_first; u < sample_next; u++) {
                if (want_repair[u] && !got[u] && now - last_sent[u] > srtt + srtt / 2U) {
                    unit = u;
                    repair = true;
                    break;
                }
            }
            if (unit < 0 && sample_next <= sample_end) {
                unit = sample_next++;
            }
            if (unit < 0) {
                break;
            }
            if (qn + 1U >= MAX_Q || qbytes + UNIT_WIRE > cfg->queue_bytes) {
                out->dropped++;
            } else {
                queue[qt] = (struct pkt) {now, UNIT_WIRE, unit, 0, 0};
                qt = (qt + 1U) % MAX_Q;
                qn++;
                qbytes += UNIT_WIRE;
            }
            last_sent[unit] = now;
            want_repair[unit] = 0;
            out->sent_units++;
            if (repair) {
                out->repair_units++;
            }
            if (mode == MODE_ADAPTIVE) {
                ap_note_send(&ap, now + 1U, UNIT_WIRE, repair);
            }
            uint64_t gap = ((uint64_t)UNIT_WIRE * 8ULL * 1000000000ULL) / rate;
            next_send_ns = (next_send_ns < now ? now : next_send_ns) + gap;
            if (next_send_ns > now + 400000ULL) { // a send tick of at most 4 steps' work per step
                break;
            }
        }
        // 3. a solicit, once per RTprop (10 ms before a sample of round trips), while a sample is open
        if (sampling && now >= next_solicit_ns) {
            uint64_t rtprop = mode == MODE_ADAPTIVE ? ap_rtprop_ns(&ap) : srtt;
            next_solicit_ns = now + (rtprop < 5000000ULL ? 5000000ULL : rtprop);
            if (qn + 1U < MAX_Q && qbytes + SOLICIT_WIRE <= cfg->queue_bytes) {
                queue[qt] = (struct pkt) {now, SOLICIT_WIRE, -1, now, sample_next - 1};
                qt = (qt + 1U) % MAX_Q;
                qn++;
                qbytes += SOLICIT_WIRE;
                out->solicits++;
            }
        }
        // 4. the link serves its queue at its rate
        credit += (int64_t)(link_rate(cfg, now) * STEP_NS / 1000000000ULL);
        while (qn != 0 && credit >= (int64_t)queue[qh].wire * 8) {
            struct pkt p = queue[qh];
            qh = (qh + 1U) % MAX_Q;
            qn--;
            qbytes -= p.wire;
            credit -= (int64_t)p.wire * 8;
            uint64_t qd = now - p.enq_ns;
            if (qd > out->queue_delay_max_ns) {
                out->queue_delay_max_ns = qd;
            }
            out->queue_delay_sum_ns += qd;
            out->queue_delay_n++;
            fwd[ft] = (struct flight) {now + cfg->one_way_ns, p};
            ft = (ft + 1U) % MAX_DELAY;
            fn++;
        }
        if (qn == 0 && credit > (int64_t)UNIT_WIRE * 8) {
            credit = (int64_t)UNIT_WIRE * 8; // an idle link does not bank a burst
        }
        // 5. the reader receives; a solicit makes it report
        while (fn != 0 && fwd[fh].at_ns <= now) {
            struct pkt p = fwd[fh].pkt;
            fh = (fh + 1U) % MAX_DELAY;
            fn--;
            if (p.unit >= 0) {
                if (!got[p.unit]) {
                    got[p.unit] = 1;
                    delivered_units++;
                }
                if (p.unit > highest) {
                    highest = p.unit;
                }
            } else {
                // the report travels back: the reader's own state when the solicit arrived
                rev[rt] = (struct flight) {now + cfg->one_way_ns,
                                           (struct pkt) {p.solicit_ns, 0, (int64_t)delivered_units,
                                                         (uint64_t)(highest > p.last_unit ? highest : p.last_unit), 0}};
                rt = (rt + 1U) % MAX_DELAY;
                rn++;
            }
        }
        // 6. the writer receives the report
        while (rn != 0 && rev[rh].at_ns <= now) {
            struct flight f = rev[rh];
            rh = (rh + 1U) % MAX_DELAY;
            rn--;
            uint64_t rtt = now - f.pkt.enq_ns;
            srtt = srtt == 0 ? rtt : (srtt * 7U + rtt) / 8U;
            int64_t reported_highest = (int64_t)f.pkt.solicit_ns;
            for (int64_t u = sample_first; u <= reported_highest && u <= sample_end; u++) {
                if (!got[u]) {
                    want_repair[u] = 1; // the reader names what is missing below its highest
                }
            }
            if (mode == MODE_ADAPTIVE) {
                ap_on_feedback(&ap, now, (uint64_t)f.pkt.unit * UNIT_WIRE, rtt);
            }
        }
        // 7. a sample completes
        if (sampling) {
            bool all = true;
            for (int64_t u = sample_first; u <= sample_end && all; u++) {
                all = got[u] != 0;
            }
            if (all) {
                if (out->samples_done < 64U) {
                    out->completion_ns[out->samples_done] = now - sample_started;
                }
                out->samples_done++;
                sampling = false;
                if (period_ns == 0) {
                    next_sample_ns = now;
                }
            }
        }
        if (mode == MODE_ADAPTIVE && getenv("TAP_TRACE") != NULL && now % 500000000ULL < STEP_NS) {
            printf("  t %5.1f s link %5.1f Mbit/s  BtlBw %5.2f rate %5.2f Mbit/s RTprop %5.1f ms state %d phase %d "
                   "drain %d q %u B\n",
                   (double)now / 1e9, (double)link_rate(cfg, now) / 1e6, (double)ap_btlbw_bps(&ap) / 1e6,
                   (double)ap_rate_bps(&ap) / 1e6, ms(ap_rtprop_ns(&ap)), (int)ap.state, (int)ap.phase,
                   (int)ap.draining, qbytes);
        }
        // 8. convergence of the estimate after a step, against the link's own rate (adaptive runs)
        if (mode == MODE_ADAPTIVE && sampling) {
            uint64_t c = link_rate(cfg, now);
            uint64_t bw = ap_btlbw_bps(&ap);
            int phase = now < cfg->step1_ns ? 0 : (now < cfg->step2_ns ? 1 : 2);
            uint64_t phase_start = phase == 0 ? 0 : (phase == 1 ? cfg->step1_ns : cfg->step2_ns);
            if (bw * 4U >= c * 3U && bw * 4U <= c * 5U) {
                if (rate_ok_since[phase] == 0 && out->rate_after_step_ok_ns[phase] == 0) {
                    rate_ok_since[phase] = now - phase_start + 1U;
                    out->rate_after_step_ok_ns[phase] = rate_ok_since[phase];
                }
            }
        }
    }
    out->trips_q = ap.queue_trips;
    out->trips_d = ap.drop_trips;
}

static int failures;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
            failures++;                   \
        }                                 \
    } while (0)

// Repairs as a share of what was sent, in per cent: the unpaced control resends most of what it sends.
static uint64_t repair_share_pct(const struct result* r) {
    return r->sent_units != 0 ? r->repair_units * 100U / r->sent_units : 0U;
}

static void print_result(const char* name, const struct result* r) {
    uint64_t med = 0, worst = 0;
    uint64_t sorted[64];
    uint32_t n = r->samples_done < 64U ? r->samples_done : 64U;
    memcpy(sorted, r->completion_ns, n * sizeof(uint64_t));
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1U; j < n; j++) {
            if (sorted[j] < sorted[i]) {
                uint64_t t = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = t;
            }
        }
    }
    if (n != 0) {
        med = sorted[n / 2U];
        worst = sorted[n - 1U];
    }
    printf("%-28s samples %2u  completion p50 %7.0f ms max %7.0f ms  sent %6llu (repairs %5llu, dropped %5llu)  "
           "queue delay mean %5.1f max %6.1f ms  solicits %llu  trips q%llu d%llu\n",
           name, r->samples_done, ms(med), ms(worst), (unsigned long long)r->sent_units,
           (unsigned long long)r->repair_units, (unsigned long long)r->dropped,
           r->queue_delay_n != 0 ? ms(r->queue_delay_sum_ns / r->queue_delay_n) : 0.0, ms(r->queue_delay_max_ns),
           (unsigned long long)r->solicits, (unsigned long long)r->trips_q, (unsigned long long)r->trips_d);
}

int main(void) {
    struct result unpaced, fixed, adaptive;
    const uint64_t SEC = 1000000000ULL;

    // 1. constant 4 Mbit/s, queue of 40 frames, 10 ms each way, a 1 MiB sample every 4 s for 40 s
    struct link_cfg slow = {{4000000ULL, 4000000ULL, 4000000ULL}, 1000 * SEC, 2000 * SEC, 40U * UNIT_WIRE, 10000000ULL};
    run(&slow, MODE_UNPACED, 0, 0, 0, 4 * SEC, 40 * SEC, &unpaced);
    run(&slow, MODE_FIXED, 3000000ULL, 0, 0, 4 * SEC, 40 * SEC, &fixed);
    run(&slow, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 4 * SEC, 40 * SEC, &adaptive);
    printf("-- 4 Mbit/s, queue 40, RTT 20 ms, 1 MiB every 4 s --\n");
    print_result("unpaced", &unpaced);
    print_result("fixed 3 Mbit/s (oracle)", &fixed);
    print_result("adaptive", &adaptive);
    CHECK(unpaced.dropped > 0, "the unpaced control must overflow the queue");
    CHECK(adaptive.samples_done >= 8U, "adaptive completes the samples: %u", adaptive.samples_done);
    CHECK(adaptive.queue_delay_n != 0 && adaptive.queue_delay_sum_ns / adaptive.queue_delay_n < 10 * MS,
          "the mean queue delay is %.1f ms", ms(adaptive.queue_delay_sum_ns / adaptive.queue_delay_n));
    CHECK(repair_share_pct(&adaptive) <= 5U, "the repair share is %llu %%",
          (unsigned long long)repair_share_pct(&adaptive));
    CHECK(repair_share_pct(&adaptive) * 4U < repair_share_pct(&unpaced), "repair share %llu %% vs unpaced %llu %%",
          (unsigned long long)repair_share_pct(&adaptive), (unsigned long long)repair_share_pct(&unpaced));

    // 2. constant 30 Mbit/s, queue 100 frames
    struct link_cfg fast = {{30000000ULL, 30000000ULL, 30000000ULL},
                            1000 * SEC,
                            2000 * SEC,
                            100U * UNIT_WIRE,
                            10000000ULL};
    run(&fast, MODE_UNPACED, 0, 0, 0, 4 * SEC, 40 * SEC, &unpaced);
    run(&fast, MODE_FIXED, 22000000ULL, 0, 0, 4 * SEC, 40 * SEC, &fixed);
    run(&fast, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 4 * SEC, 40 * SEC, &adaptive);
    printf("-- 30 Mbit/s, queue 100, RTT 20 ms, 1 MiB every 4 s --\n");
    print_result("unpaced", &unpaced);
    print_result("fixed 22 Mbit/s (oracle)", &fixed);
    print_result("adaptive", &adaptive);
    CHECK(adaptive.samples_done >= 8U, "adaptive completes the samples: %u", adaptive.samples_done);

    // 3. rate step 30 -> 4 -> 30 Mbit/s, bulk (back-to-back samples)
    struct link_cfg step = {{30000000ULL, 4000000ULL, 30000000ULL}, 8 * SEC, 24 * SEC, 60U * UNIT_WIRE, 10000000ULL};
    run(&step, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 0, 40 * SEC, &adaptive);
    run(&step, MODE_UNPACED, 0, 0, 0, 0, 40 * SEC, &unpaced);
    printf("-- step 30 -> 4 (t = 8 s) -> 30 (t = 24 s) Mbit/s, queue 60, bulk --\n");
    print_result("unpaced", &unpaced);
    print_result("adaptive", &adaptive);
    printf("BtlBw within 25 %% of the link: %.0f ms after start, %.0f ms after the drop, %.0f ms after the rise\n",
           ms(adaptive.rate_after_step_ok_ns[0]), ms(adaptive.rate_after_step_ok_ns[1]),
           ms(adaptive.rate_after_step_ok_ns[2]));
    CHECK(adaptive.rate_after_step_ok_ns[0] != 0, "the estimate never reached the first rate");
    CHECK(adaptive.rate_after_step_ok_ns[1] != 0, "the estimate never followed the drop");
    CHECK(adaptive.rate_after_step_ok_ns[2] != 0, "the estimate never followed the rise");
    CHECK(adaptive.rate_after_step_ok_ns[0] < 3 * SEC, "the start took %.0f ms", ms(adaptive.rate_after_step_ok_ns[0]));
    CHECK(adaptive.rate_after_step_ok_ns[1] < 1 * SEC, "the drop took %.0f ms", ms(adaptive.rate_after_step_ok_ns[1]));
    CHECK(adaptive.rate_after_step_ok_ns[2] < 1 * SEC, "the rise took %.0f ms", ms(adaptive.rate_after_step_ok_ns[2]));
    CHECK(repair_share_pct(&adaptive) * 4U < repair_share_pct(&unpaced),
          "step: repair share %llu %% vs unpaced %llu %%", (unsigned long long)repair_share_pct(&adaptive),
          (unsigned long long)repair_share_pct(&unpaced));

    // 4. the cap and the floor are honoured
    run(&fast, MODE_ADAPTIVE, 0, 2000000ULL, 500000ULL, 0, 20 * SEC, &adaptive);
    printf("-- 30 Mbit/s link, cap 2 Mbit/s, floor 0.5 Mbit/s: rate %.2f .. %.2f Mbit/s --\n",
           (double)adaptive.min_rate_bps / 1e6, (double)adaptive.max_rate_bps / 1e6);
    CHECK(adaptive.max_rate_bps <= 2000000ULL, "above the cap: %llu", (unsigned long long)adaptive.max_rate_bps);
    CHECK(adaptive.min_rate_bps >= 500000ULL, "below the floor: %llu", (unsigned long long)adaptive.min_rate_bps);

    if (failures != 0) {
        printf("test_adaptive_pacing: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_adaptive_pacing: all checks passed\n");
    return 0;
}

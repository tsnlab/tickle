// algosim: offline model comparison of rate-control candidates for TickLE's reliable, paced large-sample send
// (DESIGN.md section 27). One run = one (workload schedule, sample size, candidate, feedback model, seed).
//
//   algosim <sched.txt> <units per sample> <candidate> <a|b> <seed> [period_s] > one result line
//
// The link model is that of tests/test_adaptive_pacing.c (a 100 us clock, a paced sender, a drop-tail queue served at
// the link rate, a reader that answers solicitations at its poll tick, watermark ACKNACKs, repairs behind the paced
// send), generalised to a schedule of rate / one-way delay / loss, a packet-count queue limit, outages (100 % loss), a
// Gilbert-Elliott loss state, and a lossy reverse path. The current ADAPTIVE (include/tickle/pacing_adaptive.h) is the
// control and runs unmodified on the existing feedback.
//
// Feedback models:  a = the feedback the library has (one ACKNACK per solicitation round, a round trip sample in 1 of
// 12 rounds, a reader poll tick of 8 ms);  b = a, plus an enhanced receiver report every 100 ms (see struct enh) with
// the fields each candidate needs.
//
// NOT a statement about the Pi: the model has no lumpy readings from the Pi's executor, no absolute latency, and its
// repairs and RTT samples are cleaner than the rig's (DESIGN.md section 27, limits).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/pacing_adaptive.h> // the unmodified library header plus two knobs (build.sh): g_ap_sparse, g_ap_start_max

#define UNIT_WIRE 1538U
#define SOLICIT_WIRE 90U
#define ACKNACK_WIRE 110U
#define STEP_NS 100000ULL
#define MS 1000000ULL
#define SEC 1000000000ULL
#define MAX_Q 4096U
#define MAX_DELAY 16384U
#define MAX_UNITS 150000U
#define MAX_SCHED 1024U
#define ENH_PERIOD_NS (100ULL * MS)
#define TICK_NS (8ULL * MS)
#define UNSOLICITED_NS (3ULL * MS)

enum cand { C_FIXED, C_ORACLE, C_ADAPT, C_BBR, C_NADA, C_GCC, C_TRAIN, C_TRAIN1, C_TRAIN1T, C_TRAINT };
#define TRAIN_K 20U // datagrams of a sample sent back to back as a train (TRAIN, TRAIN1)
static enum cand g_cand;
static bool g_enh;           // feedback model b
static uint64_t g_start_bps; // the start rate of the candidates that have one
static uint32_t g_seed = 1;
static uint32_t g_sample_units = 89;
static uint64_t g_slot_ns; // stress: the link serves its queue only at slot boundaries (an aggregating or scheduled
                           // link), 0 = continuously
static uint64_t g_jit_ns;  // stress: uniform jitter on the receiver's arrival time stamps, +- this
static uint32_t g_train_k = 20U; // datagrams of the current sample's head sent as a train

// ---- schedule ----------------------------------------------------------------------------------------------------
struct spt {
    uint64_t t_ns, bps, delay_ns;
    uint32_t loss_ppm;
};
static struct spt g_s[MAX_SCHED];
static uint32_t g_sn;
static uint64_t g_dur_ns;
static uint32_t g_limit_pkts = 100;
static bool g_ge;
static uint32_t g_ge_p, g_ge_r, g_ge_bad, g_ge_good;
static uint32_t g_si;

static void sched_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long a;
        unsigned long b;
        unsigned long c;
        unsigned long d;
        char nm[64];
        if (sscanf(line, "name %63s", nm) == 1) {
            continue;
        }
        if (sscanf(line, "dur_ms %lu", &a) == 1) {
            g_dur_ns = a * MS;
        } else if (sscanf(line, "limit_pkts %lu", &a) == 1) {
            g_limit_pkts = (uint32_t)a;
        } else if (sscanf(line, "ge %lu %lu %lu %lu", &a, &b, &c, &d) == 4) {
            g_ge = true;
            g_ge_p = (uint32_t)a;
            g_ge_r = (uint32_t)b;
            g_ge_bad = (uint32_t)c;
            g_ge_good = (uint32_t)d;
        } else if (sscanf(line, "%lu %lu %lu %lu", &a, &b, &c, &d) == 4 && g_sn < MAX_SCHED) {
            g_s[g_sn++] = (struct spt) {a * MS, b * 1000ULL, c * MS, (uint32_t)d};
        }
    }
    fclose(f);
}
static const struct spt* sched_at(uint64_t now) {
    while (g_si + 1U < g_sn && now >= g_s[g_si + 1U].t_ns) {
        g_si++;
    }
    return &g_s[g_si];
}
static uint64_t cap_at(uint64_t now) { // read-only lookup (metrics)
    uint32_t i = 0;
    while (i + 1U < g_sn && now >= g_s[i + 1U].t_ns) {
        i++;
    }
    return g_s[i].bps;
}
static double mean_cap_bps(void) {
    double acc = 0;
    for (uint32_t i = 0; i < g_sn; i++) {
        uint64_t t1 = i + 1U < g_sn ? g_s[i + 1U].t_ns : g_dur_ns;
        acc += (double)g_s[i].bps * (double)(t1 - g_s[i].t_ns);
    }
    return acc / (double)g_dur_ns;
}

// ---- packets, rings ----------------------------------------------------------------------------------------------
struct pkt {
    uint64_t enq_ns;
    uint32_t wire;
    int64_t unit;        // -1 for a solicit
    uint64_t solicit_ns; // (report) the solicit's send time
    int64_t last_unit;   // (solicit) its HEARTBEAT last_seq_no; (report) the watermark
};
struct flight {
    uint64_t at_ns;
    struct pkt pkt;
};

#define MAX_GROUPS 96U
struct grp {
    uint64_t send_ns, arr_ns;
    uint32_t bytes;
};
struct enh { // the enhanced receiver report (feedback model b): what each candidate may read
    uint64_t gen_ns;
    uint32_t n_pkts;                    // data datagrams arrived in the interval
    uint64_t bytes;                     // their wire bytes
    uint64_t t_first, t_last;           // arrival times (receiver clock) of the first and last
    uint64_t last_send_ns, last_arr_ns; // send stamp and arrival of the newest datagram (RTT echo)
    int64_t owd_min15_ns; // relative one-way delay, min over the last 15 datagrams (receiver minus sender clock)
    uint32_t gaps_new;    // new-data ids skipped in the interval
    uint32_t new_units;   // new-data datagrams arrived
    uint32_t ngroups;     // completed 5 ms send groups, in arrival order
    bool train_valid;     // the first TRAIN_K fragments of a sample arrived: their first/last arrival and count
    uint32_t train_n, train_k;
    uint64_t train_t_first, train_t_last;
    struct grp g[MAX_GROUPS];
};
struct enh_flight {
    uint64_t at_ns;
    struct enh e;
};

// ---- result ------------------------------------------------------------------------------------------------------
struct result {
    uint64_t sent_units, repair_units, dropped, solicits, reports, enh_reports;
    uint32_t samples_done, samples_started;
    double lat_ms[512];
    uint64_t steps_counted;
    uint64_t ratio_hist[512];
    uint64_t active_steps, qdelay100_steps;
    double cap_active_bits; // integral of the link rate over the time a sample was open (wire bits)
    double got_bits;        // wire bits of the samples that completed
    uint64_t over3_steps, frozen_steps;
    uint32_t rate_kbit[2048]; // the pacing rate at every 100 ms
    uint64_t fb_bytes;        // reverse-path feedback bytes
    uint32_t enh_size;
};
static struct result R;

// ---- reader / link state (globals: one run per process) ------------------------------------------------------------
static struct pkt queue_[MAX_Q];
static struct flight fwd_[MAX_DELAY];
static struct flight rev_[MAX_DELAY];
static struct flight pending_[64];
static struct enh_flight enhq_[64];
static uint8_t got_[MAX_UNITS];
static uint64_t last_sent_[MAX_UNITS];
static uint64_t named_ns_[MAX_UNITS];
static uint8_t want_repair_[MAX_UNITS];

static uint32_t lcg_;
static uint32_t rnd(void) {
    lcg_ = (lcg_ * 1103515245U) + 12345U;
    return (lcg_ >> 8) % 1000000U;
}

// ---- the reader's ACKNACK (as tests/test_adaptive_pacing.c reader_report, one reader)
// ---------------------------------
static void reader_report(int64_t first_id, int64_t last_id, uint64_t unique, uint64_t now, uint64_t omit_ns,
                          uint64_t* held, uint64_t* mark) {
    *held = unique;
    *mark = unique;
    int64_t highest_got = -1;
    for (int64_t u = first_id; u <= last_id; u++) {
        if (got_[u] != 0) {
            highest_got = u;
        }
    }
    if (highest_got < 0) {
        *held = first_id; // earlier samples are complete
        *mark = first_id;
        return;
    }
    int64_t lowest_missing = -1;
    int64_t highest_named = -1;
    uint64_t named = 0;
    for (int64_t u = first_id; u <= highest_got; u++) {
        if (got_[u] != 0) {
            continue;
        }
        lowest_missing = lowest_missing < 0 ? u : lowest_missing;
        if (named_ns_[u] != 0 && now - named_ns_[u] < omit_ns) {
            continue;
        }
        named_ns_[u] = now;
        highest_named = u;
        named++;
    }
    if (lowest_missing < 0) {
        *held = (uint64_t)highest_got + 1U;
        *mark = *held;
        return;
    }
    uint64_t watermark = (uint64_t)lowest_missing;
    *mark = watermark;
    if (named == 0) {
        *held = watermark;
        return;
    }
    uint64_t window = ((uint64_t)highest_named - watermark) + 1U;
    *held = watermark + window - named;
}

// ======================================================================================================================
// Candidates. Every one gets the same events: note_send, cur_fb (a report of the existing kind), enh_fb (model b).
// ======================================================================================================================
static struct ap_pacer ap_;
static uint64_t fixed_bps_;

// -- history of the pacing rate (the rate a datagram was sent at)
// ------------------------------------------------------
static uint32_t rate_hist_[4096]; // kbit/s at 10 ms resolution, ring
static uint64_t rate_at(uint64_t t) {
    return (uint64_t)rate_hist_[(t / (10 * MS)) % 4096U] * 1000ULL;
}

// -- shared estimators -----------------------------------------------------------------------------------------------
struct cur_fb {
    uint64_t now;
    uint64_t held, mark; // cumulative, units
    bool rtt_ok;         // a round trip sample (1 in 12 under model a, every report under b is read from enh instead)
    uint64_t rtt_ns;
};

// BBRv3 (compact; no ProbeRTT: a bursty application is mostly idle, which refreshes min_rtt)
// ------------------------------
enum bbr_st { B_STARTUP, B_DRAIN, B_DOWN, B_CRUISE, B_REFILL, B_UP };
static struct {
    enum bbr_st st;
    double bw_hist[10];
    uint32_t bw_n;
    double bw_lo; // 0 = unset
    double pacing_bps;
    double full_bw;
    uint32_t full_cnt;
    uint64_t minrtt, minrtt_t;
    uint64_t st_t, next_probe_t;
    uint64_t i_t0, i_held0, i_sent0, i_rep0;
    bool i_open;
    uint64_t sent_bytes, rep_bytes;
    double gain;
} bbr_;
static double bbr_bw(void) {
    double m = 0;
    for (uint32_t i = 0; i < bbr_.bw_n && i < 10; i++) {
        m = bbr_.bw_hist[i] > m ? bbr_.bw_hist[i] : m;
    }
    return m;
}
static void bbr_sample(uint64_t now, double bw_bps, uint64_t rtt_ns, double loss, bool app_limited) {
    if (rtt_ns != 0 && (bbr_.minrtt == 0 || rtt_ns <= bbr_.minrtt || now - bbr_.minrtt_t > 10 * SEC)) {
        bbr_.minrtt = rtt_ns;
        bbr_.minrtt_t = now;
    }
    double cur = bbr_bw();
    if (!app_limited || bw_bps >= cur) {
        bbr_.bw_hist[bbr_.bw_n++ % 10U] = bw_bps;
    }
    double bw = bbr_bw();
    bool lossy = loss > 0.02;
    if (lossy) { // v3: lower bound from a lossy round
        double lo = bbr_.bw_lo != 0 ? bbr_.bw_lo * 0.7 : bw * 0.7;
        bbr_.bw_lo = lo > bw_bps ? lo : bw_bps;
    }
    switch (bbr_.st) {
    case B_STARTUP:
        if (bw >= bbr_.full_bw * 1.25) {
            bbr_.full_bw = bw;
            bbr_.full_cnt = 0;
        } else if (!app_limited && ++bbr_.full_cnt >= 3) {
            bbr_.st = B_DRAIN;
        }
        if (lossy && bbr_.st == B_STARTUP) {
            bbr_.st = B_DRAIN;
        }
        bbr_.gain = 2.77;
        if (bbr_.st == B_DRAIN) {
            bbr_.st_t = now;
        }
        break;
    case B_DRAIN:
        bbr_.gain = 0.5;
        if (now - bbr_.st_t > 2 * (bbr_.minrtt != 0 ? bbr_.minrtt : 10 * MS) + 20 * MS) {
            bbr_.st = B_CRUISE;
            bbr_.st_t = now;
            bbr_.next_probe_t = now + 2500 * MS + (uint64_t)(rnd() % 1000U) * MS;
        }
        break;
    case B_DOWN:
        bbr_.gain = 0.9;
        if (now - bbr_.st_t > (bbr_.minrtt != 0 ? bbr_.minrtt : 10 * MS) + 10 * MS) {
            bbr_.st = B_CRUISE;
            bbr_.st_t = now;
        }
        break;
    case B_CRUISE:
        bbr_.gain = 1.0;
        if (now >= bbr_.next_probe_t) {
            bbr_.st = B_REFILL;
            bbr_.st_t = now;
            bbr_.bw_lo = 0;
        }
        break;
    case B_REFILL:
        bbr_.gain = 1.0;
        if (now - bbr_.st_t > (bbr_.minrtt != 0 ? bbr_.minrtt : 10 * MS)) {
            bbr_.st = B_UP;
            bbr_.st_t = now;
        }
        break;
    case B_UP:
        bbr_.gain = 1.25;
        if (lossy || now - bbr_.st_t > 3 * (bbr_.minrtt != 0 ? bbr_.minrtt : 10 * MS) + 20 * MS) {
            bbr_.st = B_DOWN;
            bbr_.st_t = now;
            bbr_.next_probe_t = now + 2500 * MS + (uint64_t)(rnd() % 1000U) * MS;
        }
        break;
    }
    double base = bw;
    if (bbr_.bw_lo != 0 && bbr_.bw_lo < base && bbr_.st != B_UP && bbr_.st != B_STARTUP) {
        base = bbr_.bw_lo;
    }
    double r = bbr_.gain * base * 0.99;
    bbr_.pacing_bps = r < 100000.0 ? 100000.0 : (r > 1e9 ? 1e9 : r);
}
static void bbr_cur(const struct cur_fb* f) {
    uint64_t bytes = f->held > bbr_.i_held0 ? (f->held - bbr_.i_held0) * UNIT_WIRE : 0;
    if (!bbr_.i_open) {
        bbr_.i_open = true;
        bbr_.i_t0 = f->now;
        bbr_.i_held0 = f->held;
        bbr_.i_sent0 = bbr_.sent_bytes;
        bbr_.i_rep0 = bbr_.rep_bytes;
        return;
    }
    uint64_t el = f->now - bbr_.i_t0;
    uint64_t least = bbr_.minrtt != 0 ? bbr_.minrtt : 10 * MS;
    if (least < 10 * MS) {
        least = 10 * MS;
    }
    if (el < least || bytes == 0) {
        return;
    }
    double bw = (double)bytes * 8e9 / (double)el;
    uint64_t sent = bbr_.sent_bytes - bbr_.i_sent0;
    double loss = sent != 0 ? (double)(bbr_.rep_bytes - bbr_.i_rep0) / (double)sent : 0.0;
    bool app = (double)sent * 8e9 / (double)el < 0.9 * bbr_.pacing_bps;
    bbr_sample(f->now, bw, f->rtt_ok ? f->rtt_ns : 0, loss, app);
    bbr_.i_t0 = f->now;
    bbr_.i_held0 = f->held;
    bbr_.i_sent0 = bbr_.sent_bytes;
    bbr_.i_rep0 = bbr_.rep_bytes;
}
static struct {
    uint64_t t0, sent0, rep0;
} bbr_enh_;
static void bbr_enh(uint64_t now, const struct enh* e) {
    if (e->n_pkts < 4 || e->t_last <= e->t_first) {
        return;
    }
    double bw = (double)e->bytes * 8e9 / (double)(e->t_last - e->t_first + 1ULL);
    uint64_t sent = bbr_.sent_bytes - bbr_enh_.sent0;
    uint64_t rep = bbr_.rep_bytes - bbr_enh_.rep0;
    bbr_enh_.sent0 = bbr_.sent_bytes;
    bbr_enh_.rep0 = bbr_.rep_bytes;
    double loss = sent != 0 ? (double)rep / (double)sent : 0.0;
    uint64_t rtt = e->last_arr_ns > e->last_send_ns ? (e->last_arr_ns - e->last_send_ns) + (now - e->gen_ns) : 0;
    bbr_sample(now, bw, rtt, loss, false);
}

// NADA (RFC 8698): gradual update with an accelerated ramp-up; the receiver's one-way delay stands in via the report
// -----
static struct {
    double r_ref, x_prev, rmax;
    uint64_t last_upd;
    double dbase_ms;
    uint64_t dbase_t;
    double rrecv_hist[64];
    uint64_t rrecv_t[64];
    uint32_t rrecv_n; // r_recv samples for the adaptive RMAX (10 s)
    // model a accumulation
    uint64_t iv_t0, iv_tl, iv_held0, iv_heldl;
    uint32_t iv_n;
    double rtt15[15];
    uint32_t rtt15_n;
    uint64_t sent_bytes, rep_bytes, iv_sent0, iv_rep0;
} nada_;
static void nada_update(uint64_t now, double r_recv, double d_queue_ms, double p_loss, double rtt_ms) {
    if (nada_.last_upd == 0) {
        nada_.last_upd = now - 100 * MS;
    }
    double dt = (double)(now - nada_.last_upd) / 1e6;
    if (dt > 500.0) {
        dt = 500.0; // after an idle gap one interval, not the gap
    }
    nada_.last_upd = now;
    // RMAX: twice the best receive rate seen in the last 10 s (the RFC takes the application's maximum, which a
    // sender that does not know the link does not have)
    nada_.rrecv_hist[nada_.rrecv_n % 64U] = r_recv;
    nada_.rrecv_t[nada_.rrecv_n % 64U] = now;
    nada_.rrecv_n++;
    double mx = 0;
    for (uint32_t i = 0; i < 64U && i < nada_.rrecv_n; i++) {
        if (now - nada_.rrecv_t[i] < 10 * SEC && nada_.rrecv_hist[i] > mx) {
            mx = nada_.rrecv_hist[i];
        }
    }
    nada_.rmax = 2.0 * mx > 1e6 ? 2.0 * mx : 1e6;
    double x_curr = d_queue_ms + 10.0 * (p_loss / 0.01) * (p_loss / 0.01);
    double r = nada_.r_ref;
    if (p_loss < 0.02 &&
        d_queue_ms < 10.0) { // accelerated ramp-up (loss threshold loosened: the links lose 1-4 % at random)
        double gamma = 50.0 / (rtt_ms + 100.0 + 120.0);
        gamma = gamma > 0.5 ? 0.5 : gamma;
        double up = (1.0 + gamma) * r_recv;
        r = r > up ? r : up;
    } else { // gradual update, tau 500 ms, kappa 0.5, eta 2, XREF 10 ms, PRIO 1
        double x_offset = x_curr - 10.0 * nada_.rmax / r;
        double x_diff = x_curr - nada_.x_prev;
        r = r - 0.5 * (dt / 500.0) * (x_offset / 500.0) * r - 0.5 * 2.0 * (x_diff / 500.0) * r;
        double lim = 1.5 * r_recv;
        r = r > lim ? lim : r;
    }
    nada_.x_prev = x_curr;
    r = r < 1e6 ? 1e6 : (r > nada_.rmax ? nada_.rmax : r);
    nada_.r_ref = r;
}
static void nada_cur(const struct cur_fb* f) { // model a: r_recv from the held counts, d_queue from sparse round trips
    if (f->rtt_ok) {
        nada_.rtt15[nada_.rtt15_n++ % 15U] = (double)f->rtt_ns / 1e6;
        if (nada_.dbase_ms == 0 || (double)f->rtt_ns / 1e6 < nada_.dbase_ms || f->now - nada_.dbase_t > 10 * SEC) {
            nada_.dbase_ms = (double)f->rtt_ns / 1e6;
            nada_.dbase_t = f->now;
        }
    }
    if (f->held > nada_.iv_heldl) {
        if (f->now - nada_.iv_tl > 200 * MS) {
            nada_.iv_n = 0; // an idle gap ends the interval
        }
        if (nada_.iv_n == 0) {
            nada_.iv_t0 = f->now;
            nada_.iv_held0 = f->held;
            nada_.iv_sent0 = nada_.sent_bytes;
            nada_.iv_rep0 = nada_.rep_bytes;
        }
        nada_.iv_n++;
        nada_.iv_tl = f->now;
        nada_.iv_heldl = f->held;
    }
    if (nada_.iv_n >= 3 && f->now - nada_.iv_t0 >= 20 * MS && nada_.iv_tl > nada_.iv_t0) {
        double r_recv =
            (double)(nada_.iv_heldl - nada_.iv_held0) * UNIT_WIRE * 8e9 / (double)(nada_.iv_tl - nada_.iv_t0);
        double m = 1e9;
        uint32_t n = nada_.rtt15_n < 15U ? nada_.rtt15_n : 15U;
        for (uint32_t i = 0; i < n; i++) {
            m = nada_.rtt15[i] < m ? nada_.rtt15[i] : m;
        }
        double dq = n != 0 && nada_.dbase_ms != 0 ? m - nada_.dbase_ms : 0.0;
        uint64_t sent = nada_.sent_bytes - nada_.iv_sent0;
        double p = sent != 0 ? (double)(nada_.rep_bytes - nada_.iv_rep0) / (double)sent : 0.0;
        nada_update(f->now, r_recv, dq, p, nada_.dbase_ms != 0 ? nada_.dbase_ms : 20.0);
        nada_.iv_n = 0;
        nada_.iv_heldl = f->held;
    }
}
static struct {
    double owd_base_ms;
    uint64_t owd_base_t;
} nada_enh_;
static void nada_enh(uint64_t now, const struct enh* e) {
    if (e->n_pkts < 4 || e->t_last <= e->t_first) {
        return;
    }
    double r_recv = (double)e->bytes * 8e9 / (double)(e->t_last - e->t_first + 1ULL);
    double owd = (double)e->owd_min15_ns / 1e6;
    if (nada_enh_.owd_base_t == 0 || owd < nada_enh_.owd_base_ms || now - nada_enh_.owd_base_t > 10 * SEC) {
        nada_enh_.owd_base_ms = owd;
        nada_enh_.owd_base_t = now;
    }
    double dq = owd - nada_enh_.owd_base_ms;
    uint32_t tot = e->gaps_new + e->new_units;
    double p = tot != 0 ? (double)e->gaps_new / (double)tot : 0.0;
    double rtt_ms = e->last_arr_ns > e->last_send_ns
                        ? (double)((e->last_arr_ns - e->last_send_ns) + (now - e->gen_ns)) / 1e6
                        : 20.0;
    nada_update(now, r_recv, dq, p, rtt_ms);
}

// GCC: trendline delay-gradient over arrival groups, over-use detector, AIMD, loss cap
// ---------------------------------
static struct {
    double rate;
    double acc, smooth; // accumulated and smoothed delay
    double tl_t[20], tl_y[20];
    uint32_t tl_n;
    double thr; // adaptive threshold, ms
    uint64_t thr_t;
    int hold_state; // 0 hold, 1 increase, 2 decrease
    uint64_t last_chg, last_dec;
    double avg_max, var_max; // bitrate at the last decrease
    uint32_t over_cnt;
    double prev_trend;
    bool have_prev;
    uint64_t prev_send, prev_arr; // last completed group
    uint32_t num_deltas;
    double acked_bps;
    double loss_rate;
    uint64_t sent_bytes, rep_bytes, iv_sent0, iv_rep0;
    uint64_t base_t;
} gcc_;
static void gcc_aimd(uint64_t now, int signal, double rtt_ms) {
    double dt_s = gcc_.last_chg != 0 ? (double)(now - gcc_.last_chg) / 1e9 : 0.1;
    dt_s = dt_s > 1.0 ? 1.0 : dt_s;
    gcc_.last_chg = now;
    if (signal == 1) { // over-use
        if (gcc_.hold_state != 2) {
            gcc_.hold_state = 2;
        }
    } else if (signal == -1) { // under-use
        gcc_.hold_state = 0;
    } else if (gcc_.hold_state == 0) {
        gcc_.hold_state = 1;
    } else if (gcc_.hold_state == 2) {
        gcc_.hold_state = 0;
    }
    double acked = gcc_.acked_bps;
    if (gcc_.hold_state == 2) {
        if (acked > 0 && now - gcc_.last_dec > (uint64_t)((rtt_ms + 10.0) * 1e6)) {
            double nr = 0.85 * acked;
            if (nr < gcc_.rate) {
                gcc_.rate = nr;
                gcc_.last_dec = now;
                gcc_.avg_max = gcc_.avg_max == 0 ? acked : 0.95 * gcc_.avg_max + 0.05 * acked;
            }
        }
    } else if (gcc_.hold_state == 1) {
        bool near = gcc_.avg_max != 0 && fabs(gcc_.rate - gcc_.avg_max) < 0.1 * gcc_.avg_max;
        if (near) { // additive: about a packet per response time
            gcc_.rate += 1200.0 * 8.0 / ((rtt_ms + 100.0) / 1000.0) * dt_s * 10.0;
        } else {
            gcc_.rate *= pow(1.08, dt_s);
        }
    }
    double lim = 1.5 * acked + 10000.0;
    if (acked > 0 && gcc_.rate > lim) {
        gcc_.rate = lim;
    }
}
static void gcc_trend_add(uint64_t now, double d_ms, double arr_ms, double rtt_ms) {
    gcc_.num_deltas++;
    gcc_.acc += d_ms;
    gcc_.smooth = 0.9 * gcc_.smooth + 0.1 * gcc_.acc;
    gcc_.tl_t[gcc_.tl_n % 20U] = arr_ms;
    gcc_.tl_y[gcc_.tl_n % 20U] = gcc_.smooth;
    gcc_.tl_n++;
    uint32_t n = gcc_.tl_n < 20U ? gcc_.tl_n : 20U;
    double slope = 0;
    if (n >= 5) {
        double mt = 0;
        double my = 0;
        for (uint32_t i = 0; i < n; i++) {
            mt += gcc_.tl_t[i];
            my += gcc_.tl_y[i];
        }
        mt /= n;
        my /= n;
        double nu = 0;
        double de = 0;
        for (uint32_t i = 0; i < n; i++) {
            nu += (gcc_.tl_t[i] - mt) * (gcc_.tl_y[i] - my);
            de += (gcc_.tl_t[i] - mt) * (gcc_.tl_t[i] - mt);
        }
        slope = de != 0 ? nu / de : 0;
    }
    double m = (gcc_.num_deltas < 60U ? (double)gcc_.num_deltas : 60.0) * slope * 4.0;
    // detector
    int signal = 0;
    double dt_ms = 5.0;
    if (fabs(m) < gcc_.thr + 15.0) {
        double k = fabs(m) < gcc_.thr ? 0.039 : 0.0087;
        gcc_.thr += k * (fabs(m) - gcc_.thr) * dt_ms;
        gcc_.thr = gcc_.thr < 6.0 ? 6.0 : (gcc_.thr > 600.0 ? 600.0 : gcc_.thr);
    }
    if (m > gcc_.thr) {
        gcc_.over_cnt++;
        if (gcc_.over_cnt > 1 && m >= gcc_.prev_trend) {
            signal = 1;
        }
    } else {
        gcc_.over_cnt = 0;
        signal = m < -gcc_.thr ? -1 : 0;
    }
    gcc_.prev_trend = m;
    gcc_aimd(now, signal, rtt_ms);
}
static void gcc_enh(uint64_t now, const struct enh* e) {
    if (e->n_pkts == 0) {
        return;
    }
    if (e->t_last > e->t_first && e->n_pkts >= 4) {
        double b = (double)e->bytes * 8e9 / (double)(e->t_last - e->t_first + 1ULL);
        gcc_.acked_bps = gcc_.acked_bps == 0 ? b : 0.7 * gcc_.acked_bps + 0.3 * b;
    }
    double rtt_ms = e->last_arr_ns > e->last_send_ns
                        ? (double)((e->last_arr_ns - e->last_send_ns) + (now - e->gen_ns)) / 1e6
                        : 20.0;
    for (uint32_t i = 0; i < e->ngroups; i++) {
        const struct grp* g = &e->g[i];
        if (gcc_.have_prev) {
            double d =
                ((double)g->arr_ns - (double)gcc_.prev_arr) / 1e6 - ((double)g->send_ns - (double)gcc_.prev_send) / 1e6;
            if (g->send_ns - gcc_.prev_send > 200 * MS) { // across an idle gap: a new train, no delay variation to read
                gcc_.have_prev = false;
            } else {
                gcc_trend_add(now, d, (double)g->arr_ns / 1e6, rtt_ms);
            }
        }
        gcc_.prev_send = g->send_ns;
        gcc_.prev_arr = g->arr_ns;
        gcc_.have_prev = true;
    }
    // loss-based controller on the report's gaps
    uint32_t tot = e->gaps_new + e->new_units;
    double p = tot != 0 ? (double)e->gaps_new / (double)tot : 0.0;
    double lb = gcc_.rate;
    if (p > 0.10) {
        lb = gcc_.rate * (1.0 - 0.5 * p);
    } else if (p < 0.02) {
        lb = gcc_.rate * 1.05;
    }
    if (p > 0.10) {
        gcc_.rate = lb;
    }
    gcc_.rate = gcc_.rate < 1e5 ? 1e5 : (gcc_.rate > 1e9 ? 1e9 : gcc_.rate);
}
static struct {
    double rtt15[15];
    uint32_t n;
    double base_ms;
    uint64_t base_t;
    double prev_rtt;
    uint64_t prev_t;
} gcc_cur_;
static void gcc_cur(const struct cur_fb* f) { // model a: the delay gradient is read off the sparse round trip samples
    // no per-datagram arrival times: acked bitrate from the held counts, delay variation from successive RTT samples
    static uint64_t h0 = 0;
    static uint64_t t0 = 0;
    static uint64_t tl = 0;
    if (t0 == 0 || f->now - tl > 200 * MS) {
        h0 = f->held; // an idle gap ends the interval
        t0 = f->now;
        tl = f->now;
    } else if (f->held > h0 && f->now - t0 >= 20 * MS) {
        double b = (double)(f->held - h0) * UNIT_WIRE * 8e9 / (double)(f->now - t0);
        gcc_.acked_bps = gcc_.acked_bps == 0 ? b : 0.7 * gcc_.acked_bps + 0.3 * b;
        h0 = f->held;
        t0 = f->now;
    }
    if (f->held > h0) {
        tl = f->now;
    }
    bool trended = false;
    if (f->rtt_ok) {
        double rtt_ms = (double)f->rtt_ns / 1e6;
        if (gcc_cur_.prev_t != 0 && f->now - gcc_cur_.prev_t < 500 * MS) {
            gcc_trend_add(f->now, rtt_ms - gcc_cur_.prev_rtt, (double)f->now / 1e6, rtt_ms);
            trended = true;
        }
        gcc_cur_.prev_rtt = rtt_ms;
        gcc_cur_.prev_t = f->now;
    }
    if (!trended && gcc_.acked_bps != 0 && f->now - gcc_.last_chg >= 100 * MS) {
        gcc_aimd(f->now, 0, gcc_cur_.prev_rtt != 0 ? gcc_cur_.prev_rtt : 20.0); // no gradient to read: no over-use seen
    }
    uint64_t sent = gcc_.sent_bytes - gcc_.iv_sent0;
    if (sent > 200000) {
        double p = (double)(gcc_.rep_bytes - gcc_.iv_rep0) / (double)sent;
        if (p > 0.10) {
            gcc_.rate *= 1.0 - 0.5 * p;
        }
        gcc_.iv_sent0 = gcc_.sent_bytes;
        gcc_.iv_rep0 = gcc_.rep_bytes;
    }
    gcc_.rate = gcc_.rate < 1e5 ? 1e5 : (gcc_.rate > 1e9 ? 1e9 : gcc_.rate);
}

// TRAIN: the arrival dispersion of a back-to-back train is the capacity. The first TRAIN_K fragments of every sample go
// out unpaced (a few tens of kilobytes, far under any queue), their first/last arrival times give the capacity; the
// rest is paced at 95 % of it. TRAIN also reads the 100 ms reports (a paced stretch that the path spreads; the queue
// delay corroborates); TRAIN1 reads the train alone (one small report per sample).
static struct {
    double rate;
    double est; // capacity estimate, 0 = none
    uint64_t owd_base_t;
    double owd_base_ms;
    bool probing;
    uint64_t last_probe;
    // model a
    uint64_t t0, tl, h0, hl;
    uint32_t n;
    double rtt15[15];
    uint32_t rtt_n;
    double rbase;
    uint64_t rbase_t;
} tr_;
static double clampd(double v) {
    return v < 1e5 ? 1e5 : (v > 1e9 ? 1e9 : v);
}
static void train_sample(uint64_t now, double disp) { // a train's dispersion: the capacity, take it
    (void)now;
    tr_.est = (tr_.est == 0 || disp > 2.0 * tr_.est || disp < 0.5 * tr_.est) ? disp : 0.5 * tr_.est + 0.5 * disp;
    tr_.rate = clampd(0.95 * tr_.est);
}
static uint32_t tr_unsat_;
static uint64_t tr_unsat_t_;
static void train_paced(uint64_t now, double disp, double rsent, double q_ms) {
    if (now - tr_unsat_t_ > SEC) {
        tr_unsat_ = 0; // an idle gap ends the run of unsaturated readings
    }
    tr_unsat_t_ = now;
    bool sat = disp < 0.8 * rsent && (q_ms > 3.0 || !g_enh);
    if (sat) {
        tr_.est = disp;
        tr_.rate = clampd(0.95 * disp);
        tr_unsat_ = 0;
    } else if (disp >= 0.9 * rsent) {
        // the path carried what was sent: two such readings in a row raise the rate (a probe, corrected by the next
        // saturated one)
        if (++tr_unsat_ >= 2U) {
            tr_.rate = clampd(tr_.rate * 1.25);
            if (tr_.est < tr_.rate) {
                tr_.est = tr_.rate;
            }
        }
    }
}
static void train_enh(uint64_t now, const struct enh* e) {
    if (e->train_valid && e->train_n >= e->train_k / 2U && e->train_t_last > e->train_t_first) {
        double disp = (double)(e->train_n - 1U) * UNIT_WIRE * 8e9 / (double)(e->train_t_last - e->train_t_first);
        train_sample(now, disp);
    }
    if (g_cand == C_TRAIN1 || g_cand == C_TRAIN1T || e->n_pkts < 8 || e->t_last <= e->t_first + MS) {
        return;
    }
    double disp = (double)e->bytes * 8e9 / (double)(e->t_last - e->t_first + 1ULL);
    double owd = (double)e->owd_min15_ns / 1e6;
    if (tr_.owd_base_t == 0 || owd < tr_.owd_base_ms || now - tr_.owd_base_t > 10 * SEC) {
        tr_.owd_base_ms = owd;
        tr_.owd_base_t = now;
    }
    double rsent = (double)rate_at(e->last_send_ns);
    if (rsent < 1e9 * 0.9) { // the paced part (a report that is only the train says nothing of the paced rate)
        train_paced(now, disp, rsent, owd - tr_.owd_base_ms);
    }
}
static void train_cur(const struct cur_fb* f) { // model a: the same, from the held counts at the report times
    if (f->rtt_ok) {
        tr_.rtt15[tr_.rtt_n++ % 15U] = (double)f->rtt_ns / 1e6;
        if (tr_.rbase == 0 || (double)f->rtt_ns / 1e6 < tr_.rbase || f->now - tr_.rbase_t > 10 * SEC) {
            tr_.rbase = (double)f->rtt_ns / 1e6;
            tr_.rbase_t = f->now;
        }
    }
    if (f->held > tr_.hl) {
        if (f->now - tr_.tl > 200 * MS) {
            tr_.n = 0;
        }
        if (tr_.n == 0) {
            tr_.t0 = f->now;
            tr_.h0 = f->held;
        }
        tr_.n++;
        tr_.tl = f->now;
        tr_.hl = f->held;
    }
    if (tr_.n >= 3 && tr_.tl - tr_.t0 >= 20 * MS) {
        double disp = (double)(tr_.hl - tr_.h0) * UNIT_WIRE * 8e9 / (double)(tr_.tl - tr_.t0);
        double m = 1e9;
        uint32_t n = tr_.rtt_n < 15U ? tr_.rtt_n : 15U;
        for (uint32_t i = 0; i < n; i++) {
            m = tr_.rtt15[i] < m ? tr_.rtt15[i] : m;
        }
        double q = n != 0 && tr_.rbase != 0 ? m - tr_.rbase : 0.0;
        double rsent = (double)rate_at(tr_.tl - 20 * MS);
        if (rsent > 0.9e9) {
            train_sample(f->now, disp); // the train, as far as report times show it
        } else {
            train_paced(f->now, disp, rsent, q);
        }
        tr_.n = 0;
        tr_.h0 = tr_.hl;
    }
}

// ---- dispatch ----------------------------------------------------------------------------------------------------
static void cand_init(uint64_t one_way_ns) {
    double start = (double)g_start_bps;
    switch (g_cand) {
    case C_ADAPT:
        ap_init(&ap_, 100000ULL, 1000000000ULL, 0, 2 * one_way_ns, UNIT_WIRE);
        break;
    case C_BBR:
        bbr_.st = B_STARTUP;
        bbr_.gain = 2.77;
        bbr_.pacing_bps = start;
        bbr_.bw_hist[0] = start / 2.77; // a first guess one startup gain below the start rate
        bbr_.bw_n = 1;
        break;
    case C_NADA:
        nada_.r_ref = start;
        nada_.rmax = 1e6;
        break;
    case C_GCC:
        gcc_.rate = start;
        gcc_.thr = 12.5;
        break;
    case C_TRAIN:
    case C_TRAINT:
    case C_TRAIN1:
    case C_TRAIN1T:
        tr_.rate = start;
        break;
    default:
        break;
    }
}
static uint64_t cand_rate(uint64_t now) {
    switch (g_cand) {
    case C_FIXED:
        return fixed_bps_;
    case C_ORACLE:
        return cap_at(now) * 95U / 100U;
    case C_ADAPT:
        return ap_rate_bps(&ap_);
    case C_BBR:
        return (uint64_t)bbr_.pacing_bps;
    case C_NADA:
        return (uint64_t)nada_.r_ref;
    case C_GCC:
        return (uint64_t)gcc_.rate;
    case C_TRAIN:
    case C_TRAINT:
    case C_TRAIN1:
    case C_TRAIN1T:
        return (uint64_t)tr_.rate;
    }
    return 1000000;
}
static void cand_note_send(uint64_t now, uint32_t wire, bool repair, bool original) {
    switch (g_cand) {
    case C_ADAPT:
        ap_note_send(&ap_, now + 1U, wire, repair);
        if (original) {
            ap_note_data_sent(&ap_, now, wire, 1U);
        }
        break;
    case C_BBR:
        bbr_.sent_bytes += wire;
        bbr_.rep_bytes += repair ? wire : 0U;
        break;
    case C_NADA:
        nada_.sent_bytes += wire;
        nada_.rep_bytes += repair ? wire : 0U;
        break;
    case C_GCC:
        gcc_.sent_bytes += wire;
        gcc_.rep_bytes += repair ? wire : 0U;
        break;
    default:
        break;
    }
}
static void cand_cur(const struct cur_fb* f) {
    switch (g_cand) {
    case C_ADAPT:
        ap_on_feedback(&ap_, f->now, f->held, f->mark);
        break;
    case C_BBR:
        if (!g_enh) {
            bbr_cur(f);
        }
        break;
    case C_NADA:
        if (!g_enh) {
            nada_cur(f);
        }
        break;
    case C_GCC:
        if (!g_enh) {
            gcc_cur(f);
        }
        break;
    case C_TRAIN:
    case C_TRAINT:
    case C_TRAIN1:
    case C_TRAIN1T:
        if (!g_enh) {
            train_cur(f);
        }
        break;
    default:
        break;
    }
}
// bytes of one enhanced report from each candidate's fields: 12 byte header (interval bytes, t_first/t_last offsets)
static uint32_t enh_size(uint32_t groups) {
    switch (g_cand) {
    case C_BBR:
        return 12 + 4; // + RTT echo
    case C_NADA:
        return 12 + 4 + 2 + 2; // + owd_min15, gaps, new units
    case C_GCC:
        return 12 + 4 + 2 + 5 * groups; // + gaps, per-group (send bucket delta, arrival offset)
    case C_TRAIN:
    case C_TRAINT:
        return 12 + 4 + 4 + 12; // + n_pkts, owd_min15, the train (t_first, t_last, count)
    case C_TRAIN1:
    case C_TRAIN1T:
        return 12; // the train alone: (t_first, t_last as a 4 byte span, count)
    default:
        return 0;
    }
}
static void cand_enh(uint64_t now, const struct enh* e) {
    switch (g_cand) {
    case C_BBR:
        bbr_enh(now, e);
        break;
    case C_NADA:
        nada_enh(now, e);
        break;
    case C_GCC:
        gcc_enh(now, e);
        break;
    case C_TRAIN:
    case C_TRAINT:
    case C_TRAIN1:
    case C_TRAIN1T:
        train_enh(now, e);
        break;
    default:
        break;
    }
}

// ======================================================================================================================
static void run(void) {
    memset(&R, 0, sizeof R);
    memset(got_, 0, sizeof got_);
    memset(last_sent_, 0, sizeof last_sent_);
    memset(named_ns_, 0, sizeof named_ns_);
    memset(want_repair_, 0, sizeof want_repair_);
    uint32_t qh = 0, qt = 0, qn = 0;
    uint32_t fh = 0, ft = 0, fn = 0;
    uint32_t rh = 0, rt = 0, rn = 0;
    uint32_t pend_n = 0;
    uint32_t eh = 0, et = 0, en = 0;
    int64_t credit = 0;
    int64_t sample_first = 0, sample_next = 0, sample_end = -1;
    uint32_t sample_got = 0, sample_total = 0;
    bool sampling = false;
    uint64_t sample_started = 0, next_sample_ns = 0, next_send_ns = 0, next_unsol_ns = 0, last_solicit_ns = 0;
    uint64_t next_solicit_ns = 0;
    bool solicit_open = false;
    bool ge_bad = false;
    uint64_t srtt = 2 * g_s[0].delay_ns;
    uint64_t period_ns = 4 * SEC;
    uint64_t delivered_units = 0;
    uint64_t last_fwd_at = 0, last_rev_at = 0;
    // enhanced-report accumulation
    struct enh acc;
    memset(&acc, 0, sizeof acc);
    int64_t owd15[15];
    uint32_t owd_n = 0;
    uint64_t next_enh_ns = ENH_PERIOD_NS;
    int64_t highest_orig = -1;
    int64_t grp_bucket = -1;
    struct grp cur_grp = {0, 0, 0};
    uint32_t sparse_cnt = 0;
    uint32_t tr_n = 0;
    uint64_t tr_tf = 0, tr_tl = 0;
    bool tr_ready = false;
    bool trains = g_cand == C_TRAIN || g_cand == C_TRAIN1 || g_cand == C_TRAIN1T || g_cand == C_TRAINT;
    cand_init(g_s[0].delay_ns);
    fixed_bps_ = (uint64_t)(0.95 * mean_cap_bps());
    uint64_t fb_cur_bytes = 0;
    (void)fb_cur_bytes;

    for (uint64_t now = 0; now < g_dur_ns; now += STEP_NS) {
        const struct spt* L = sched_at(now);
        // 1. a sample begins (sequential: the next waits for the last)
        if (!sampling && now >= next_sample_ns && sample_end + 1 + g_sample_units < (int64_t)MAX_UNITS) {
            sample_first = sample_end + 1;
            sample_next = sample_first;
            sample_end = sample_first + (int64_t)g_sample_units - 1;
            sample_got = 0;
            tr_n = 0;
            tr_ready = false;
            g_train_k = TRAIN_K;
            if ((g_cand == C_TRAIN1T || g_cand == C_TRAINT) &&
                tr_.est > 0) { // a train of about 10 ms of the link: long against its slots
                uint32_t k = (uint32_t)(tr_.est * 0.010 / ((double)UNIT_WIRE * 8.0));
                g_train_k = k < TRAIN_K ? TRAIN_K : (k > g_sample_units / 2U ? g_sample_units / 2U : k);
            }
            sample_total = g_sample_units;
            sampling = true;
            sample_started = now;
            next_sample_ns = now + period_ns;
            R.samples_started++;
        }
        // 2. the writer sends, repairs ahead of new units (deferred behind the paced send), at the pacing rate
        uint64_t pace = cand_rate(now);
        uint64_t rate = pace;
        if (trains && sampling && sample_next - sample_first < (int64_t)g_train_k) {
            rate = 1000000000ULL; // the head of a sample goes out as a back-to-back train
        }
        rate_hist_[(now / (10 * MS)) % 4096U] = (uint32_t)(rate / 1000U);
        {
            uint64_t cap = L->bps;
            if (now > 2 * SEC) {
                uint64_t r100 = cap != 0 ? (pace * 100000ULL) / (cap * 991ULL) : 0; // pace / (cap x 0.991) in percent
                R.ratio_hist[r100 < 511 ? r100 : 511]++;
                R.steps_counted++;
                if (cap != 0 && pace > 3 * cap) {
                    R.over3_steps++;
                }
            }
            if (now % (100 * MS) < STEP_NS && now / (100 * MS) < 2048U) {
                R.rate_kbit[now / (100 * MS)] = (uint32_t)(pace / 1000U);
            }
            if (sampling) {
                R.active_steps++;
                R.cap_active_bits += (double)cap * (double)STEP_NS / 1e9;
                if (qn != 0 && (double)qn * UNIT_WIRE * 8.0 / (double)cap > 0.1) {
                    R.qdelay100_steps++; // the queue ahead of a new datagram is more than 100 ms of the link
                }
                if (cap >= 5000000ULL && pace < cap / 10U) {
                    R.frozen_steps++;
                }
            }
        }
        while (sampling && now >= next_send_ns) {
            int64_t unit = -1;
            bool repair = false;
            for (int64_t u = sample_first; u < sample_next; u++) {
                if (want_repair_[u] && !got_[u] && now - last_sent_[u] > srtt + srtt / 2U && sample_next > sample_end) {
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
            if (qn + 1U >= MAX_Q || qn >= g_limit_pkts) {
                R.dropped++;
            } else {
                queue_[qt] = (struct pkt) {now, UNIT_WIRE, unit, 0, 0};
                qt = (qt + 1U) % MAX_Q;
                qn++;
            }
            last_sent_[unit] = now;
            want_repair_[unit] = 0;
            R.sent_units++;
            R.repair_units += repair ? 1U : 0U;
            cand_note_send(now, UNIT_WIRE, repair, !repair);
            uint64_t gap = ((uint64_t)UNIT_WIRE * 8ULL * SEC) / (rate != 0 ? rate : 1U);
            if (next_send_ns + MS < now) {
                next_send_ns = now; // idle: no catching up in a burst
            }
            next_send_ns += gap;
        }
        // 3. a solicit once per RTprop (at least 5 ms) while a sample is open
        if (solicit_open && now - last_solicit_ns > 100 * MS) {
            solicit_open = false;
        }
        if (sampling && now >= next_solicit_ns && !solicit_open) {
            uint64_t rtprop = g_cand == C_ADAPT ? ap_rtprop_ns(&ap_) : srtt;
            next_solicit_ns = now + (rtprop < 5 * MS ? 5 * MS : rtprop);
            solicit_open = true;
            last_solicit_ns = now;
            if (qn + 1U < MAX_Q && qn < g_limit_pkts) {
                queue_[qt] = (struct pkt) {now, SOLICIT_WIRE, -1, now, sample_next - 1};
                qt = (qt + 1U) % MAX_Q;
                qn++;
                R.solicits++;
            }
        }
        // 4. the link serves its queue at its rate
        credit += (int64_t)(L->bps * STEP_NS / SEC);
        while (qn != 0 && credit >= (int64_t)queue_[qh].wire * 8 && (g_slot_ns == 0 || now % g_slot_ns < STEP_NS)) {
            struct pkt p = queue_[qh];
            qh = (qh + 1U) % MAX_Q;
            qn--;
            credit -= (int64_t)p.wire * 8;
            uint32_t loss_now = L->loss_ppm;
            if (g_ge) {
                uint32_t flip = rnd();
                ge_bad = ge_bad ? flip >= g_ge_r : flip < g_ge_p;
                loss_now = ge_bad ? g_ge_bad : g_ge_good;
            }
            if (loss_now != 0 && rnd() < loss_now) {
                R.dropped++;
                continue;
            }
            uint64_t at = now + L->delay_ns;
            at = at < last_fwd_at ? last_fwd_at : at; // no reordering: a delay decrease compresses the flight
            last_fwd_at = at;
            fwd_[ft] = (struct flight) {at, p};
            ft = (ft + 1U) % MAX_DELAY;
            fn++;
        }
        if (qn == 0 && credit > (int64_t)UNIT_WIRE * 8) {
            credit = (int64_t)UNIT_WIRE * 8;
        }
        // 4b. a reader's poll answers a solicit: the report travels back (and may be lost)
        for (uint32_t i = 0; i < pend_n;) {
            if (pending_[i].at_ns > now) {
                i++;
                continue;
            }
            struct pkt q = pending_[i].pkt;
            pending_[i] = pending_[--pend_n];
            uint64_t held_now = 0, mark_now = 0;
            reader_report(sample_first, sample_end, delivered_units, now, srtt + srtt / 2U, &held_now, &mark_now);
            R.fb_bytes += ACKNACK_WIRE;
            R.reports++;
            if (!(L->loss_ppm >= 1000000U) && rnd() >= L->loss_ppm && rn + 1U < MAX_DELAY) {
                uint64_t at = now + L->delay_ns;
                at = at < last_rev_at ? last_rev_at : at;
                last_rev_at = at;
                rev_[rt] = (struct flight) {
                    at, (struct pkt) {q.solicit_ns, 0, (int64_t)held_now, (uint64_t)q.last_unit, (int64_t)mark_now}};
                rt = (rt + 1U) % MAX_DELAY;
                rn++;
            }
        }
        // 4c. a reader with a gap reports by itself at its retry timer
        if (now >= next_unsol_ns) {
            next_unsol_ns = now + UNSOLICITED_NS;
            if (sampling) {
                int64_t highest_got = -1;
                bool gap = false;
                for (int64_t u = sample_first; u <= sample_end; u++) {
                    if (got_[u]) {
                        highest_got = u;
                    } else if (highest_got >= 0) {
                        gap = true;
                        break;
                    }
                }
                if (gap && rn + 1U < MAX_DELAY) {
                    uint64_t held_now = 0, mark_now = 0;
                    reader_report(sample_first, sample_end, delivered_units, now, srtt + srtt / 2U, &held_now,
                                  &mark_now);
                    R.fb_bytes += ACKNACK_WIRE;
                    R.reports++;
                    if (!(L->loss_ppm >= 1000000U) && rnd() >= L->loss_ppm) {
                        uint64_t at = now + L->delay_ns;
                        at = at < last_rev_at ? last_rev_at : at;
                        last_rev_at = at;
                        rev_[rt] = (struct flight) {
                            at, (struct pkt) {0, 0, (int64_t)held_now, (uint64_t)highest_got, (int64_t)mark_now}};
                        rt = (rt + 1U) % MAX_DELAY;
                        rn++;
                    }
                }
            }
        }
        // 5. the reader receives; a solicit makes it report at its next poll
        while (fn != 0 && fwd_[fh].at_ns <= now) {
            struct pkt p = fwd_[fh].pkt;
            fh = (fh + 1U) % MAX_DELAY;
            fn--;
            uint64_t ts = now; // the receiver's arrival stamp
            if (g_jit_ns != 0) {
                int64_t j = (int64_t)(rnd() % (uint32_t)(2 * g_jit_ns / 1000U + 1U)) * 1000 - (int64_t)g_jit_ns;
                ts = (uint64_t)((int64_t)now + j);
            }
            if (p.unit >= 0) {
                if (!got_[p.unit]) {
                    if (trains && sampling && p.unit >= sample_first && p.unit <= sample_end) {
                        if (p.unit - sample_first < (int64_t)g_train_k) {
                            if (tr_n == 0) {
                                tr_tf = ts;
                            }
                            tr_n++;
                            tr_tl = ts;
                        } else if (tr_n != 0 && !tr_ready) {
                            tr_ready = true; // the paced part follows: the train is over
                        }
                    }
                    got_[p.unit] = 1;
                    delivered_units++;
                    if (p.unit >= sample_first && p.unit <= sample_end) {
                        sample_got++;
                    }
                }
                // enhanced-report accumulation (the receiver stamps arrivals; the sender's stamp is the send time)
                if (acc.n_pkts == 0) {
                    acc.t_first = ts;
                }
                acc.n_pkts++;
                acc.bytes += p.wire;
                acc.t_last = ts;
                acc.last_send_ns = p.enq_ns;
                acc.last_arr_ns = ts;
                owd15[owd_n++ % 15U] = (int64_t)ts - (int64_t)p.enq_ns;
                if (p.unit > highest_orig) {
                    if (p.unit > highest_orig + 1 && highest_orig >= 0) {
                        acc.gaps_new += (uint32_t)(p.unit - highest_orig - 1);
                    }
                    highest_orig = p.unit;
                    acc.new_units++;
                }
                int64_t bucket = (int64_t)(p.enq_ns / (5 * MS));
                if (grp_bucket < 0) {
                    grp_bucket = bucket;
                    cur_grp = (struct grp) {p.enq_ns, ts, p.wire};
                } else if (bucket == grp_bucket) {
                    cur_grp.arr_ns = ts;
                    cur_grp.send_ns = p.enq_ns;
                    cur_grp.bytes += p.wire;
                } else if (bucket > grp_bucket) {
                    if (acc.ngroups < MAX_GROUPS) {
                        acc.g[acc.ngroups++] = cur_grp;
                    }
                    grp_bucket = bucket;
                    cur_grp = (struct grp) {p.enq_ns, ts, p.wire};
                }
            } else if (pend_n < 64U) {
                uint64_t poll_at = ((now / TICK_NS) + 1U) * TICK_NS;
                pending_[pend_n++] = (struct flight) {poll_at, p};
            }
        }
        // 5b. model b: an enhanced report every 100 ms (at the reader's poll tick), when anything arrived
        bool fire = (g_cand == C_TRAIN1 || g_cand == C_TRAIN1T) ? (tr_ready && now % TICK_NS < STEP_NS)
                                                                : (now >= next_enh_ns && now % TICK_NS < STEP_NS);
        if (g_enh && g_cand >= C_BBR && fire) {
            next_enh_ns = now + ENH_PERIOD_NS;
            if (acc.n_pkts != 0) {
                if (tr_ready) {
                    acc.train_valid = true;
                    acc.train_n = tr_n;
                    acc.train_k = g_train_k;
                    acc.train_t_first = tr_tf;
                    acc.train_t_last = tr_tl;
                    tr_ready = false;
                    tr_n = 0;
                }
                if (grp_bucket >= 0 && acc.ngroups < MAX_GROUPS) {
                    acc.g[acc.ngroups++] = cur_grp;
                    grp_bucket = -1;
                }
                acc.gen_ns = now;
                int64_t m = owd15[0];
                uint32_t n = owd_n < 15U ? owd_n : 15U;
                for (uint32_t i = 0; i < n; i++) {
                    m = owd15[i] < m ? owd15[i] : m;
                }
                acc.owd_min15_ns = m;
                uint32_t sz = enh_size(acc.ngroups);
                R.fb_bytes += sz + 42U; // plus IP/UDP/Ethernet framing
                R.enh_reports++;
                if (!(L->loss_ppm >= 1000000U) && rnd() >= L->loss_ppm && en < 63U) {
                    uint64_t at = now + L->delay_ns;
                    at = at < last_rev_at ? last_rev_at : at;
                    last_rev_at = at;
                    enhq_[et] = (struct enh_flight) {at, acc};
                    et = (et + 1U) % 64U;
                    en++;
                }
                memset(&acc, 0, sizeof acc);
            }
        }
        // 6. the writer receives
        while (rn != 0 && rev_[rh].at_ns <= now) {
            struct flight f = rev_[rh];
            rh = (rh + 1U) % MAX_DELAY;
            rn--;
            bool answers = f.pkt.enq_ns != 0;
            uint64_t rtt = answers ? now - f.pkt.enq_ns : 0;
            if (answers) {
                srtt = srtt == 0 ? rtt : (srtt * 7U + rtt) / 8U;
            }
            solicit_open = false;
            int64_t reported_highest = (int64_t)f.pkt.solicit_ns;
            for (int64_t u = sample_first; u <= reported_highest && u <= sample_end; u++) {
                if (!got_[u]) {
                    want_repair_[u] = 1;
                }
            }
            struct cur_fb cf = {now, (uint64_t)f.pkt.unit, (uint64_t)f.pkt.last_unit, false, 0};
            if (answers && rtt < SEC) {
                cf.rtt_ok = (sparse_cnt++ % 12U) == 0;
                cf.rtt_ns = rtt;
            }
            cand_cur(&cf);
        }
        while (en != 0 && enhq_[eh].at_ns <= now) {
            struct enh e = enhq_[eh].e;
            eh = (eh + 1U) % 64U;
            en--;
            cand_enh(now, &e);
        }
        // 7. a sample completes
        if (sampling && sample_got == sample_total) {
            if (R.samples_done < 512U) {
                R.lat_ms[R.samples_done] = (double)(now - sample_started) / 1e6;
            }
            R.samples_done++;
            R.got_bits += (double)sample_total * UNIT_WIRE * 8.0;
            sampling = false;
        }
    }
    R.enh_size = 0;
}

// ---- metrics -----------------------------------------------------------------------------------------------------
static double pct_hist(int lo, int hi) {
    uint64_t c = 0;
    for (int i = lo; i < hi && i < 512; i++) {
        c += R.ratio_hist[i];
    }
    return R.steps_counted != 0 ? 100.0 * (double)c / (double)R.steps_counted : 0.0;
}
static double median_ratio(void) {
    uint64_t half = R.steps_counted / 2U, acc = 0;
    for (int i = 0; i < 512; i++) {
        acc += R.ratio_hist[i];
        if (acc >= half) {
            return (double)i / 100.0;
        }
    }
    return 0;
}
// recovery: for each capacity step of at least 1.5x lasting at least 3 s, the time until the rate is within the band
static void recovery(double* up_med, double* down_med, int* nup, int* ndown) {
    double ups[64], downs[64];
    int nu = 0, nd = 0;
    for (uint32_t i = 1; i < g_sn; i++) {
        double a = (double)g_s[i - 1].bps, b = (double)g_s[i].bps;
        uint64_t t_end = i + 1U < g_sn ? g_s[i + 1U].t_ns : g_dur_ns;
        if (t_end - g_s[i].t_ns < 3 * SEC || (b < 1.5 * a && a < 1.5 * b)) {
            continue;
        }
        bool up = b > a;
        double rec = (double)(t_end - g_s[i].t_ns) / 1e9; // censored at the length of the stretch
        for (uint64_t t = g_s[i].t_ns; t < t_end; t += 100 * MS) {
            uint64_t k = t / (100 * MS);
            if (k >= 2048U) {
                break;
            }
            double r = (double)R.rate_kbit[k] * 1000.0;
            if ((up && r >= 0.9 * b) || (!up && r <= 1.1 * b)) {
                rec = (double)(t - g_s[i].t_ns) / 1e9;
                break;
            }
        }
        if (up && nu < 64) {
            ups[nu++] = rec;
        } else if (!up && nd < 64) {
            downs[nd++] = rec;
        }
    }
    int cmp(const void* x, const void* y) {
        return (*(const double*)x > *(const double*)y) - (*(const double*)x < *(const double*)y);
    }
    qsort(ups, (size_t)nu, sizeof(double), cmp);
    qsort(downs, (size_t)nd, sizeof(double), cmp);
    *up_med = nu != 0 ? ups[nu / 2] : -1;
    *down_med = nd != 0 ? downs[nd / 2] : -1;
    *nup = nu;
    *ndown = nd;
}
static double startup_s(void) {
    for (uint32_t k = 0; k < 2048U && (uint64_t)k * 100 * MS < g_dur_ns; k++) {
        if ((double)R.rate_kbit[k] * 1000.0 >= 0.9 * (double)g_s[0].bps) {
            return (double)k / 10.0;
        }
    }
    return (double)g_dur_ns / 1e9;
}

static enum cand parse_cand(const char* s, uint64_t* start) {
    *start = 8000000ULL;
    char b[64];
    snprintf(b, sizeof b, "%s", s);
    char* us = strchr(b, '_');
    if (us != NULL && strcmp(us, "_hi") == 0) {
        *us = 0;
        *start = 62500000ULL;
    }
    if (strcmp(b, "fixed") == 0)
        return C_FIXED;
    if (strcmp(b, "oracle") == 0)
        return C_ORACLE;
    if (strcmp(b, "adapt") == 0)
        return C_ADAPT;
    if (strcmp(b, "bbr3") == 0)
        return C_BBR;
    if (strcmp(b, "nada") == 0)
        return C_NADA;
    if (strcmp(b, "gcc") == 0)
        return C_GCC;
    if (strcmp(b, "train") == 0)
        return C_TRAIN;
    if (strcmp(b, "train1") == 0)
        return C_TRAIN1;
    if (strcmp(b, "train1t") == 0)
        return C_TRAIN1T;
    if (strcmp(b, "traint") == 0)
        return C_TRAINT;
    fprintf(stderr, "unknown candidate %s\n", s);
    exit(2);
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: algosim sched.txt units cand a|b seed\n");
        return 2;
    }
    sched_load(argv[1]);
    g_sample_units = (uint32_t)atoi(argv[2]);
    g_cand = parse_cand(argv[3], &g_start_bps);
    g_enh = argv[4][0] == 'b';
    if (getenv("SLOT_US") != NULL) {
        g_slot_ns = (uint64_t)atoi(getenv("SLOT_US")) * 1000ULL;
    }
    if (getenv("JITTER_US") != NULL) {
        g_jit_ns = (uint64_t)atoi(getenv("JITTER_US")) * 1000ULL;
    }
    g_seed = (uint32_t)atoi(argv[5]);
    lcg_ = g_seed;
    g_ap_start_max = g_start_bps;
    g_ap_sparse = g_enh ? 0U : 12U;
    run();
    double up, down;
    int nu, nd;
    recovery(&up, &down, &nu, &nd);
    double lat[512];
    uint32_t n = R.samples_done < 512U ? R.samples_done : 512U;
    memcpy(lat, R.lat_ms, n * sizeof(double));
    uint32_t undeliv = R.samples_started - R.samples_done;
    uint32_t late = 0;
    for (uint32_t i = 0; i < n; i++) {
        late += lat[i] > 4000.0 ? 1U : 0U;
    }
    double dur_s = (double)g_dur_ns / 1e9;
    printf("med=%.3f in30=%.1f over13=%.1f under07=%.1f over3=%.2f slow10=%.2f gp=%.3f q100=%.2f retx=%.2f fb=%.0f "
           "sent=%llu started=%u done=%u undeliv=%u late=%u up=%.2f down=%.2f nup=%d ndown=%d startup=%.1f lat=",
           median_ratio(), pct_hist(70, 131), pct_hist(131, 512), pct_hist(0, 70),
           R.steps_counted != 0 ? 100.0 * (double)R.over3_steps / (double)R.steps_counted : 0.0,
           R.active_steps != 0 ? 100.0 * (double)R.frozen_steps / (double)R.active_steps : 0.0,
           R.cap_active_bits != 0 ? R.got_bits / R.cap_active_bits : 0.0,
           R.active_steps != 0 ? 100.0 * (double)R.qdelay100_steps / (double)R.active_steps : 0.0,
           R.sent_units != 0 ? 100.0 * (double)R.repair_units / (double)R.sent_units : 0.0, (double)R.fb_bytes / dur_s,
           (unsigned long long)R.sent_units, R.samples_started, R.samples_done, undeliv, late, up, down, nu, nd,
           startup_s());
    for (uint32_t i = 0; i < n; i++) {
        printf("%s%.1f", i != 0 ? "," : "", lat[i]);
    }
    printf("\n");
    return 0;
}

#!/usr/bin/env python3
"""r7_diag_read.py - where R7's copies after delivery come from (C11, 2026-10-30). Diagnostic, not a gate.

Reads rel_small run directories of wifi_burst_loss_netns.sh run with SMALL_TRACE=1 from a tree carrying
r4_trace.patch + r7_diag.patch (CLOCK_REALTIME events, so tv_phases.txt's `date +%s.%N` lines place them):
  echo.trace (the reader of ping's stream): W arrival (seq, targeted), L arrival below the watermark (seq, targeted),
    B a HEARTBEAT eviction moved the watermark (from, to), a ACKNACK sent (base, reason | last << 8), I retry timer
    fired (interval us, retry);
  ping.trace (the writer of that stream): s resend (seq, its resends before), N ACKNACK received (seq, bits).
Every copy that reached the reader below its watermark (L, what R7 counts with the bitmap duplicates) is put in one
class: ABANDONED (the reader had given the sample up on an eviction HEARTBEAT before it came: a repair too late, not a
spurious one), AFTER_RECOVERY (the sample had arrived before: a second copy - its request was answered twice) or OTHER.
For AFTER_RECOVERY copies, how many requests the reader sent naming the sample before it arrived, which of them the
timer sent, and the sample's request-to-recovery time against the reader's timer interval at the time.
"""
import bisect
import collections
import os
import statistics
import sys


def load(p):
    out = []
    try:
        with open(p) as f:
            for line in f:
                k, t, a, b = line.split()
                out.append((k, int(t), int(a), int(b)))
    except OSError:
        pass
    return out


def phases(d):
    ch = []
    try:
        with open(os.path.join(d, "tv_phases.txt")) as f:
            for line in f:
                t, _, _, ph = line.split()
                ch.append((int(float(t) * 1e9), int(ph)))
    except OSError:
        pass
    return ch


def phase_at(ch, t):
    times = [c[0] for c in ch]
    i = bisect.bisect_right(times, t) - 1
    return ch[i][1] if i >= 0 else 0


def q(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else float("nan")


cls = collections.Counter()
by_phase = collections.Counter()
reqs_before = collections.Counter()
timer_reqs = collections.Counter()
rtt_vs_interval = collections.defaultdict(list)
intervals = collections.defaultdict(list)
resend_by_prior = collections.Counter()
for d in sys.argv[1:]:
    echo = load(os.path.join(d, "echo.trace"))
    ping = load(os.path.join(d, "ping.trace"))
    ch = phases(d)
    first_arrival = {}
    abandoned_at = []  # (t, from, to)
    acks = []  # (t, base, last, reason)
    first_named = None
    named_any = True
    for k, t, a, b in echo:
        if k == "A":
            first_named = a if b > 0 else None  # the ACKNACK's first named seq_no (None: it named none); 'a' follows
            named_any = b > 0
        elif k == "W" and a not in first_arrival:
            first_arrival[a] = (t, b)
        elif k == "B":
            abandoned_at.append((t, a, b))
        elif k == "a":
            if named_any:
                acks.append((t, first_named if first_named is not None else a, a + (b >> 8), chr(b & 0xFF)))
            first_named = None
        elif k == "I":
            intervals[phase_at(ch, t)].append(a / 1000.0)
    for k, t, a, b in ping:
        if k == "s":
            resend_by_prior[min(b, 3)] += 1
    for k, t, a, b in echo:
        if k != "L":
            continue
        ph = phase_at(ch, t)
        gone = any(ta <= t and fr <= a < to and a not in first_arrival for ta, fr, to in abandoned_at)
        if a in first_arrival and first_arrival[a][0] <= t:
            c = "AFTER_RECOVERY"
            t_rec = first_arrival[a][0]
            named = [x for x in acks if x[1] <= a <= x[2] and x[0] < t_rec]
            reqs_before[min(len(named), 4)] += 1
            timer_reqs[sum(1 for x in named if x[3] == "T")] += 1
            if named:
                took = (t_rec - named[0][0]) / 1e6
                rtt_vs_interval[ph].append(took)
        elif gone or a not in first_arrival:
            c = "ABANDONED"
        else:
            c = "OTHER"
        cls[c] += 1
        by_phase[(c, ph)] += 1
print(f"copies below the watermark: {dict(cls)}")
print("by tv phase (0 clean, 1 ge5b4, 2 ge10b8 + 20 ms): " +
      ", ".join(f"{c}/{p}={n}" for (c, p), n in sorted(by_phase.items())))
print(f"AFTER_RECOVERY: requests naming the sample before it arrived {dict(sorted(reqs_before.items()))} (4 = 4+); "
      f"of them by the timer {dict(sorted(timer_reqs.items()))}")
for ph in sorted(rtt_vs_interval):
    v = rtt_vs_interval[ph]
    print(f"  phase {ph}: first request -> recovery of a sample copied twice p50 {q(v, .5):.1f} p90 {q(v, .9):.1f} ms "
          f"(n {len(v)}); reader timer interval p50 {q(intervals.get(ph, []), .5):.1f} ms "
          f"(n {len(intervals.get(ph, []))})")
for ph in sorted(intervals):
    if ph not in rtt_vs_interval:
        print(f"  phase {ph}: reader timer interval p50 {q(intervals[ph], .5):.1f} ms (n {len(intervals[ph])})")
print(f"writer resends by the resends before them (3 = 3+): {dict(sorted(resend_by_prior.items()))}")

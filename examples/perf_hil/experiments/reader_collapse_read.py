#!/usr/bin/env python3
"""reader_collapse_read.py - the pre-registered reading of reader_collapse_census.sh (RESULTS `RLC`, 2026-10-26).

QUESTION. On main 81daa880 (the writer-storm fix 8d0d2eed in it), does the READER's own retry estimate still collapse -
a rep whose timer ACKNACKs run far above the cell's median (~80-200) - in which cells, how often, by which mechanism?

Written before the census's run. Applied here, in code:

Per rel_large rep (echo.log RSTATS acknack_tx timer=; ping.log RESULT retransmitted=):
  timer      the reader's timer ACKNACKs.
  BASE       the median rep's timer over every rep of that profile + arm in this run (all labels pooled).
  COLLAPSE   timer > 5 x BASE and timer >= 500 (the floor keeps ETHERNET ge0.1b1, median ~6, from calling 31 a collapse).
  SEVERE     timer > 20 x BASE.
  rate       COLLAPSE reps / reps per profile + arm, with the Clopper-Pearson 95% upper bound; with 0 of n the bound is
             what the run can exclude ((1 - p)^n = 0.05).
TRACE CONTROL: traced labels (T*, E*) against the untraced main tree (U) on the WIFI arms pooled, COLLAPSE reps,
  two-sided Fisher. p < 0.05: TRACE-EFFECT - the hooks change the behaviour, so the traced mechanism is read for traced
  reps only and the rate is taken from U and the historical census only.

MECHANISM (traced reps, sub.trace from r4_trace.patch + k5_add_r_event.py: R a probe on the watermark ended (took_us,
retry | path<<8 | plain<<9 | busy<<10), r srtt/rttvar after it, I a timer ACKNACK (interval_us, retry), A/a ACKNACKs
sent (what they named), U/X units recorded). A timed sample is an R with path=1, busy=0. AMBIGUOUS: an ACKNACK named the
sample's unit before the probe began (the repair answering the older request may already be on its way).
  HYPOTHESIS H, "short-sample lock under the default rule": srtt is pulled under SHORT by a timed sample shorter than
  any round trip - an ambiguous one, the seed or later. The interval (srtt + max(G, 4 rttvar), 3 x srtt after a
  seed) is then so short that an honest probe (>= PATH) runs past tt_RELIABLE_RETRY (3) retries before its repair
  comes and is not timed (the default rule times retry <= 3 only); the only probes timed are again ambiguous short
  ones, so the estimate stays down and the timer fires at the short interval (backed off) for as long as it lasts.
  It ends when a probe a little longer than srtt is timed within 3 retries and ratchets srtt up.
  WINDOW: from the timed sample that took srtt under SHORT to the first one after it that left srtt >= SHORT (or the
  trace's end). A rep may have several; they are summed.
  P1  >= 50% of the rep's timer ACKNACKs fired at an interval < SHORT.
  P2  srtt went under SHORT through a timed sample < SHORT that was AMBIGUOUS.
  P3  inside its windows, no timed sample (the one that closes a window excluded) was >= PATH, and >= 50% of the rep's
      timer ACKNACKs fired inside a window.
  WIFI: SHORT 1000 us, PATH 4000 us (netem 2 ms each way; least repair transit 4.1-4.7 ms, R4K K5).
  VERDICT over the WIFI traced COLLAPSE reps: UNDECIDED with fewer than 2 of them; FALSIFIED when any fails P1 (the
  timer count has another cause than a collapsed interval); SHOWN when every one holds P1, P2 and P3; else PARTIAL
  (the interval collapses, by another route than H's - each failing rep printed).
  CONTROL (not a verdict): the traced non-COLLAPSE reps whose srtt went under SHORT too - how many, their windows'
  timer ACKNACKs and length; H reads the difference as how long the lock held, which the seed's size and how soon a
  ratchet sample comes decide (printed: least interval, window seconds).
  ETHERNET (amp: 1 Gbit/s, 100 us each way, 30 Hz): SHORT 100 us, PATH 200 us, printed per rep, no verdict - the
  thresholds were not measured for that path before this run.
Usage: reader_collapse_read.py BASE_OUT TAG          (the census run: <TAG>_<label>/ and <TAG>_eth_<label>/)
       reader_collapse_read.py --mech DIR...         (per-rep mechanism lines for any traced run dir, e.g. old ones)
"""
import bisect
import collections
import glob
import math
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
FLOORS = {"wifi": (1000, 4000), "ethernet": (100, 200)}


def kv(line):
    return dict(KV.findall(line))


def lines(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


def cp_upper(k, n, alpha=0.05):
    """Clopper-Pearson one-sided-at-alpha/2 upper bound by bisection on the binomial tail."""
    if n == 0:
        return float("nan")
    if k == n:
        return 1.0
    lo, hi = k / n, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        tail = sum(math.comb(n, i) * mid ** i * (1 - mid) ** (n - i) for i in range(0, k + 1))
        if tail > alpha / 2:
            lo = mid
        else:
            hi = mid
    return hi


def fisher_two_sided(a, n1, b, n2):
    k = a + b
    total = math.comb(n1 + n2, k)
    probs = {x: math.comb(n1, x) * math.comb(n2, k - x) / total for x in range(max(0, k - n2), min(k, n1) + 1)}
    p0 = probs.get(a, 0.0)
    return min(1.0, sum(p for p in probs.values() if p <= p0 * (1 + 1e-9)))


def rep_row(d):
    timer = None
    for x in lines(os.path.join(d, "echo.log")):
        if x.startswith("RSTATS: role=sub acknack_tx"):
            timer = int(kv(x).get("timer", 0))
    resends = None
    for x in lines(os.path.join(d, "ping.log")):
        if x.startswith("RESULT"):
            resends = int(kv(x).get("retransmitted", 0))
    meta = kv(" ".join(lines(os.path.join(d, "meta.txt"))))
    return timer, resends, float(meta.get("load", 0) or 0)


def mechanism(trace, short, path):
    ev = []
    with open(trace, errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) == 4 and p[0] in "RrIAaUX":
                ev.append((p[0], int(p[1]), int(p[2]), int(p[3])))
    if not ev:
        return None
    t0 = ev[0][1]
    names = []  # (t, lo, hi)
    pend = None
    timed = []  # (t, took_us, retry, ambiguous, srtt_after, rttvar_after)
    iv = []  # (t, interval_us, retry)
    for i, (k, t, a, b) in enumerate(ev):
        if k == "A":
            pend = (t, a, b)
        elif k == "a" and pend is not None:
            if pend[2] > 0:
                names.append((pend[0], pend[1], a + (b >> 8)))
            pend = None
        elif k == "I":
            iv.append((t, a, b))
        elif k == "R":
            retry, path_, busy = b & 0xFF, (b >> 8) & 1, (b >> 10) & 1
            if not path_ or busy:
                continue
            seq = None
            for k2, _, a2, _ in ev[i + 1:i + 6]:
                if k2 in "UX":
                    seq = a2
                    break
            srtt = rttvar = None
            for k2, _, a2, b2 in ev[i + 1:i + 4]:
                if k2 == "r":
                    srtt, rttvar = a2, b2
                    break
            start = t - a * 1000
            amb = seq is not None and any(lo <= seq <= hi for tn, lo, hi in names if tn < start)
            timed.append((t, a, retry, amb, srtt, rttvar))
    # windows by srtt
    windows = []  # [open_idx, close_idx or None]
    cur = None
    for j, s in enumerate(timed):
        if s[4] is None:
            continue
        if cur is None and s[4] < short:
            cur = [j, None]
        elif cur is not None and s[4] >= short:
            cur[1] = j
            windows.append(cur)
            cur = None
    if cur is not None:
        windows.append(cur)
    t_end = ev[-1][1]
    spans = []
    for o, c in windows:
        spans.append((timed[o][0], timed[c][0] if c is not None else t_end))
    starts = [s for s, _ in spans]

    def in_window(t):
        j = bisect.bisect_right(starts, t) - 1
        return j >= 0 and t < spans[j][1]

    n_iv = len(iv)
    iv_short = sum(1 for _, a, _ in iv if a < short)
    iv_win = sum(1 for t, _, _ in iv if in_window(t))
    iv_win_backoff = sum(1 for t, _, r in iv if in_window(t) and r > 3)
    long_inside = 0
    timed_inside = 0
    amb_inside = 0
    for o, c in windows:
        for j in range(o + 1, c if c is not None else len(timed)):
            timed_inside += 1
            amb_inside += timed[j][3]
            long_inside += timed[j][1] >= path
    openers = [timed[o] for o, _ in windows]
    p2 = any(op[1] < short and op[3] for op in openers)
    closers = [timed[c] for _, c in windows if c is not None]
    seed = timed[0] if timed else None
    return {
        "timer": n_iv, "iv_short": iv_short, "iv_win": iv_win, "iv_win_backoff": iv_win_backoff,
        "min_iv": min((a for _, a, _ in iv), default=-1),
        "windows": len(windows), "win_s": sum(e - s for s, e in spans) / 1e9,
        "first_win_at": (spans[0][0] - t0) / 1e9 if spans else None,
        "timed": len(timed), "timed_amb": sum(1 for s in timed if s[3]),
        "timed_inside": timed_inside, "amb_inside": amb_inside, "long_inside": long_inside,
        "openers": [(round((o[0] - t0) / 1e9, 2), o[1], o[2], int(o[3]), o[4]) for o in openers[:4]],
        "closers": [(round((c[0] - t0) / 1e9, 2), c[1], c[2], int(c[3]), c[4]) for c in closers[:4]],
        "seed": (round((seed[0] - t0) / 1e9, 2), seed[1], seed[2], int(seed[3])) if seed else None,
        "P1": n_iv > 0 and iv_short >= 0.5 * n_iv,
        "P2": p2,
        "P3": bool(windows) and long_inside == 0 and iv_win >= 0.5 * n_iv,
    }


def fmt_mech(m):
    return (f"timer {m['timer']} short-iv {m['iv_short']} in-window {m['iv_win']} (backed-off {m['iv_win_backoff']}) "
            f"min-iv {m['min_iv']}us windows {m['windows']} {m['win_s']:.1f}s first@{m['first_win_at']} "
            f"timed {m['timed']} (amb {m['timed_amb']}) inside {m['timed_inside']} (amb {m['amb_inside']}, "
            f">=PATH {m['long_inside']}) seed {m['seed']} open {m['openers']} close {m['closers']} "
            f"P1 {int(m['P1'])} P2 {int(m['P2'])} P3 {int(m['P3'])}")


def run_dirs(base, tag):
    """(profile, label, dir) for every tree's output of the census."""
    out = []
    for d in sorted(glob.glob(os.path.join(base, f"{tag}_*"))):
        if not os.path.isdir(d):
            continue
        label = os.path.basename(d)[len(tag) + 1:]
        prof = "ethernet" if label.startswith("eth_") else "wifi"
        out.append((prof, label.replace("eth_", ""), d))
    return out


def census(base, tag):
    rows = []
    for prof, label, d in run_dirs(base, tag):
        for r in sorted(glob.glob(os.path.join(d, "runs", "*"))):
            arm, cell, rep = os.path.basename(r).rsplit("-", 2)
            if cell != "rel_large":
                continue
            timer, resends, load = rep_row(r)
            if timer is None:
                print(f"VOID rep {label} {arm}-{rep}: no reader RSTATS")
                continue
            rows.append({"prof": prof, "label": label, "arm": arm, "rep": rep, "timer": timer, "resends": resends,
                         "load": load, "dir": r, "traced": os.path.exists(os.path.join(r, "sub.trace"))})
    base_of = {}
    for key in {(x["prof"], x["arm"]) for x in rows}:
        base_of[key] = statistics.median([x["timer"] for x in rows if (x["prof"], x["arm"]) == key])
    for x in rows:
        b = base_of[(x["prof"], x["arm"])]
        x["collapse"] = x["timer"] > 5 * b and x["timer"] >= 500
        x["severe"] = x["timer"] > 20 * b
    print("OCCURRENCE (rel_large, COLLAPSE: timer > 5 x BASE and >= 500; SEVERE > 20 x):")
    for key in sorted(base_of):
        v = [x for x in rows if (x["prof"], x["arm"]) == key]
        k = sum(x["collapse"] for x in v)
        t = sorted(x["timer"] for x in v)
        hi = sorted(((x["timer"], x["label"], x["rep"], x["resends"]) for x in v if x["collapse"]), reverse=True)
        print(f"  {key[0]:8} {key[1]:8} reps {len(v):3} BASE {base_of[key]:.0f} max {t[-1]} COLLAPSE {k} "
              f"SEVERE {sum(x['severe'] for x in v)} rate {k / len(v):.3f} (95% upper {cp_upper(k, len(v)):.3f}) "
              f"loaded>8 {sum(1 for x in v if x['load'] > 8)} {hi}")
    w = [x for x in rows if x["prof"] == "wifi"]
    tr = [x for x in w if x["label"] != "U"]
    un = [x for x in w if x["label"] == "U"]
    a, b = sum(x["collapse"] for x in tr), sum(x["collapse"] for x in un)
    p = fisher_two_sided(a, len(tr), b, len(un)) if tr and un else float("nan")
    print(f"TRACE CONTROL (WIFI, traced vs U): {a}/{len(tr)} vs {b}/{len(un)}, two-sided p {p:.3f} -> "
          f"{'TRACE-EFFECT' if p < 0.05 else 'PASS'}")
    print("MECHANISM per traced rep (COLLAPSE first):")
    coll, ctrl = [], []
    for x in sorted(rows, key=lambda r: (not r["collapse"], r["prof"], r["arm"], r["label"], int(r["rep"]))):
        if not x["traced"]:
            continue
        short, path = FLOORS[x["prof"]]
        m = mechanism(os.path.join(x["dir"], "sub.trace"), short, path)
        if m is None:
            print(f"  {x['prof']} {x['arm']} {x['label']}-{x['rep']}: empty trace")
            continue
        if x["prof"] == "wifi":
            (coll if x["collapse"] else ctrl).append((x, m))
        if x["collapse"] or m["windows"]:
            print(f"  {'COLLAPSE' if x['collapse'] else 'normal  '} {x['prof']} {x['arm']} {x['label']}-{x['rep']} "
                  f"resends {x['resends']}: {fmt_mech(m)}")
    print(f"CONTROL: WIFI traced normal reps {len(ctrl)}; srtt under SHORT in {sum(1 for _, m in ctrl if m['windows'])}; "
          f"their in-window timer ACKNACKs median "
          f"{statistics.median([m['iv_win'] for _, m in ctrl if m['windows']] or [0])}, "
          f"window s median {statistics.median([m['win_s'] for _, m in ctrl if m['windows']] or [0]):.2f}; "
          f"collapse reps' window s {[round(m['win_s'], 1) for _, m in coll]}")
    if len(coll) < 2:
        verdict = f"UNDECIDED ({len(coll)} traced WIFI COLLAPSE reps)"
    elif not all(m["P1"] for _, m in coll):
        verdict = "FALSIFIED (P1 fails: " + ", ".join(f"{x['arm']} {x['label']}-{x['rep']}" for x, m in coll
                                                      if not m["P1"]) + ")"
    elif all(m["P1"] and m["P2"] and m["P3"] for _, m in coll):
        verdict = f"SHOWN ({len(coll)} of {len(coll)} hold P1-P3)"
    else:
        verdict = "PARTIAL (" + ", ".join(f"{x['arm']} {x['label']}-{x['rep']} P2 {int(m['P2'])} P3 {int(m['P3'])}"
                                          for x, m in coll if not (m["P2"] and m["P3"])) + ")"
    print(f"VERDICT MECHANISM H: {verdict}")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--mech":
        for d in sys.argv[2:]:
            prov = " ".join(lines(os.path.join(d, "provenance.txt")))
            prof = "ethernet" if "profile=ethernet" in prov else "wifi"
            short, path = FLOORS[prof]
            for tr in sorted(glob.glob(os.path.join(d, "runs", "*", "sub.trace"))):
                r = os.path.dirname(tr)
                timer, resends, _ = rep_row(r)
                m = mechanism(tr, short, path)
                if m is not None:
                    print(f"{os.path.basename(d)} {os.path.basename(r)} timer {timer} resends {resends}: {fmt_mech(m)}")
        return
    census(sys.argv[1], sys.argv[2])


main()

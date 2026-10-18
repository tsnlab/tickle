#!/usr/bin/env python3
"""Read i4be_close_gap.sh's runs: does each A/B switch of ab/i4be-cpu-candidates lower rmw_tickle's CPU per MB in I4
best effort (Array4m, BEST_EFFORT KEEP_LAST 5, 15 Hz) without harm?

Input: <OUT>.runs (rmw_keepall_rig.sh with PERF_ATTACH; arms tickle@head, tickle@aa, fastdds and candidates). The per
run figures come from i4be_perf_attach_read.run(): perf stat --per-thread summed over perf_test's threads over the
attach window, divided by the samples in it (the subscriber's steady rate x the window), with its VOID rules (pstat
missing or an event not supported, subscriber rate off 15 Hz by more than 1%, perf's task-clock against the process's
own ticks off by more than 10% + 2 ticks a thread).

HOW TO READ IT - written 2026-10-18 before the first run, enforced below:
  TREATMENT  (VOID run otherwise - "could not look" is never "on")
    tickle@thp      both logs print rmw_tickle's "large pool hugepage=1 ... advised=<a> anon_huge_kb=<k>" with a >= 1 and
                    k >= 2048 (at least one whole huge page realized, not only advised).
    tickle@ntcopy, tickle@revcopy, tickle@ntrev
                    the subscriber prints "take_copy=<1|2|3> take_copies_streamed=<n>" with its own mode and n at least
                    90% of the samples it received (the switch took the copy, not the decoder).
    every other tickle arm: no "hugepage=1" and no take_copy line in either log (the control got no switch).
  PER RUN    each side's task-clock (ms per sample; per MB = / 4.194304), cycles and instructions :u and :k, kernel IPC
             (instructions:k / cycles:k), and latency: the median over steady seconds of perf_test's latency_mean (PC:
             one clock; rig: two clocks, so its absolute value carries their offset, the same for every arm of a rep).
  FLOOR      per side and metric, max(|median(aa) - median(head)|, 2 x SE of median(C) - median(head)) (SE over reps).
  METRIC     d = median(C) - median(head): BETTER below -floor, WORSE above +floor, else HELD.
  PRIMARY    sub task-clock for the copy candidates (they act in the subscriber's take only); sub AND pub task-clock
             for tickle@thp (both ends use the pool: reassembly at one, the sample's buffer at the other).
  NO HARM    latency median not WORSE; and the arm has no more rate-VOID runs than tickle@head (a switch that costs
             delivery shows as runs off 15 Hz). Either failing is HARM.
  VERDICT    per candidate: CANDIDATE when every primary is BETTER (or one BETTER and none WORSE for thp) and no HARM;
             REFUTED when a primary is WORSE or there is HARM; else NO EFFECT at this resolution. Fewer than 2 usable
             runs of the candidate, tickle@head or tickle@aa: NOT READ. Beside it, the share of the head - fastdds gap
             the candidate closes, (head - C) / (head - fastdds), on each primary.
  DIAGNOSTIC (printed, not judged) the copy candidates' claim is a warmer reassembly buffer: sub cycles:k lower and
             kernel IPC higher at about the same instructions:k; the cost to watch is sub cycles:u (the copy itself).
             tickle@thp's claim is fewer TLB misses in the kernel's copy-in and the take's copy-out: cycles:k and
             cycles:u lower at the same instructions.
  On the PC (x86, 42 MB L3) the 4 MB buffer fits the cache, so NO EFFECT for the copy candidates is the expected
  reading there and says nothing about the Pi 5 (2 MB L3); the PC reading is about treatment, harm and cost.
"""
import math
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import i4be_perf_attach_read as par  # noqa: E402
from rmw_keepall_rig_summary import rows, sample_bytes, whole_counts  # noqa: E402

MB = par.SIZE / 1e6
COPY_MODE = {"tickle@ntcopy": 1, "tickle@revcopy": 2, "tickle@ntrev": 3}
CANDIDATES = ("tickle@thp", "tickle@ntcopy", "tickle@revcopy", "tickle@ntrev")
POOL = re.compile(r"large pool hugepage=(\d) allocated=(\d+) advised=(\d+) anon_huge_kb=(-?\d+)")
TAKE = re.compile(r"take_copy=(\d+) take_copies_streamed=(\d+)")


def text_of(path):
    try:
        return path.read_text(errors="replace")
    except OSError:
        return None


def received_total(sub_log):
    rs, _ = rows(sub_log)
    whole_counts(rs, sample_bytes("Array4m"))
    return sum(r.get("n_received", 0) for r in rs)


def treatment(runs, stem, arm):
    """'' when the arm's treatment is realized (and the control's absent), else why not."""
    logs = {side: text_of(runs / f"{stem}_{side}.log") for side in ("pub", "sub")}
    if any(t is None for t in logs.values()):
        return "a log is missing"
    if arm == "tickle@thp":
        for side, t in logs.items():
            m = POOL.search(t)
            if not m:
                return f"{side}: no large pool line"
            hp, _, adv, kb = (int(x) for x in m.groups())
            if hp != 1 or adv < 1 or kb < 2048:
                return f"{side}: hugepage={hp} advised={adv} anon_huge_kb={kb}"
    elif arm in COPY_MODE:
        m = TAKE.search(logs["sub"])
        if not m:
            return "sub: no take_copy line"
        mode, n = int(m.group(1)), int(m.group(2))
        got = received_total(runs / f"{stem}_sub.log")
        if mode != COPY_MODE[arm] or n < 0.9 * got:
            return f"sub: take_copy={mode} take_copies_streamed={n} of {got:.0f} received"
    if arm != "tickle@thp" and arm.startswith("tickle@"):
        for side, t in logs.items():
            m = POOL.search(t)
            if m and m.group(1) == "1":
                return f"{side}: control shows hugepage=1"
    if arm not in COPY_MODE and arm.startswith("tickle@") and TAKE.search(logs["sub"]):
        return "sub: control shows a take_copy line"
    return ""


def latency(runs, stem):
    rs, _ = rows(runs / f"{stem}_sub.log")
    whole_counts(rs, sample_bytes("Array4m"))
    s = par.steady(rs, "n_received")
    if not s:
        return float("nan")
    live = [r["latency_mean (ms)"] for r in rs if r.get("n_received", 0) > 0 and "latency_mean (ms)" in r]
    live = live[par.WARM:-par.COOL] if len(live) > par.WARM + par.COOL else live
    return st.median(live) if live else float("nan")


def read_set(runs, window):
    arms, voids, rate_voids = {}, [], {}
    for sub in sorted(runs.glob("*_Array4m_l0_r*_sub.log")):
        stem = sub.name[: -len("_sub.log")]
        arm = stem.split("_Array4m")[0]
        why = treatment(runs, stem, arm)
        if why:
            voids.append(f"VOID {stem}: treatment - {why}")
            continue
        rec, why = par.run(runs, stem, window)
        if rec is None:
            voids.append(f"VOID {stem}: {why}")
            if "rate" in why:
                rate_voids[arm] = rate_voids.get(arm, 0) + 1
            continue
        rec["lat_ms"] = latency(runs, stem)
        for side in ("pub", "sub"):
            ck = rec.get(f"{side}_cycles:k", 0.0)
            rec[f"{side}_ipc_k"] = rec.get(f"{side}_instructions:k", 0.0) / ck if ck else float("nan")
            cu = rec.get(f"{side}_cycles:u", 0.0)
            rec[f"{side}_ipc_u"] = rec.get(f"{side}_instructions:u", 0.0) / cu if cu else float("nan")
        arms.setdefault(arm, []).append(rec)
    return arms, voids, rate_voids


def judge(arms, cand, key):
    h, a, c = arms["tickle@head"], arms["tickle@aa"], arms[cand]
    d = par.med(c, key) - par.med(h, key)
    floor = max(abs(par.med(a, key) - par.med(h, key)), 2 * math.sqrt(par.se(c, key) ** 2 + par.se(h, key) ** 2))
    v = "BETTER" if d < -floor else "WORSE" if d > floor else "HELD"
    return d, floor, v


def main():
    runs = Path(sys.argv[1])
    window = par.window_of(runs)
    if window is None:
        print(f"NOT READ: no perf_attach= in {runs}/meta.txt")
        return 1
    for line in (runs / "meta.txt").read_text().splitlines():
        if line.startswith("perf_events="):
            par.EVENTS = tuple(line.split("=", 1)[1].split(","))
    arms, voids, rate_voids = read_set(runs, window)
    print(f"== {runs} (window {window:.0f} s, events {','.join(par.EVENTS)})")
    for v in voids:
        print(v)
    print("usable runs: " + ", ".join(f"{k} {len(v)}" for k, v in sorted(arms.items())))
    keys = ["task-clock", "cycles:k", "instructions:k", "ipc_k", "cycles:u", "instructions:u", "ipc_u",
            "context-switches"]
    if "exc_irq" in par.EVENTS:
        keys += ["exc_svc", "exc_irq"]
    order = [a for a in ("tickle@head", "tickle@aa", *CANDIDATES, "fastdds") if a in arms]
    for side in ("sub", "pub"):
        print(f"\n--- {side}: medians per sample (task-clock ms; also ms/MB)")
        print(f"{'metric':<18}" + "".join(f"{a:>16}" for a in order))
        for k in keys:
            vals = [par.med(arms[a], f"{side}_{k}") for a in order]
            fmt = (lambda x: f"{x:16.3f}") if k in ("task-clock", "ipc_k", "ipc_u") else (lambda x: f"{x:16,.0f}")
            print(f"{k:<18}" + "".join(fmt(x) for x in vals))
        print(f"{'task-clock ms/MB':<18}" + "".join(f"{par.med(arms[a], f'{side}_task-clock') / MB:16.4f}"
                                                   for a in order))
    print(f"{'latency med ms':<18}" + "".join(f"{par.med(arms[a], 'lat_ms'):16.2f}" for a in order))

    if not all(len(arms.get(x, [])) >= 2 for x in ("tickle@head", "tickle@aa")):
        print("\nNOT READ: fewer than 2 usable runs of tickle@head or tickle@aa")
        return 1
    print("\n=== verdicts (d = candidate - head; floor = max(|aa - head|, 2 SE))")
    for cand in CANDIDATES:
        if cand not in arms and cand not in rate_voids:
            continue
        if len(arms.get(cand, [])) < 2:
            print(f"{cand}: NOT READ ({len(arms.get(cand, []))} usable runs)")
            continue
        primaries = ("sub", "pub") if cand == "tickle@thp" else ("sub",)
        verdicts = []
        for side in primaries:
            d, floor, v = judge(arms, cand, f"{side}_task-clock")
            gap = par.med(arms["tickle@head"], f"{side}_task-clock") - par.med(arms.get("fastdds", []),
                                                                               f"{side}_task-clock")
            share = -d / gap if gap and not math.isnan(gap) else float("nan")
            print(f"{cand} PRIMARY {side} task-clock: {d / MB:+.4f} ms/MB (floor {floor / MB:.4f}) {v}; "
                  f"closes {share:+.0%} of the head - fastdds gap ({gap / MB:+.4f} ms/MB)")
            verdicts.append(v)
            for k in ("cycles:k", "instructions:k", "ipc_k", "cycles:u", "instructions:u"):
                dd, ff, vv = judge(arms, cand, f"{side}_{k}")
                print(f"    diagnostic {side} {k}: {dd:+,.3f} (floor {ff:,.3f}) {vv}")
        dl, fl, vl = judge(arms, cand, "lat_ms")
        harm = []
        if vl == "WORSE":
            harm.append(f"latency {dl:+.2f} ms (floor {fl:.2f})")
        if rate_voids.get(cand, 0) > rate_voids.get("tickle@head", 0):
            harm.append(f"{rate_voids[cand]} rate-VOID runs against head's {rate_voids.get('tickle@head', 0)}")
        print(f"    no-harm: latency {dl:+.2f} ms (floor {fl:.2f}) {vl}; "
              f"rate-VOID runs {rate_voids.get(cand, 0)} (head {rate_voids.get('tickle@head', 0)})")
        if harm or "WORSE" in verdicts:
            final = "REFUTED" + (" (HARM: " + "; ".join(harm) + ")" if harm else "")
        elif all(v == "BETTER" for v in verdicts) or ("BETTER" in verdicts and "WORSE" not in verdicts
                                                     and cand == "tickle@thp"):
            final = "CANDIDATE"
        else:
            final = "NO EFFECT at this resolution"
        print(f"  {cand}: {final}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

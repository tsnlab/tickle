#!/usr/bin/env python3
"""Read pub_hugepage_ab.sh's runs: do huge pages for the publisher's large buffers (ab/pub-hugepage) lower the
publisher's CPU per MB in I4 best effort against main, without harm?

Usage: pub_hugepage_read.py <A.runs> <B.runs> --realized <1|0>
  A.runs  phase A, I4 best effort (Array4m, BEST_EFFORT KEEP_LAST 5, 15 Hz) with PERF_ATTACH
  B.runs  phase B, the no-harm cells at 30 Hz: I4s (Array4m, saturating) and I1 (Array1m)
  --realized 1 (rig): a treated publisher must also show the huge page realized (AnonHugePages); 0 (PC): only advised,
          because whether a huge page is free at fault time is the host's memory state, not the change.
Arms: tickle@pre = main (baseline B), tickle@head = the change (T), tickle@aa = the change again (A/A), fastdds.

HOW TO READ IT - written 2026-10-18 before the first run, enforced below:
  TREATMENT  per run, from rmw_tickle's own line at shutdown, "large pool publish_hugepage=<h> allocated=<n>
             publish_allocated=<p> advised=<a> anon_huge_kb=<k>" (VOID otherwise - "could not look" is never "on"):
    tickle@head, tickle@aa  publisher: h=1, p>=1, and for an Array4m cell a>=1 (its 4 MB buffer is above one huge
                    page) and with --realized 1 k>=2048 (a whole huge page realized, not only advised); for Array1m a=0
                    (1 MB is below one huge page: the rule leaves it alone). Subscriber: h=1, p=0 and a=0 (a reassembly
                    buffer is never advised).
    tickle@pre      no such line in either log (main prints none): a line means the arm ran the change's binary.
  PER RUN    A: perf stat --per-thread over the attach window, per sample (i4be_perf_attach_read.run(): its VOID rules -
             a .pstat missing or an event unsupported, the subscriber's steady rate off 15 Hz by more than 1%, perf's
             task-clock against the process's own ticks off by more than 10% + 2 ticks a thread); per MB = / 4.194304.
             A and B: largemsg_rig_summary.load() - rusage CPU per MB (cpu_pub, cpu_sub) over the steady seconds, the
             clock-corrected latency median (lat_med), delivery (offered_pct; incomplete = lost > 0 or nothing
             received). A run that summary marks VOID (identity: the library loaded) is dropped.
  FLOOR      per cell and metric, max(|median(aa) - median(head)|, 2 x SE of median(head) - median(pre)), SE over reps.
  METRIC     d = median(head) - median(pre), lower is better (higher for offered_pct): BETTER beyond -floor, WORSE
             beyond +floor, else HELD.
  PRIMARY    A: the publisher's task-clock per MB (perf).
  NO HARM    each WORSE is HARM:
             A   the subscriber's task-clock per MB (perf); latency; more incomplete or rate-VOID runs than pre;
             I4s the publisher's and the subscriber's rusage CPU per MB; latency; offered_pct;
             I1  the same four (and more incomplete runs than pre). I1 cannot be touched by the change (1 MB < 2 MiB),
                 so a WORSE there is harm from the bookkeeping or drift - either way not a landing.
             A metric with fewer than 3 usable values in head, aa or pre is NOT READ, and a NOT READ no-harm check keeps
             the verdict from LAND (a check that could not look did not pass).
  VERDICT    LAND when the primary is BETTER, no HARM and nothing NOT READ; REFUTED when the primary is WORSE or there is
             HARM; else NO EFFECT at this resolution (or NOT READ when the primary itself is). Fewer than 3 usable
             phase A runs of pre, head or aa: NOT READ.
  BESIDE IT  (printed, not judged) the share of main's publisher gap to Fast DDS the change closes, (pre - head) /
             (pre - fastdds); fastdds's own medians in each phase (drift); the publisher's cycles:k, cycles:u and
             instructions (the claim is fewer TLB misses in the encode and in the kernel's copy out of the buffer:
             cycles down at about the same instructions); the previous reading to compare with, both-ends huge pages
             on 2026-10-18: pub -0.122 ms/MB (floor 0.035), sub +0.058 (floor 0.037).
"""
import argparse
import math
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import i4be_perf_attach_read as par  # noqa: E402
import largemsg_rig_summary as lrs  # noqa: E402

MB4 = par.SIZE / 1e6
POOL = re.compile(r"large pool publish_hugepage=(\d) allocated=(\d+) publish_allocated=(\d+) advised=(\d+) "
                  r"anon_huge_kb=(-?\d+)")
BASE, TREAT, AA = "tickle@pre", "tickle@head", "tickle@aa"
MIN_RUNS = 3


def text_of(path):
    try:
        return path.read_text(errors="replace")
    except OSError:
        return None


def treatment(runs, stem, arm, topic, realized):
    """'' when the arm's treatment is realized as the rules say, else why not."""
    logs = {side: text_of(runs / f"{stem}_{side}.log") for side in ("pub", "sub")}
    if any(t is None for t in logs.values()):
        return "a log is missing"
    if arm == BASE:
        for side, t in logs.items():
            if POOL.search(t):
                return f"{side}: the baseline printed a large pool line (the change's binary)"
        return ""
    if arm not in (TREAT, AA):
        return ""
    lines = {}
    for side, t in logs.items():
        found = POOL.findall(t)
        if not found:
            return f"{side}: no large pool line"
        lines[side] = tuple(int(x) for x in found[-1])
    h, _, p, a, k = lines["pub"]
    if h != 1 or p < 1:
        return f"pub: publish_hugepage={h} publish_allocated={p}"
    if topic == "Array4m":
        if a < 1:
            return f"pub: advised={a} for a 4 MB buffer"
        if realized and k < 2048:
            return f"pub: advised={a} but anon_huge_kb={k} (not realized)"
    elif a != 0:
        return f"pub: advised={a} for a buffer below one huge page"
    h, _, p, a, _ = lines["sub"]
    if h != 1 or p != 0 or a != 0:
        return f"sub: publish_hugepage={h} publish_allocated={p} advised={a} (a reassembly buffer advised)"
    return ""


def pool_text(runs, stem):
    out = []
    for side in ("pub", "sub"):
        t = text_of(runs / f"{stem}_{side}.log") or ""
        m = POOL.findall(t)
        out.append(f"{side} " + ("/".join(m[-1]) if m else "-"))
    return " ".join(out)


def summary_records(runs, realized, voids):
    """{(arm, topic): [record]} from largemsg_rig_summary, treatment-checked."""
    _, _, recs = lrs.load(runs)
    out = {}
    for r in recs:
        stem = f"{r['arm']}_{r['topic']}_l{r['loss']}_r{r['rep']}"
        if r["void"]:
            voids.append(f"VOID {runs.name}/{stem}: {r['void']}")
            continue
        why = treatment(runs, stem, r["arm"], r["topic"], realized)
        if why:
            voids.append(f"VOID {runs.name}/{stem}: treatment - {why}")
            continue
        r["pool"] = pool_text(runs, stem)
        out.setdefault((r["arm"], r["topic"]), []).append(r)
    return out


def perf_records(runs, realized, voids):
    window = par.window_of(runs)
    if window is None:
        voids.append(f"NOT READ: no perf_attach= in {runs}/meta.txt")
        return {}, {}
    for line in (runs / "meta.txt").read_text().splitlines():
        if line.startswith("perf_events="):
            par.EVENTS = tuple(line.split("=", 1)[1].split(","))
    arms, rate_voids = {}, {}
    for sub in sorted(runs.glob("*_Array4m_l0_r*_sub.log")):
        stem = sub.name[: -len("_sub.log")]
        arm = stem.split("_Array4m")[0]
        why = treatment(runs, stem, arm, "Array4m", realized)
        if why:
            voids.append(f"VOID {runs.name}/{stem} (perf): treatment - {why}")
            continue
        rec, why = par.run(runs, stem, window)
        if rec is None:
            voids.append(f"VOID {runs.name}/{stem} (perf): {why}")
            if "rate" in why:
                rate_voids[arm] = rate_voids.get(arm, 0) + 1
            continue
        arms.setdefault(arm, []).append(rec)
    return arms, rate_voids


def values(recs, key):
    return [r[key] for r in recs if key in r and isinstance(r[key], (int, float)) and not math.isnan(r[key])]


def judge(groups, key, higher_better=False):
    """(d, floor, verdict) with verdict BETTER/WORSE/HELD, or NOT READ."""
    b, t, a = (values(groups.get(x, []), key) for x in (BASE, TREAT, AA))
    if min(len(b), len(t), len(a)) < MIN_RUNS:
        return float("nan"), float("nan"), f"NOT READ ({len(b)}/{len(t)}/{len(a)} values pre/head/aa)"
    d = st.median(t) - st.median(b)
    se = math.sqrt((st.stdev(t) ** 2 / len(t)) + (st.stdev(b) ** 2 / len(b)))
    floor = max(abs(st.median(a) - st.median(t)), 2 * se)
    good = d > floor if higher_better else d < -floor
    bad = d < -floor if higher_better else d > floor
    return d, floor, "BETTER" if good else "WORSE" if bad else "HELD"


def med(recs, key):
    v = values(recs, key)
    return st.median(v) if v else float("nan")


def table(groups, keys, order, title):
    print(f"\n--- {title}: medians")
    print(f"{'metric':<22}" + "".join(f"{a:>14}" for a in order) + f"{'n':>12}")
    for k, scale, fmt in keys:
        row = "".join(f"{med(groups.get(a, []), k) * scale:14{fmt}}" for a in order)
        n = "/".join(str(len(values(groups.get(a, []), k))) for a in order)
        print(f"{k:<22}{row}{n:>12}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a_runs")
    ap.add_argument("b_runs")
    ap.add_argument("--realized", type=int, choices=(0, 1), required=True)
    args = ap.parse_args()
    a_runs, b_runs, realized = Path(args.a_runs), Path(args.b_runs), args.realized == 1
    voids = []
    perf, rate_voids = perf_records(a_runs, realized, voids)
    summ_a = summary_records(a_runs, realized, voids) if (a_runs / "index.txt").exists() else {}
    summ_b = summary_records(b_runs, realized, voids) if (b_runs / "index.txt").exists() else {}
    print(f"== A {a_runs}\n== B {b_runs}\n   realized required: {int(realized)}")
    for v in voids:
        print(v)
    print("usable perf runs (A): " + ", ".join(f"{k} {len(v)}" for k, v in sorted(perf.items())))
    for name, s in (("A", summ_a), ("B", summ_b)):
        print(f"usable summary runs ({name}): " + ", ".join(f"{k[0]}@{k[1]} {len(v)}" for k, v in sorted(s.items())))
    order = [a for a in (BASE, TREAT, AA, "fastdds") if a in perf or any(k[0] == a for k in summ_a)]

    # The treatment as realized, per arm and cell (first usable run's line).
    for name, s in (("A", summ_a), ("B", summ_b)):
        for (arm, topic), recs in sorted(s.items()):
            if arm.startswith("tickle@"):
                print(f"  treatment {name} {arm}@{topic}: {recs[0]['pool']}")

    keys = [("task-clock", 1 / MB4, ".4f"), ("cycles:k", 1, ",.0f"), ("cycles:u", 1, ",.0f"),
            ("instructions:k", 1, ",.0f"), ("instructions:u", 1, ",.0f")]
    for side in ("pub", "sub"):
        table(perf, [(f"{side}_{k}", s, f) for k, s, f in keys], order, f"A perf, {side} (task-clock in ms/MB)")
    lkeys = [("cpu_pub", 1, ".4f"), ("cpu_sub", 1, ".4f"), ("lat_med", 1, ".2f"), ("offered_pct", 1, ".2f"),
             ("rate_hz", 1, ".2f")]
    for name, s, topic in (("A I4be", summ_a, "Array4m"), ("B I4s", summ_b, "Array4m"), ("B I1", summ_b, "Array1m")):
        groups = {arm: recs for (arm, t), recs in s.items() if t == topic}
        table(groups, lkeys, order, f"{name} summary (CPU in ms/MB, rusage)")

    if min(len(perf.get(x, [])) for x in (BASE, TREAT, AA)) < MIN_RUNS:
        print(f"\nNOT READ: fewer than {MIN_RUNS} usable phase A perf runs of pre, head or aa")
        return 1

    print("\n=== verdict (d = head - pre; floor = max(|aa - head|, 2 SE))")
    d, floor, primary = judge(perf, "pub_task-clock")
    gap = med(perf.get(BASE, []), "pub_task-clock") - med(perf.get("fastdds", []), "pub_task-clock")
    share = -d / gap if gap and not math.isnan(gap) else float("nan")
    print(f"PRIMARY A pub task-clock: {d / MB4:+.4f} ms/MB (floor {floor / MB4:.4f}) {primary}; closes {share:+.0%} "
          f"of main's publisher gap to fastdds ({gap / MB4:+.4f} ms/MB)")
    for k in ("cycles:k", "cycles:u", "instructions:k", "instructions:u"):
        dd, ff, vv = judge(perf, f"pub_{k}")
        print(f"    diagnostic pub {k}: {dd:+,.0f} (floor {ff:,.0f}) {vv}")

    harm, not_read = [], []

    def check(label, groups, key, scale=1.0, higher_better=False):
        dd, ff, vv = judge(groups, key, higher_better)
        print(f"  no-harm {label}: {dd * scale:+.4f} (floor {ff * scale:.4f}) {vv}")
        if vv == "WORSE":
            harm.append(label)
        elif vv.startswith("NOT READ"):
            not_read.append(label)

    check("A sub task-clock ms/MB", perf, "sub_task-clock", 1 / MB4)
    a4 = {arm: recs for (arm, t), recs in summ_a.items() if t == "Array4m"}
    check("A latency ms", a4, "lat_med")
    for arm_set, label in ((a4, "A"),):
        inc_t = sum(1 for r in arm_set.get(TREAT, []) if r["incomplete"])
        inc_b = sum(1 for r in arm_set.get(BASE, []) if r["incomplete"])
        print(f"  no-harm {label} incomplete runs: head {inc_t}, pre {inc_b}; rate-VOID (perf) head "
              f"{rate_voids.get(TREAT, 0)}, pre {rate_voids.get(BASE, 0)}")
        if inc_t > inc_b:
            harm.append(f"{label} incomplete runs")
        if rate_voids.get(TREAT, 0) > rate_voids.get(BASE, 0):
            harm.append(f"{label} rate-VOID runs")
    for name, topic in (("I4s", "Array4m"), ("I1", "Array1m")):
        groups = {arm: recs for (arm, t), recs in summ_b.items() if t == topic}
        check(f"{name} pub CPU ms/MB", groups, "cpu_pub")
        check(f"{name} sub CPU ms/MB", groups, "cpu_sub")
        check(f"{name} latency ms", groups, "lat_med")
        check(f"{name} offered %", groups, "offered_pct", higher_better=True)
        if name == "I1":
            inc_t = sum(1 for r in groups.get(TREAT, []) if r["incomplete"])
            inc_b = sum(1 for r in groups.get(BASE, []) if r["incomplete"])
            print(f"  no-harm I1 incomplete runs: head {inc_t}, pre {inc_b}")
            if inc_t > inc_b:
                harm.append("I1 incomplete runs")
    for name, s in (("A", summ_a), ("B", summ_b)):
        for topic in ("Array4m", "Array1m"):
            f = s.get(("fastdds", topic))
            if f:
                print(f"  fastdds {name}@{topic} (drift control): cpu_pub {med(f, 'cpu_pub'):.4f} cpu_sub "
                      f"{med(f, 'cpu_sub'):.4f} lat_med {med(f, 'lat_med'):.2f} offered {med(f, 'offered_pct'):.2f}")
    print("  previous reading (both ends, 2026-10-18 i4be_close_rig_20261018-102207): pub -0.1217 ms/MB (floor 0.0354), "
          "sub +0.0576 (floor 0.0367)")

    if primary.startswith("NOT READ"):
        final = "NOT READ (primary)"
    elif primary == "WORSE" or harm:
        final = "REFUTED" + (f" (HARM: {', '.join(harm)})" if harm else "")
    elif primary == "BETTER" and not not_read:
        final = "LAND"
    elif primary == "BETTER":
        final = f"NOT READ (no-harm checks that could not look: {', '.join(not_read)})"
    else:
        final = "NO EFFECT at this resolution"
    print(f"\nVERDICT: {final}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

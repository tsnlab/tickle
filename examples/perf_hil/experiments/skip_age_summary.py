#!/usr/bin/env python3
"""skip_age_summary.py RUNS ARM... - the reading of skip_age_pc.sh, whose header states every rule implemented here.

perf_test's subscriber rows with received > 0, the first 2 and last 2 of them dropped; delivered/s = sum(received) /
sum(T_loop); CPU per delivered sample = the getrusage delta over those rows / sum(received); sample age = latency_mean
(perf_test prints it in ms to 1 ns) weighted by received, reported in us.
"""
import math
import re
import statistics
import sys
from pathlib import Path


def mean_se(xs):
    xs = [x for x in xs if x is not None and not math.isnan(x)]
    if not xs:
        return float("nan"), float("nan"), 0
    if len(xs) < 2:
        return xs[0], float("nan"), 1
    return statistics.mean(xs), statistics.stdev(xs) / math.sqrt(len(xs)), len(xs)


def perf_rows(text):
    rows, header = [], None
    for line in text.splitlines():
        cells = [c.strip() for c in line.split(",")]
        if cells and cells[0] == "T_experiment":
            header = cells
            continue
        if header is None or len(cells) < len(header) - 1:
            continue
        try:
            rows.append({h: float(c) for h, c in zip(header, cells) if h})
        except ValueError:
            continue
    return rows


def run(d):
    try:
        text = (d / "sub.log").read_text(errors="replace")
    except OSError:
        return None
    rows = perf_rows(text)
    live = [i for i, r in enumerate(rows) if r.get("received", 0) > 0]
    if len(live) < 6:
        return None
    idx = live[2:-2]
    first, last = idx[0], idx[-1]
    window = rows[first:last + 1]
    recv = sum(r["received"] for r in window)
    secs = sum(r["T_loop"] for r in window)
    cpu = lambda r: r["ru_utime"] + r["ru_stime"]  # noqa: E731
    out = dict(rate=recv / secs / 1e3, cpu_us=(cpu(rows[last]) - cpu(rows[first - 1])) / recv * 1e6,
               age_us=sum(r["latency_mean (ms)"] * r["received"] for r in window) / recv * 1e3)
    skipped = re.findall(r"rx_shm_skipped_superseded=(\d+)", text)
    out["skipped"] = int(skipped[-1]) if skipped else None
    prof = re.findall(r"drain_profile: (.*)", text)
    if prof:
        out["prof"] = {k: float(v) for k, v in (kv.split("=") for kv in prof[-1].split())}
    return out


def fmt(t, digits=2):
    m, se, n = t
    return f"{m:.{digits}f} +- {se:.{digits}f} (n={n})"


def profile_line(runs_of_arm):
    keys = ["drains", "plan_calls", "plan_cyc", "plan_records", "skiphead_n", "skiphead_cyc", "read_n", "read_cyc",
            "kl_n", "kl_pre_cyc", "kl_dec_cyc"]
    ps = [r["prof"] for r in runs_of_arm if "prof" in r]
    if not ps:
        return None
    hz = statistics.mean(p["tsc_hz"] for p in ps)
    tot = {k: sum(p[k] for p in ps) for k in keys}
    ns = lambda c: c / hz * 1e9  # noqa: E731
    div = lambda a, b: a / b if b else float("nan")  # noqa: E731
    return (f"    profile (n={len(ps)}, tsc {hz / 1e9:.3f} GHz): per drain {div(tot['plan_records'], tot['plan_calls']):.1f}"
            f" records planned, plan {div(ns(tot['plan_cyc']), tot['plan_calls']):.0f} ns/call;"
            f" skipped head {div(ns(tot['skiphead_cyc']), tot['skiphead_n']):.1f} ns/record"
            f" ({div(tot['skiphead_n'], tot['kl_n']):.2f} per KL delivery);"
            f" read {div(ns(tot['read_cyc']), tot['read_n']):.0f} ns/record ({div(tot['read_n'], tot['drains']):.2f}"
            f" per drain); kl_pre {div(ns(tot['kl_pre_cyc']), tot['kl_n']):.0f} ns, kl_dec"
            f" {div(ns(tot['kl_dec_cyc']), tot['kl_n']):.0f} ns (kl_n={tot['kl_n']:.0f})")


def main():
    runs = Path(sys.argv[1])
    arms = sys.argv[2:]
    void = []
    cells = sorted({d.name.split("_")[0] for d in runs.iterdir() if d.is_dir()})
    for msg in cells:
        print(f"\n== {msg} BEST_EFFORT KEEP_LAST 1, -r 0")
        res, raw = {}, {}
        for arm in arms + ["cyclonedds"]:
            reps = []
            for d in sorted(runs.glob(f"{msg}_{arm}_r*"), key=lambda p: int(p.name.rsplit("_r", 1)[1])):
                r = run(d)
                if r is None:
                    void.append(f"{d.name}: fewer than 6 delivering rows")
                    continue
                if arm != "cyclonedds":
                    if arm == "main" and r["skipped"] not in (None, 0):
                        void.append(f"{d.name}: main skipped {r['skipped']}")
                        continue
                    if arm != "main" and not r["skipped"]:
                        void.append(f"{d.name}: no rx_shm_skipped_superseded > 0 (treatment absent)")
                        continue
                reps.append(r)
            if not reps:
                continue
            raw[arm] = reps
            res[arm] = {k: mean_se([r[k] for r in reps]) for k in ("rate", "cpu_us", "age_us")}
            sk = mean_se([r["skipped"] for r in reps if r["skipped"] is not None])
            print(f"  {arm:12s} age {fmt(res[arm]['age_us'], 3)} us  delivered {fmt(res[arm]['rate'], 1)} k/s"
                  f"  CPU {fmt(res[arm]['cpu_us'], 3)} us/sample" + (f"  skipped/run {sk[0]:,.0f}" if sk[2] else ""))
            pl = profile_line(reps)
            if pl:
                print(pl)
            if arm != "cyclonedds":
                print("    age per rep: " + " ".join(f"{r['age_us']:.3f}" for r in reps))
        drift = False
        if "cyclonedds" in raw and len(raw["cyclonedds"]) >= 4:
            xs = [r["rate"] for r in raw["cyclonedds"]]
            h = len(xs) // 2
            a, b = mean_se(xs[:h]), mean_se(xs[h:])
            drift = abs(a[0] - b[0]) > 2 * math.hypot(a[1], b[1])
            print(f"  control cyclonedds halves {a[0]:.1f} / {b[0]:.1f} k/s -> {'DRIFT' if drift else 'stable'}")
        for arm in arms:
            if arm in ("main", "skip") or arm not in res or "main" not in res or "skip" not in res:
                continue
            if min(res[a]["age_us"][2] for a in (arm, "main", "skip")) < 3:
                print(f"  {arm}: NO VERDICT (n < 3)")
                continue

            def within(x, ref, key, worse_if_higher):
                (mx, sx, _), (mr, sr, _) = res[x][key], res[ref][key]
                lim = 2 * math.hypot(sx, sr)
                return mx <= mr + lim if worse_if_higher else mx >= mr - lim

            verdicts = [("age<=main", within(arm, "main", "age_us", True)),
                        ("delivered>=skip", within(arm, "skip", "rate", False)),
                        ("cpu<=skip", within(arm, "skip", "cpu_us", True))]
            print(f"  TARGET {arm}: " + "  ".join(f"{k}: {'MET' if v else 'NOT MET'}" for k, v in verdicts)
                  + ("  (DRIFT)" if drift else ""))
        if "main" in res and "skip" in raw and any("prof" in r for r in raw["skip"]):
            excess = res["skip"]["age_us"][0] - res["main"]["age_us"][0]
            ps = [r["prof"] for r in raw["skip"] if "prof" in r]
            hz = statistics.mean(p["tsc_hz"] for p in ps)
            kl_pre = sum(p["kl_pre_cyc"] for p in ps) / max(sum(p["kl_n"] for p in ps), 1) / hz * 1e6
            verdict = "FALSIFIED" if kl_pre < excess / 3 else "consistent"
            print(f"  HYPOTHESIS: skip age excess {excess:.3f} us, skip kl_pre {kl_pre:.3f} us -> {verdict}")
    if void:
        print("\nVOID runs:")
        for v in void:
            print("  " + v)


if __name__ == "__main__":
    main()

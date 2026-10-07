#!/usr/bin/env python3
"""skip_newest_summary.py RUNS - the reading of skip_newest_pc.sh, whose header states every rule implemented here.

rmw cells: perf_test's subscriber rows with received > 0, the first 2 and the last 2 of them dropped (start-up and
shut-down); delivered/s = sum(received) / sum(T_loop); subscriber CPU per delivered sample = the getrusage delta over
those rows / sum(received); sample age = perf_test's latency_mean weighted by received. Native cells: the RESULT lines,
recv / the client's elapsed_s for throughput, rtt_avg_ms for latency.
"""
import math
import re
import statistics
import sys
from pathlib import Path

ARMS = ["main", "one", "skip"]
RMW_CELLS = [("Array1k", "best_effort"), ("Array4k", "best_effort"), ("Array1k", "reliable")]
NATIVE_CELLS = [("best_effort_throughput", "p3"), ("best_effort_throughput", "p4"),
                ("reliable_latency", "p2"), ("reliable_latency", "p4")]


def mean_se(xs):
    xs = [x for x in xs if x is not None and not math.isnan(x)]
    if not xs:
        return float("nan"), float("nan"), 0
    if len(xs) < 2:
        return xs[0], float("nan"), 1
    return statistics.mean(xs), statistics.stdev(xs) / math.sqrt(len(xs)), len(xs)


def perf_rows(path):
    rows, header = [], None
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return [], ""
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
    return rows, text


def rmw_run(d):
    rows, text = perf_rows(d / "sub.log")
    live = [i for i, r in enumerate(rows) if r.get("received", 0) > 0]
    if len(live) < 6:
        return None
    idx = live[2:-2]
    first, last = idx[0], idx[-1]
    window = rows[first:last + 1]
    recv = sum(r["received"] for r in window)
    secs = sum(r["T_loop"] for r in window)
    cpu = lambda r: r["ru_utime"] + r["ru_stime"]  # noqa: E731
    cpu_s = cpu(rows[last]) - cpu(rows[first - 1])
    lat = sum(r["latency_mean (ms)"] * r["received"] for r in window) / recv if recv else float("nan")
    skipped = re.findall(r"rx_shm_skipped_superseded=(\d+)", text)
    return dict(rate=recv / secs, cpu_us=cpu_s / recv * 1e6, age_ms=lat,
                skipped=int(skipped[-1]) if skipped else None)


def result_fields(path):
    try:
        for line in path.read_text(errors="replace").splitlines():
            if line.startswith("RESULT:"):
                return dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
    except OSError:
        pass
    return None


def native_run(d, scen):
    cli, srv = result_fields(d / "cli.log"), result_fields(d / "srv.log")
    if not cli or not srv:
        return None
    skipped = int(srv.get("rx_shm_skipped_superseded", cli.get("rx_shm_skipped_superseded", "-1")))
    if scen == "best_effort_throughput":
        recv = int(srv["recv"])
        return dict(value=recv / float(cli["elapsed_s"]), cpu_us=float(srv["sched_cpu_s"]) / max(recv, 1) * 1e6,
                    skipped=skipped, worse_if="lower")
    if int(cli.get("measured", "0")) < 1000:
        return None  # sample count before value: a handful of round trips is not a latency figure
    return dict(value=float(cli["rtt_avg_ms"]) * 1e3, cpu_us=float("nan"), skipped=skipped, worse_if="higher")


def fmt(m, se, n, scale=1.0, digits=1):
    return f"{m * scale:.{digits}f} +- {se * scale:.{digits}f} (n={n})"


def main():
    runs = Path(sys.argv[1])
    void = []
    print("rmw cells (perf_test -r 0, same host, private netns)")
    control_drift = False
    for msg, qos in RMW_CELLS:
        print(f"\n== {msg} {qos}")
        res = {}
        for arm in ARMS + ["cyclonedds"]:
            reps = []
            for d in sorted(runs.glob(f"rmw_{msg}_{qos}_{arm}_r*")):
                r = rmw_run(d)
                if r is None:
                    void.append(f"{d.name}: fewer than 6 delivering rows")
                    continue
                if arm == "skip":
                    if r["skipped"] is None:
                        void.append(f"{d.name}: no rx_shm_skipped_superseded on the traffic line (treatment unseen)")
                        continue
                    if qos == "best_effort" and r["skipped"] == 0:
                        void.append(f"{d.name}: skipped=0 on a KEEP_LAST 1 cell (treatment not applied)")
                        continue
                    if qos == "reliable" and r["skipped"] != 0:
                        void.append(f"{d.name}: skipped={r['skipped']} on a KEEP_ALL cell")
                        continue
                reps.append(r)
            res[arm] = reps
            rate = mean_se([r["rate"] for r in reps])
            cpu = mean_se([r["cpu_us"] for r in reps])
            age = mean_se([r["age_ms"] for r in reps])
            sk = [r["skipped"] for r in reps if r["skipped"] is not None]
            print(f"  {arm:10} delivered {fmt(*rate, 1e-3)} k/s  sub CPU {fmt(*cpu, 1, 3)} us/sample  "
                  f"age {fmt(*age, 1, 4)} ms" + (f"  skipped/run {statistics.mean(sk):,.0f}" if sk else ""))
        cyc = [r["rate"] for r in res["cyclonedds"]]
        if len(cyc) >= 4:
            h = len(cyc) // 2
            a, b = mean_se(cyc[:h]), mean_se(cyc[h:])
            drift = abs(a[0] - b[0]) > 2 * math.hypot(a[1], b[1])
            control_drift = control_drift or drift
            print(f"  control cyclonedds halves {a[0] / 1e3:.1f} / {b[0] / 1e3:.1f} k/s -> {'DRIFT' if drift else 'stable'}")
        if any(len(res[a]) < 3 for a in ARMS):
            print("  NO VERDICT: n < 3 for an arm")
            continue
        m = {a: {k: mean_se([r[k] for r in res[a]]) for k in ("rate", "cpu_us", "age_ms")} for a in ARMS}
        if qos == "best_effort":
            age_ok = m["skip"]["age_ms"][0] <= m["main"]["age_ms"][0] + 2 * math.hypot(m["skip"]["age_ms"][1],
                                                                                       m["main"]["age_ms"][1])
            rate_ok = m["skip"]["rate"][0] >= m["one"]["rate"][0] - 2 * math.hypot(m["skip"]["rate"][1],
                                                                                   m["one"]["rate"][1])
            cpu_ok = m["skip"]["cpu_us"][0] <= m["one"]["cpu_us"][0] + 2 * math.hypot(m["skip"]["cpu_us"][1],
                                                                                      m["one"]["cpu_us"][1])
            print(f"  TARGET age<=main: {'MET' if age_ok else 'NOT MET'}  delivered>=one: {'MET' if rate_ok else 'NOT MET'}"
                  f"  cpu<=one: {'MET' if cpu_ok else 'NOT MET'}{'  (DRIFT)' if control_drift else ''}")
        else:
            worse = m["skip"]["rate"][0] < m["main"]["rate"][0] - 2 * math.hypot(m["skip"]["rate"][1],
                                                                                m["main"]["rate"][1])
            print(f"  NO REGRESSION vs main (delivered/s): {'WORSE' if worse else 'PASS'}")

    print("\nnative bench cells (same host, private netns; tickle only)")
    for scen, size in NATIVE_CELLS:
        print(f"\n== {scen} {size}")
        res = {}
        for arm in ARMS:
            reps = []
            for d in sorted(runs.glob(f"nat_{scen}_{size}_{arm}_r*")):
                r = native_run(d, scen)
                if r is None:
                    void.append(f"{d.name}: no RESULT line, or fewer than 1000 measured round trips")
                    continue
                if arm == "skip" and r["skipped"] != 0:
                    void.append(f"{d.name}: skipped={r['skipped']} where every Subscriber keeps depth 0")
                    continue
                reps.append(r)
            res[arm] = reps
            v = mean_se([r["value"] for r in reps])
            c = mean_se([r["cpu_us"] for r in reps])
            unit = "k/s" if scen == "best_effort_throughput" else "us rtt"
            scale = 1e-3 if scen == "best_effort_throughput" else 1.0
            print(f"  {arm:6} {fmt(*v, scale)} {unit}" + (f"  server CPU {fmt(*c, 1, 3)} us/recv"
                                                         if scen == "best_effort_throughput" else ""))
        if any(len(res[a]) < 3 for a in ("main", "skip")):
            print("  NO VERDICT: n < 3")
            continue
        s, mn = mean_se([r["value"] for r in res["skip"]]), mean_se([r["value"] for r in res["main"]])
        band = 2 * math.hypot(s[1], mn[1])
        worse = s[0] < mn[0] - band if scen == "best_effort_throughput" else s[0] > mn[0] + band
        print(f"  NO REGRESSION vs main: {'WORSE' if worse else 'PASS'}")
    if void:
        print("\nVOID runs:")
        for v in void:
            print("  " + v)


if __name__ == "__main__":
    main()

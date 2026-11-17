#!/usr/bin/env python3
"""Markdown tables for WIFI_DIAG_RESULTS from t1pi_wdiag.py `dir` output. Usage: t1pi_wdiag_tables.py c DIR | b DIR_1M DIR_128K
State class of a run = the STA's minstrel best rate at the end of the run: '1.0M' -> slow, anything else -> fast (the sampler's last line)."""
import json, subprocess, sys, os, statistics

HERE = os.path.dirname(os.path.abspath(__file__))


def runs(d):
    out = subprocess.run([sys.executable, os.path.join(HERE, "t1pi_wdiag.py"), "dir", d], capture_output=True, text=True).stdout
    return [json.loads(l) for l in out.splitlines() if l.strip()]


def f(v, p=0):
    return "-" if v is None else ("%.*f" % (p, v) if isinstance(v, float) else str(v))


def med(v):
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None


def cls(r):
    return "slow" if r.get("rc_sta_best_last") == "1.0M" else "fast"


def table_c(d):
    print("| arm | run | delivered/15 | STA rate start>end | STA att at start | recv frames/s busy p50 | peak 100 ms (fps) | 20 s mean fps / cv | peer reg | retransmitted | hybrid runs | disc dg first 10 s (c) | tx_udp_bcast | summary_copies |")
    print("|---|---|---:|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|")
    for r in runs(d):
        print("| %s | %s | %s | %s>%s | %s | %s | %s | %s / %s | %s | %s | %s | %s | %s | %s |" % (r["arm"], r["run"].split("_")[0], r.get("delivered"), r.get("rc_sta_best_first"), r.get("rc_sta_best_last"),
              r.get("rc_sta_sum_att_first"), r.get("recv_busy_1s_p50"), r.get("recv_peak_100ms_fps"), f(r.get("win20_mean_fps")), f(r.get("win20_cv"), 2), r.get("peer_registered_pub"),
              r.get("retransmitted"), r.get("hybrid_runs"), r["ctl_c"]["disc_first10s"] if "ctl_c" in r else "-", r.get("tx_udp_broadcast"), r.get("summary_copies")))


def table_b(dirs):
    rows = []
    for d, sz in dirs:
        for r in runs(d):
            r["size"] = sz
            rows.append(r)
    print("| size | arm | fw | run | STA rate | done/seen | compl p50 / max ms | units/sample | frames/dg max | send amplification | burst frames p50/p90/max | intra-burst gap us | busy recv fps p50 | peak 100 ms fps | repair interval s (NACK p50 / burst p50) |")
    print("|---|---|---|---|---|---:|---|---:|---:|---:|---|---:|---:|---:|---|")
    for r in sorted(rows, key=lambda r: (r["size"], r["arm"], r["fw"], r["run"])):
        if "error" in r:
            print("| %s | %s | %s | %s | %s |" % (r["size"], r["arm"], r["fw"], r["run"], r["error"]))
            continue
        print("| %s | %s | %s | %s | %s | %s/%s | %s / %s | %s | %s | %s | %s/%s/%s | %s | %s | %s | %s / %s |" % (
            r["size"], r["arm"], r["fw"], r["run"].split("_")[0], cls(r), r["n_done"], r["n_samples_seen"], f(r.get("compl_ms_p50")), f(r.get("compl_ms_max")), r["units_per_sample"],
            r["dg_nframes_max"], f(r.get("send_amplification"), 2), r["burst_frames_p50"], r["burst_frames_p90"], r["burst_frames_max"], f(r["intra_gap_us_p50"]),
            r.get("recv_busy_1s_p50"), r["recv_peak_100ms_fps"], f(r.get("nack_interval_s_p50"), 2), f(r.get("burst_interval_s_p50"), 2)))
    print("\nBy framework and state class (median over runs):\n")
    print("| size | fw | class | n | done/seen med | compl p50 ms med | amplification med | burst p50 med | gap us med | busy fps med | NACK interval med s |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    keys = sorted(set((r["size"], r["fw"], cls(r)) for r in rows if "error" not in r))
    for k in keys:
        v = [r for r in rows if "error" not in r and (r["size"], r["fw"], cls(r)) == k]
        print("| %s | %s | %s | %d | %s/%s | %s | %s | %s | %s | %s | %s |" % (k[0], k[1], k[2], len(v), f(med([x["n_done"] for x in v])), f(med([x["n_samples_seen"] for x in v])), f(med([x.get("compl_ms_p50") for x in v])),
              f(med([x.get("send_amplification") for x in v]), 2), f(med([x["burst_frames_p50"] for x in v])), f(med([x["intra_gap_us_p50"] for x in v])), f(med([x.get("recv_busy_1s_p50") for x in v])), f(med([x.get("nack_interval_s_p50") for x in v]), 2)))


if __name__ == "__main__":
    if sys.argv[1] == "c":
        table_c(sys.argv[2])
    else:
        table_b([(sys.argv[2], "1 MiB"), (sys.argv[3], "128 KiB")])

#!/usr/bin/env python3
"""batch_stage0_read.py <results dir> - reads batch_stage0.sh's per-run files and applies the rule REGISTERED on
2026-11-12 in ~/claude_reports/SMALL_SAMPLE_BATCHING_STAGE0_2026-11-12.md before any rig run.

Rule (fixed): a cell in {16,100,1000 B CDR} x {flood, 10k} PASSES iff, on the medians of its 5 reps,
  1. B datagrams/sample <= 0.5
  2. B IP-level wire bytes/sample <= 0.90 x A's
  3. B delivery % >= A delivery % - |median(A) - median(A2)| (A/A2 spread of delivery %, no floor)
  4. B sender CPU s per million samples <= A's
Design stage only if >= 5 of those 6 cells PASS. 1 kHz cells are reported only. Latency is reported, never a criterion.
VOID cell: an arm with fewer than 4 of its reps having both RESULT lines, or the client's tx_udp vs eth0 tx_packets
cross-check off by more than 2 % in any counted rep (instrument fail), or (paced cells) an arm whose median sent rate is
not within 5 % of the nominal rate (the arms would not be the same load; added 2026-11-12 before the full run, after the
smoke run showed perf_client's -i does not pace). A VOID cell is neither pass nor fail; if fewer
than 5 cells remain decidable, the verdict is UNDECIDED.
"""
import glob
import os
import re
import statistics as st
import sys

MIN_REPS = 4
RATE_TOL = 0.05
NOMINAL = {"10k": 10000.0, "1k": 1000.0}
XCHECK_TOL = 0.02
CLK = 100.0


def num(s):
    return float(s.replace(",", ""))


def parse(path):
    d = {}
    ifc = {}
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("IFC_"):
            p = line.split()
            ifc[p[0]] = [int(x) for x in p[1:5]]
        elif line.startswith("CLI_TIME"):
            m = re.search(r"user=([\d.]+) sys=([\d.]+)", line)
            d["cli_cpu"] = float(m.group(1)) + float(m.group(2))
        elif line.startswith("CLI_RESULT"):
            d["sent"] = num(re.search(r"sent=([\d,]+)", line).group(1))
            d["tx_udp"] = num(re.search(r"tx_udp=(\d+)", line).group(1))
            d["buffer_full"] = num(re.search(r"buffer_full=([\d,]+)", line).group(1))
        elif "messages sent" in line:
            d["elapsed"] = float(re.search(r"MB, ([\d.]+) sec", line).group(1))
        elif line.startswith("SRV_RESULT"):
            d["recv"] = num(re.search(r"recv=([\d,]+)", line).group(1))
            d["lat_avg_res"] = float(re.search(r"avg_latency_ms=([\d.]+)", line).group(1))
        elif line.startswith("SRV_one-way"):
            m = re.search(r"= ([\d.]+)/([\d.]+)/([\d.]+) ms", line)
            d["lat_min"], d["lat_avg"], d["lat_max"] = (float(m.group(i)) for i in (1, 2, 3))
        elif line.startswith("SRV_PROC") and "utime_ticks" in line:
            m = re.search(r"utime_ticks=(\d+) stime_ticks=(\d+)", line)
            d["srv_cpu"] = (int(m.group(1)) + int(m.group(2))) / CLK
    if not all(k in d for k in ("sent", "recv", "tx_udp", "cli_cpu", "elapsed")):
        return None
    if "IFC_CLI_BEFORE" not in ifc or "IFC_CLI_AFTER" not in ifc:
        return None
    b, a = ifc["IFC_CLI_BEFORE"], ifc["IFC_CLI_AFTER"]
    dp, db = a[0] - b[0], a[1] - b[1]
    d["ifc_pkts"] = dp
    d["xcheck"] = abs(dp - d["tx_udp"]) / max(d["tx_udp"], 1)
    d["dgram_per_sample"] = d["tx_udp"] / d["sent"]
    d["ip_bytes_per_sample"] = (db - 14 * dp) / d["sent"]
    d["ip_bytes_per_dgram"] = (db - 14 * dp) / max(dp, 1)
    d["deliv_pct"] = 100.0 * d["recv"] / d["sent"]
    d["deliv_rate"] = d["recv"] / d["elapsed"]
    d["sent_rate"] = d["sent"] / d["elapsed"]
    d["cli_cpu_per_M"] = d["cli_cpu"] / (d["sent"] / 1e6)
    if "srv_cpu" in d:
        d["srv_cpu_per_M"] = d["srv_cpu"] / (d["recv"] / 1e6) if d["recv"] else float("nan")
    return d


def med(runs, k):
    v = [r[k] for r in runs if k in r]
    return st.median(v) if v else float("nan")


def pad4(n):
    return (n + 3) // 4 * 4


def arithmetic():
    out = ["", "ARITHMETIC BOUND (classic layout: 4 B header once + k x (4 + 16 + pad4(CDR)); k=1 is the single header,"
           " 4 + 16 + CDR; IP bytes include 20 IP + 8 UDP per datagram; max datagram payload 1472)"]
    for c in (16, 100, 1000):
        out.append(f"  CDR {c} B:")
        kmax = 1
        while 4 + (kmax + 1) * (20 + pad4(c)) <= 1472:
            kmax += 1
        for k in sorted({1, 2, 3, 4, 8, 16, 32, 64, kmax}):
            if k > kmax:
                continue
            payload = (4 + 16 + c) if k == 1 else 4 + k * (20 + pad4(c))
            out.append(f"    k={k:3d}: datagram payload {payload:4d} B, IP bytes/sample {(28 + payload) / k:7.1f},"
                       f" datagrams/sample {1 / k:.4f}" + ("   <- max k" if k == kmax else ""))
    return out


def main(d):
    runs = {}
    for f in sorted(glob.glob(os.path.join(d, "s*_r*_*.txt"))):
        m = re.match(r"(s\d+_\w+?)_r(\d+)_(A2|A|B)\.txt$", os.path.basename(f))
        if not m:
            continue
        r = parse(f)
        if r is not None:
            runs.setdefault((m.group(1), m.group(3)), []).append(r)
    cells = [f"s{s}_{m}" for s in (16, 100, 1000) for m in ("flood", "10k", "1k")]
    out = []
    P = out.append
    P(f"batch_stage0 verdict from {d}")
    P("per cell medians (n reps in brackets). dg/s = datagrams per sample, ipB/s = IP-level wire bytes per sample,"
      " del% = delivery, kS/s = delivered samples/s, cpuC/cpuS = sender/receiver CPU s per million samples,"
      " lat = one-way avg ms (min/max)")
    P(f"{'cell':12s} {'arm':3s} {'n':>3s} {'dg/s':>8s} {'ipB/s':>8s} {'B/dgram':>8s} {'del%':>8s} {'kS/s':>9s}"
      f" {'cpuC':>7s} {'cpuS':>7s} {'lat avg':>8s} {'xchk max':>8s} {'sent/s':>9s}")
    stats = {}
    for c in cells:
        for arm in ("A", "A2", "B"):
            rs = runs.get((c, arm), [])
            stats[(c, arm)] = rs
            if not rs:
                P(f"{c:12s} {arm:3s}   0  (no valid runs)")
                continue
            P(f"{c:12s} {arm:3s} {len(rs):3d} {med(rs, 'dgram_per_sample'):8.4f} {med(rs, 'ip_bytes_per_sample'):8.2f}"
              f" {med(rs, 'ip_bytes_per_dgram'):8.1f} {med(rs, 'deliv_pct'):8.3f} {med(rs, 'deliv_rate') / 1e3:9.2f}"
              f" {med(rs, 'cli_cpu_per_M'):7.3f} {med(rs, 'srv_cpu_per_M'):7.3f} {med(rs, 'lat_avg'):8.3f}"
              f" {max(r['xcheck'] for r in rs):8.4f} {med(rs, 'sent_rate'):9.1f}")
    P("")
    P("RULE per cell (flood and 10k cells decide; 1k reported only)")
    passes = fails = voids = 0
    for c in cells:
        decides = not c.endswith("_1k")
        A, A2, B = stats[(c, "A")], stats[(c, "A2")], stats[(c, "B")]
        void = ""
        for arm, rs in (("A", A), ("A2", A2), ("B", B)):
            if len(rs) < MIN_REPS:
                void = f"VOID ({arm} has {len(rs)} valid reps < {MIN_REPS})"
            elif max(r["xcheck"] for r in rs) > XCHECK_TOL:
                void = f"VOID ({arm}: client tx_udp vs eth0 tx_packets differ by {max(r['xcheck'] for r in rs):.3f})"
        nom = NOMINAL.get(c.split("_")[1])
        if not void and nom:
            for arm, rs in (("A", A), ("A2", A2), ("B", B)):
                if abs(med(rs, "sent_rate") - nom) > RATE_TOL * nom:
                    void = f"VOID ({arm} sent {med(rs, 'sent_rate'):.0f} samples/s, nominal {nom:.0f})"
        if void:
            P(f"  {c:12s} {void}" + ("" if decides else "  [reported only]"))
            voids += decides
            continue
        c1 = med(B, "dgram_per_sample") <= 0.5
        c2 = med(B, "ip_bytes_per_sample") <= 0.90 * med(A, "ip_bytes_per_sample")
        spread = abs(med(A, "deliv_pct") - med(A2, "deliv_pct"))
        c3 = med(B, "deliv_pct") >= med(A, "deliv_pct") - spread
        c4 = med(B, "cli_cpu_per_M") <= med(A, "cli_cpu_per_M")
        ok = c1 and c2 and c3 and c4
        wire_gain = 100.0 * (1 - med(B, "ip_bytes_per_sample") / med(A, "ip_bytes_per_sample"))
        P(f"  {c:12s} {'PASS' if ok else 'FAIL'}  c1 dg/s<=0.5 {med(B, 'dgram_per_sample'):.4f} {'ok' if c1 else 'NO'};"
          f" c2 wire -{wire_gain:.1f}% (need >=10) {'ok' if c2 else 'NO'};"
          f" c3 deliv B {med(B, 'deliv_pct'):.3f} vs A {med(A, 'deliv_pct'):.3f} spread {spread:.3f} {'ok' if c3 else 'NO'};"
          f" c4 cpuC B {med(B, 'cli_cpu_per_M'):.3f} vs A {med(A, 'cli_cpu_per_M'):.3f}"
          f" (A2 {med(A2, 'cli_cpu_per_M'):.3f}) {'ok' if c4 else 'NO'}" + ("" if decides else "  [reported only]"))
        if decides:
            passes += ok
            fails += not ok
    decidable = passes + fails
    if decidable < 5:
        verdict = "UNDECIDED (fewer than 5 decidable flood/10k cells)"
    elif passes >= 5:
        verdict = "PASS: the candidate goes to a design stage"
    else:
        verdict = "FAIL: STOP"
    P(f"VERDICT: {passes} of the 6 flood/10k cells PASS, {fails} FAIL, {voids} VOID -> {verdict}")
    P("")
    P("LATENCY COST of B (one-way ms from perf_server; only B minus A is meaningful, clock offset cancels; never a criterion)")
    for c in cells:
        A, B = stats[(c, "A")], stats[(c, "B")]
        if A and B:
            P(f"  {c:12s} A avg {med(A, 'lat_avg'):8.3f} (max {med(A, 'lat_max'):8.3f})  B avg {med(B, 'lat_avg'):8.3f}"
              f" (max {med(B, 'lat_max'):8.3f})  B-A {med(B, 'lat_avg') - med(A, 'lat_avg'):+8.3f} ms")
    P("")
    P("MEASURED samples per datagram (1/dg per sample) and spread across reps (min..max) for dg/s and ipB/s")
    for c in cells:
        for arm in ("A", "B"):
            rs = stats[(c, arm)]
            if rs:
                dg = [r["dgram_per_sample"] for r in rs]
                ib = [r["ip_bytes_per_sample"] for r in rs]
                P(f"  {c:12s} {arm}: samples/datagram {1 / med(rs, 'dgram_per_sample'):7.2f}  dg/s {min(dg):.4f}..{max(dg):.4f}"
                  f"  ipB/s {min(ib):.2f}..{max(ib):.2f}  buffer_full median {med(rs, 'buffer_full'):.0f}")
    out += arithmetic()
    print("\n".join(out))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: batch_stage0_read.py <results dir>")
    main(sys.argv[1])

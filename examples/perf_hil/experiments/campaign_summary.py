#!/usr/bin/env python3
"""Summarise campaign_sweep.sh output: per cell and framework, each metric's min/mean/max over the
repetitions, the payload-boundary gate, and a per-metric verdict against both DDS vendors.

Implements OPTIMIZATION_PLAN.md section 9's reading rules, so the verdict is computed rather than
eyeballed:

  WIN   TickLE is better than BOTH vendors and its range does not overlap either of theirs
  TIE   TickLE's mean equals a vendor's - not a draw by overlap, an actual equality
  DRAW  TickLE is better than both but a range overlaps, or it is better than only one
  LOSE  a vendor is better than TickLE with no overlap
  VOID  the cell was void for that framework (instrument, leftover, no RESULT), or the
        payload-boundary gate failed for that payload

"Better" has a direction per metric: higher for throughput, lower for everything else. A draw is
reported as a draw and never rounded into a win - the plan's rule is that inside the spread of three
repetitions is not a result.

Usage: campaign_summary.py /tmp/tickle_campaign_<stamp>.txt
"""
import re
import statistics
import sys
from collections import OrderedDict

# metric -> True when a higher value is better
DIRECTION = {
    "send_mbps": True,
    "recv_mbps": True,
    "rtt_avg_ms": False,
    "cpu_s_per_Msample": False,
    "cpu_s_per_MB": False,
    "peak_rss_kb": False,
    "wire_bytes_per_sample": False,
    "loss_pct": False,
}
# wire_packets_per_sample is deliberately NOT in DIRECTION. It is the boundary GATE, not a metric to
# win: at P1/P2 all three are meant to read 1.0, and calling an intended three-way equality a draw
# (or worse, a win) would be a verdict on the test design rather than on TickLE. The controlled test
# with equal values is what caught that.
GATE_METRIC = "wire_packets_per_sample"
VENDORS = ("cyclonedds", "fastdds")

# What wire_packets_per_sample must read at N0, per OPTIMIZATION_PLAN.md section 9. None = not gated.
BOUNDARY = {
    "p1": {"tickle": 1.0, "cyclonedds": 1.0, "fastdds": 1.0},
    "p2": {"tickle": 1.0, "cyclonedds": 1.0, "fastdds": 1.0},
    "p3": {"tickle": 1.0, "cyclonedds": 2.0, "fastdds": 2.0},
    "p4": {"tickle": None, "cyclonedds": None, "fastdds": None},  # >= 2.0, checked separately
}

LINE = re.compile(
    r"^c(?P<num>\d+)\s+(?P<shape>[TL])\s+(?P<payload>p\d)\s+(?P<qos>Q\d)\s+"
    r"(?P<net>.+?)\s+(?P<fw>tickle|cyclonedds|fastdds)\s+rep(?P<rep>\d+)\s+\|\s+"
    r"(?P<verdict>\S+)\s+\|\s+(?P<fields>.*)$"
)


def parse(path):
    cells = OrderedDict()
    for line in open(path, encoding="utf-8"):
        m = LINE.match(line.rstrip())
        if not m:
            continue
        key = (int(m["num"]), m["shape"], m["payload"], m["qos"], m["net"].strip())
        fw = cells.setdefault(key, OrderedDict()).setdefault(m["fw"], {"void": [], "vals": {}})
        if not m["verdict"].startswith("ok"):
            fw["void"].append(m["verdict"])
            continue
        for k, v in re.findall(r"(\w+)=([-\d.]+)", m["fields"]):
            if k in DIRECTION or k in ("sample_bytes", GATE_METRIC):
                try:
                    fw["vals"].setdefault(k, []).append(float(v))
                except ValueError:
                    pass
    return cells


def rng(vals):
    lo, hi = min(vals), max(vals)
    return lo, hi, statistics.mean(vals)


def fmt(vals):
    lo, hi, mean = rng(vals)
    f = (lambda x: f"{x:.0f}") if abs(hi) >= 1000 else (lambda x: f"{x:.3g}")
    return f(mean) if lo == hi else f"{f(mean)}[{f(lo)}..{f(hi)}]"


def boundary_verdict(payload, net, per_fw):
    """The section-9 gate. Only meaningful at N0, where no tc is shaping anything."""
    if net != "none":
        return None
    bad = []
    for fw, data in per_fw.items():
        got = data["vals"].get(GATE_METRIC)
        if not got:
            continue
        observed = statistics.mean(got)
        want = BOUNDARY.get(payload, {}).get(fw, "skip")
        if payload == "p4":
            if observed < 2.0:
                bad.append(f"{fw} {observed:.2f} < 2.0")
        elif want is not None and abs(observed - want) > 0.05:
            bad.append(f"{fw} {observed:.2f} != {want}")
    return bad or None


def verdict(metric, tickle, vendors):
    """Section 9's rule, with two cases the controlled test showed it needed: an actual equality is
    a TIE rather than a draw-by-overlap, and a verdict from a single repetition per side is
    provisional, because n=1 gives a zero spread that no overlap can ever be detected against."""
    hi_better = DIRECTION[metric]
    t_lo, t_hi, t_mean = rng(tickle)
    provisional = len(tickle) < 2 or any(len(v) < 2 for v in vendors.values())
    suffix = " [provisional: n<2 on some side, so a zero spread is an artefact]" if provisional else ""
    beats, overlaps, ties = [], [], []
    for name, vals in vendors.items():
        v_lo, v_hi, v_mean = rng(vals)
        ties.append(t_mean == v_mean)
        better = t_mean > v_mean if hi_better else t_mean < v_mean
        overlap = not (t_hi < v_lo or v_hi < t_lo)
        beats.append(better)
        overlaps.append(overlap)
        if not better and not overlap and t_mean != v_mean:
            return "LOSE", f"{name} is better with no overlap" + suffix
    if any(ties):
        return "TIE", "equal to " + ", ".join(n for n, t in zip(vendors, ties) if t) + suffix
    if all(beats) and not any(overlaps):
        return "WIN", suffix.strip()
    if all(beats):
        return "DRAW", "better than both, but a range overlaps" + suffix
    return "DRAW", "better than only one" + suffix


def main(path):
    cells = parse(path)
    if not cells:
        sys.exit(f"{path}: no campaign result lines matched - wrong file, or the sweep printed nothing")
    wins = draws = losses = voids = 0
    for key, per_fw in cells.items():
        num, shape, payload, qos, net = key
        print(f"\n== c{num} {shape} {payload} {qos} [{net}]")
        gate = boundary_verdict(payload, net, per_fw)
        if gate:
            print(f"   BOUNDARY GATE FAILED: {'; '.join(gate)}")
            print("   -> this payload's cross-vendor comparison is VOID (section 9). Numbers below are")
            print("      printed anyway: the observed packet count is how the right size is computed.")
        for fw, data in per_fw.items():
            if data["void"]:
                print(f"   {fw:<11} VOID x{len(data['void'])}: {data['void'][0]}")
        metrics = [m for m in DIRECTION if m in per_fw.get("tickle", {}).get("vals", {})]
        for m in metrics:
            t = per_fw["tickle"]["vals"][m]
            vend = {v: per_fw[v]["vals"][m] for v in VENDORS
                    if v in per_fw and m in per_fw[v]["vals"]}
            if len(vend) < 2:
                print(f"   {m:<24} tickle {fmt(t):<22} (no cross-vendor comparison: "
                      f"{2 - len(vend)} vendor(s) missing)")
                continue
            v, why = verdict(m, t, vend)
            if gate:
                v, why = "VOID", "boundary gate"
            cols = "  ".join(f"{name[:6]} {fmt(vals)}" for name, vals in vend.items())
            print(f"   {m:<24} tickle {fmt(t):<22} {cols:<44} {v}{'  (' + why + ')' if why else ''}")
            wins += v == "WIN"; draws += v in ("DRAW", "TIE"); losses += v == "LOSE"; voids += v == "VOID"
    print(f"\n== totals over comparable metric-cells: WIN {wins}  DRAW/TIE {draws}  LOSE {losses}  VOID {voids}")
    print("Every LOSE and every DRAW is an optimisation target and needs a named hypothesis before")
    print("any code change (OPTIMIZATION_PLAN.md section 9).")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/tickle_campaign_latest.txt")

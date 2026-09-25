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

# Metrics are keyed "<role>.<name>" because BenchStats.h emits the same names on BOTH the client and
# the server RESULT line. Collecting them without the role prefix put two values per repetition into
# one list, so the "spread" became the client-server difference rather than the repetition spread -
# and every verdict computed from it would have been wrong in a way no real data would reveal. Found
# by a fabricated cell carrying both roles.
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
# utime_s and stime_s are deliberately NOT verdict metrics, and this is a rule CHANGED AFTER SEEING
# DATA, which is worth stating plainly rather than burying. They were in DIRECTION when the campaign
# started; the first four cells showed why they cannot be.
#
# Every throughput cell runs for a FIXED DURATION (-d 5), not a fixed sample count. A framework that
# pushes more samples in those 5 seconds necessarily burns more CPU seconds, so absolute CPU time
# measures how much work was done at least as much as how efficiently. At c1 it inverts the ranking
# outright:
#
#     fw            samples  stime_s  us/sample
#     tickle         869468    3.943       4.53
#     cyclonedds     741843    3.529       4.76
#     fastdds        286155    3.634      12.70
#
#   by absolute stime_s (lower better): cyclonedds, fastdds, tickle
#   by stime per sample (lower better): tickle, cyclonedds, fastdds
#
# FastDDS "beats" TickLE on absolute stime while doing a third of the work in the same 5 seconds.
# The metric rewards being slow, so it answers a different question from the one section 2 asks.
#
# The reason to trust the change despite its timing: the argument does not depend on which way it
# fell. Had TickLE been the slow one, the same metric would have handed TickLE a free WIN by the
# same mechanism, and it would have been just as wrong. It happens to remove a TickLE LOSE, which is
# exactly the direction that should invite suspicion - so the numbers above are printed here, and
# both values are still reported per cell, marked not-comparable rather than dropped.
#
# cpu_s_per_Msample and cpu_s_per_MB are the normalised forms and stay verdict metrics; they are
# what section 2's "less CPU than both DDS vendors" has to mean in a fixed-duration test.
INFORMATIONAL = ("utime_s", "stime_s")
# wire_packets_per_sample is deliberately NOT in DIRECTION. It is the boundary GATE, not a metric to
# win: at P1/P2 all three are meant to read 1.0, and calling an intended three-way equality a draw
# (or worse, a win) would be a verdict on the test design rather than on TickLE. The controlled test
# with equal values is what caught that.
# The gate reads the CLIENT's own transmitted packets per sample. wire_packets_per_sample counts the
# whole interface in both directions, so on a RELIABLE publisher it carries the returning ACKNACKs
# and can never read 1.0 (TickLE Dev's correction - I had named the wrong field for this job). A
# publisher also sends heartbeats, so a correct single datagram reads slightly above 1.0: the gate is
# a band, not an equality.
GATE_METRIC = "wire_role_packets_per_sample"
GATE_ROLE = "client"
ONE_DATAGRAM_MAX = 1.5   # below this is one datagram per sample
TWO_DATAGRAM_MIN = 2.0   # at or above this is two
VENDORS = ("cyclonedds", "fastdds")

# What wire_packets_per_sample must read at N0, per OPTIMIZATION_PLAN.md section 9. None = not gated.
# "one" = below ONE_DATAGRAM_MAX, "two" = at or above TWO_DATAGRAM_MIN.
BOUNDARY = {
    "p1": {"tickle": "one", "cyclonedds": "one", "fastdds": "one"},
    "p2": {"tickle": "one", "cyclonedds": "one", "fastdds": "one"},
    "p3": {"tickle": "one", "cyclonedds": "two", "fastdds": "two"},
    "p4": {"tickle": "two", "cyclonedds": "two", "fastdds": "two"},
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
            # fail:samples is a fact about the cell (nothing was delivered, so every per-sample
            # figure is 0 by construction), not about the instrument. Both void the cell, and the
            # distinction is kept so the write-up can say which (TickLE Dev).
            fw["void"].append(m["verdict"])
            continue
        # Split the concatenated RESULT lines back into their roles before reading any number.
        for chunk in re.split(r"(?=role=)", m["fields"]):
            role_m = re.match(r"role=(\w+)", chunk)
            role = role_m.group(1) if role_m else "client"
            for k, v in re.findall(r"([A-Za-z_]\w*)=([-\d.]+)", chunk):
                if k in DIRECTION or k in INFORMATIONAL or k in ("sample_bytes", GATE_METRIC):
                    try:
                        fw["vals"].setdefault(f"{role}.{k}", []).append(float(v))
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
        got = data["vals"].get(f"{GATE_ROLE}.{GATE_METRIC}")
        if not got:
            bad.append(f"{fw} has no {GATE_ROLE}.{GATE_METRIC}")
            continue
        observed = statistics.mean(got)
        want = BOUNDARY.get(payload, {}).get(fw)
        if want == "one" and observed >= ONE_DATAGRAM_MAX:
            bad.append(f"{fw} {observed:.2f} >= {ONE_DATAGRAM_MAX}, wanted one datagram")
        elif want == "two" and observed < TWO_DATAGRAM_MIN:
            bad.append(f"{fw} {observed:.2f} < {TWO_DATAGRAM_MIN}, wanted two")
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
        tvals = per_fw.get("tickle", {}).get("vals", {})
        metrics = [m for m in sorted(tvals) if m.split(".", 1)[1] in DIRECTION]
        for m in [m for m in sorted(tvals) if m.split(".", 1)[1] in INFORMATIONAL]:
            vend = {v: per_fw[v]["vals"][m] for v in VENDORS
                    if v in per_fw and m in per_fw[v]["vals"]}
            cols = "  ".join(f"{name[:6]} {fmt(vals)}" for name, vals in vend.items())
            print(f"   {m:<24} tickle {fmt(tvals[m]):<22} {cols:<44} not-comparable"
                  f"  (absolute CPU over a fixed duration; see cpu_s_per_Msample)")
        for m in metrics:
            t = per_fw["tickle"]["vals"][m]
            vend = {v: per_fw[v]["vals"][m] for v in VENDORS
                    if v in per_fw and m in per_fw[v]["vals"]}
            if len(vend) < 2:
                print(f"   {m:<24} tickle {fmt(t):<22} (no cross-vendor comparison: "
                      f"{2 - len(vend)} vendor(s) missing)")
                continue
            v, why = verdict(m.split(".", 1)[1], t, vend)
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

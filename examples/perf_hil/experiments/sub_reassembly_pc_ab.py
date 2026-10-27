#!/usr/bin/env python3
"""Reads sub_reassembly_pc_ab.sh's runs.txt by the rules registered before its first run (2026-10-27).

The question (docs/DESIGN.md section 8, "Reassembly slots"; the user, 2026-10-27): with every Subscriber that
reassembles owning a slot of its own and the node's pool only overflow, does a BEST_EFFORT Subscriber beside 4-8
RELIABLE fragmented streams whose Subscribers have no reorder buffer get more of its samples - and does nothing the
RELIABLE streams get, or the CPU, get worse?

Arms: M = origin/main, A = M's binaries run again (A/A: the session's noise, THE FLOOR), F = the change.

Per run (one arm, one cell, one repetition), from the RESULT lines:
  be_pct   BEST_EFFORT samples delivered / published, %        rel_pct  RELIABLE (all N streams) delivered / published, %
  rel_p50, rel_p99   RELIABLE publish-to-decode latency, us   cpu      subscriber CPU (user + system) per delivered
                                                                       sample, us (every class's samples)
  ctl_pct, ctl_p50   the control: 64-byte RELIABLE samples, whole DATAs read through a reorder buffer - no path the
                     change touches
  torn               samples delivered with bytes not the publisher's, every class
A run is VOID when either RESULT line is missing, nothing was published, or its treatment is not its arm's: M and A
must print own_slot_bytes=0 (no per-Subscriber slot), F own_slot_bytes > 0. A cell is VOID with fewer than 3 valid
runs of any arm.

Per (cell, metric): d = mean over repetitions of (arm - M) for the percentages (points), of (arm / M - 1) for
latency and CPU (relative). FLOOR = max(fixed, |d(A)| + 2 SE(A)); fixed = 0.5 points, 5% for latency, 3% for CPU.
BETTER / WORSE: d(F) beyond the floor and beyond 2 SE of F's own d, in that direction; otherwise HELD.

Rules (each printed with its verdict):
  R1  BEST_EFFORT delivery BETTER in at least one lossy cell (1% or 5%) and WORSE in none. VOID when M dropped no
      fragment for want of a slot (frag_dropped 0) in every lossy cell: the shape never filled the parent's pool,
      so this run cannot decide.
  R2  RELIABLE delivery NOT WORSE in any cell.
  R3  RELIABLE p99 and p50 latency NOT WORSE in any cell.
  R4  subscriber CPU per delivered sample NOT WORSE in any cell.
  C   the control's delivery and p50 HELD in every cell (F against M). If not, the PC moved under the arms and
      R1-R4 are VOID, whatever they say.
  T   no torn sample in any run of any arm.
What would falsify the change: R1 FAIL (the slots do not reach the network shape), or a WORSE in R2-R4 (a cost).
Overall: PASS when T, C, R1-R4 all pass; VOID when C or every cell is VOID; FAIL otherwise.

Usage: sub_reassembly_pc_ab.py <runs.txt>
"""
import math
import re
import statistics
import sys

ARMS = ("M", "A", "F")
FIXED = {"be_pct": 0.5, "rel_pct": 0.5, "ctl_pct": 0.5, "rel_p50": 0.05, "rel_p99": 0.05, "ctl_p50": 0.05,
         "cpu": 0.03}
RELATIVE = {"rel_p50", "rel_p99", "ctl_p50", "cpu"}
HIGHER_IS_BETTER = {"be_pct", "rel_pct", "ctl_pct"}


def fields(text):
    return dict(re.findall(r"(\w+)=([^\s|]+)", text))


def parse(path):
    runs = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line.startswith("cell="):
            continue
        head, _, rest = line.partition(" | ")
        meta = fields(head)
        if "VOID" in head:
            runs.append({"cell": meta["cell"], "arm": meta["arm"], "rep": int(meta["rep"]), "void": head})
            continue
        pub_text, _, sub_text = rest.partition(" | ")
        pub, sub = fields(pub_text), fields(sub_text)
        run = {"cell": meta["cell"], "arm": meta["arm"], "rep": int(meta["rep"]), "void": None}
        try:
            own = int(sub["own_slot_bytes"])
            rel_pub, be_pub, ctl_pub = int(pub["rel_published"]), int(pub["be_published"]), int(pub["ctl_published"])
            if rel_pub == 0 or be_pub == 0 or ctl_pub == 0:
                run["void"] = "nothing published"
            elif (meta["arm"] == "F") != (own > 0):
                run["void"] = f"treatment: arm {meta['arm']} own_slot_bytes={own}"
            else:
                delivered = int(sub["rel_delivered"]) + int(sub["be_delivered"]) + int(sub["ctl_delivered"])
                run.update({
                    "be_pct": 100.0 * int(sub["be_delivered"]) / be_pub,
                    "rel_pct": 100.0 * int(sub["rel_delivered"]) / rel_pub,
                    "ctl_pct": 100.0 * int(sub["ctl_delivered"]) / ctl_pub,
                    "rel_p50": float(sub["rel_p50_us"]),
                    "rel_p99": float(sub["rel_p99_us"]),
                    "ctl_p50": float(sub["ctl_p50_us"]),
                    "cpu": 1000.0 * float(sub["cpu_ms"]) / delivered if delivered else math.nan,
                    "torn": int(sub["rel_torn"]) + int(sub["be_torn"]) + int(sub["ctl_torn"]),
                    "frag_dropped": int(sub["frag_dropped"]),
                })
        except (KeyError, ValueError) as error:
            run["void"] = f"unparsable ({error})"
        runs.append(run)
    return runs


def mean_se(values):
    if not values:
        return math.nan, math.nan
    if len(values) == 1:
        return values[0], math.inf
    return statistics.fmean(values), statistics.stdev(values) / math.sqrt(len(values))


def deltas(runs, cell, arm, metric):
    """Per repetition, arm against M of the same repetition."""
    by_rep = {}
    for run in runs:
        if run["cell"] == cell and run["void"] is None:
            by_rep.setdefault(run["rep"], {})[run["arm"]] = run[metric]
    out = []
    for arms in by_rep.values():
        if arm in arms and "M" in arms and not math.isnan(arms[arm]) and not math.isnan(arms["M"]):
            if metric in RELATIVE:
                if arms["M"] > 0:
                    out.append(arms[arm] / arms["M"] - 1.0)
            else:
                out.append(arms[arm] - arms["M"])
    return out


def verdict(runs, cell, metric):
    d_a, se_a = mean_se(deltas(runs, cell, "A", metric))
    d_f, se_f = mean_se(deltas(runs, cell, "F", metric))
    if math.isnan(d_f) or math.isnan(d_a):
        return "VOID", d_f, math.nan
    floor = max(FIXED[metric], abs(d_a) + 2.0 * se_a)
    better = d_f > 0 if metric in HIGHER_IS_BETTER else d_f < 0
    if abs(d_f) > floor and abs(d_f) > 2.0 * se_f:
        return ("BETTER" if better else "WORSE"), d_f, floor
    return "HELD", d_f, floor


def main():
    runs = parse(sys.argv[1])
    cells = sorted({run["cell"] for run in runs}, key=lambda c: (int(c.split("_")[0][1:]), float(c.split("_")[1][1:])))
    void_cells = set()
    print("cell          arm  valid  be_pct  rel_pct  rel_p50  rel_p99  cpu_us  ctl_pct  ctl_p50  frag_dropped  torn")
    for cell in cells:
        for arm in ARMS:
            valid = [r for r in runs if r["cell"] == cell and r["arm"] == arm and r["void"] is None]
            if len(valid) < 3:
                void_cells.add(cell)
            if not valid:
                print(f"{cell:13s} {arm:4s} {0:5d}  (no valid run)")
                continue

            def avg(key, valid=valid):
                return statistics.fmean(r[key] for r in valid)

            print(f"{cell:13s} {arm:4s} {len(valid):5d}  {avg('be_pct'):6.2f}  {avg('rel_pct'):7.2f}  "
                  f"{avg('rel_p50'):7.0f}  {avg('rel_p99'):7.0f}  {avg('cpu'):6.1f}  {avg('ctl_pct'):7.2f}  "
                  f"{avg('ctl_p50'):7.0f}  {avg('frag_dropped'):12.1f}  {sum(r['torn'] for r in valid)}")
    for run in runs:
        if run["void"] is not None:
            print(f"VOID run: {run['cell']} arm {run['arm']} rep {run['rep']}: {run['void']}")
    live = [c for c in cells if c not in void_cells]
    for cell in sorted(void_cells):
        print(f"VOID cell {cell}: fewer than 3 valid runs of some arm")

    table = {}
    print("\ncell          metric   d(F)        floor     verdict")
    for cell in live:
        for metric in ("be_pct", "rel_pct", "rel_p50", "rel_p99", "cpu", "ctl_pct", "ctl_p50"):
            v, d_f, floor = verdict(runs, cell, metric)
            table[(cell, metric)] = v
            unit = "" if metric not in RELATIVE else " (rel)"
            print(f"{cell:13s} {metric:8s} {d_f:+10.4f} {floor:9.4f}  {v}{unit}")

    def loss_of(cell):
        return float(cell.split("_")[1][1:])

    results = {}
    torn = sum(r.get("torn", 0) for r in runs if r["void"] is None)
    results["T"] = "PASS" if torn == 0 else f"FAIL ({torn} torn)"
    control = [table[(c, m)] for c in live for m in ("ctl_pct", "ctl_p50")]
    results["C"] = "HELD" if control and all(v == "HELD" for v in control) else ("VOID" if not control else "MOVED")
    lossy = [c for c in live if loss_of(c) > 0]
    m_dropped = {c: statistics.fmean(r["frag_dropped"] for r in runs
                                     if r["cell"] == c and r["arm"] == "M" and r["void"] is None) for c in lossy}
    if not lossy:
        results["R1"] = "VOID (no lossy cell)"
    elif all(v == 0 for v in m_dropped.values()):
        results["R1"] = "VOID (main dropped no fragment for want of a slot in any lossy cell)"
    else:
        be = [table[(c, "be_pct")] for c in lossy]
        results["R1"] = "PASS" if "BETTER" in be and "WORSE" not in be else f"FAIL ({', '.join(be)})"
    for rule, metrics in (("R2", ("rel_pct",)), ("R3", ("rel_p50", "rel_p99")), ("R4", ("cpu",))):
        worse = [f"{c} {m}" for c in live for m in metrics if table[(c, m)] == "WORSE"]
        results[rule] = "PASS" if live and not worse else ("VOID" if not live else f"FAIL ({'; '.join(worse)})")
    print()
    for rule in ("T", "C", "R1", "R2", "R3", "R4"):
        print(f"{rule}: {results[rule]}")
    if not live or results["C"] in ("MOVED", "VOID"):
        overall = "VOID"
    elif all(results[r] in ("PASS", "HELD") for r in ("T", "C", "R1", "R2", "R3", "R4")):
        overall = "PASS"
    else:
        overall = "FAIL"
    print(f"OVERALL: {overall}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

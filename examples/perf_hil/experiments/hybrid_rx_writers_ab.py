#!/usr/bin/env python3
"""Reads hybrid_rx_writers_ab.sh's runs.txt by the rules registered before its first run (2026-10-27).

The question (DESIGN.md section 8, "What the reader measures"): with the hybrid reader keeping 16 writers (the least
recently active replaced) and counting a whole body lost of unknown layout as lost runs, does the reader's collapse
test see a BEST_EFFORT stream's runs dying in the kernel's reassembly memory beside 8 or 12 RELIABLE fragmented
streams - and does that stream get more of its samples, with nothing else worse?

Arms: M = origin/main, A = M's binaries run again (A/A: the session's noise, THE FLOOR), F = the change.

Per run, from the RESULT lines and the reader's hybrid line:
  be_pct, rel_pct, ctl_pct   delivered / published, %          rel_p50, rel_p99, ctl_p50   latency, us
  cpu                        subscriber CPU per delivered sample, us
  collapses                  the reader's collapse decisions (the witness: M's are 0 where its table thrashes)
  reasm_oks                  IP datagrams the subscriber namespace reassembled (the runs that got through)
  torn                       samples delivered with bytes not the publisher's
A run is VOID when a RESULT line is missing, nothing was published, or its treatment is not its arm's: the change's
reader prints rx_writer_evictions on its hybrid line, the parent's has no such field. A cell is VOID with fewer than 3
valid runs of any arm.

Per (cell, metric): d = mean over repetitions of (arm - M), points for percentages, relative for latency and CPU.
FLOOR = max(fixed, |d(A)| + 2 SE(A)); fixed = 0.5 points, 5% latency, 3% CPU. BETTER / WORSE: d(F) beyond the floor
and beyond 2 SE of F's own d; otherwise HELD - or UNDECIDED when the floor is wider than the metric's DECIDES bound
(delivery 2 points, latency 25%, CPU 10%; as sub_reassembly_pc_ab.py, 104bf641).

Rules:
  W   the witness: in at least one cell of more than 4 streams M's reader decided no collapse (median collapses 0) -
      the blindness the change is for. Otherwise R1 is VOID: this run did not show the defect.
  R1  BEST_EFFORT delivery BETTER in at least one cell where W holds, and WORSE in no cell. UNDECIDED when none is
      BETTER, none WORSE, and every such cell's floor is wider than its bound.
  R2  RELIABLE delivery NOT WORSE in any cell.
  R3  RELIABLE p50 and p99 NOT WORSE in any cell.
  R4  CPU per delivered sample NOT WORSE in any cell.
  C   the controls: the 4-stream cells (5 writers, within the parent's 8 entries) - BEST_EFFORT and RELIABLE delivery
      HELD - and every cell's 64-byte RELIABLE control - delivery and p50 HELD. MOVED makes R1-R4 VOID: either the PC
      moved under the arms or the change does more than it is for.
  T   no torn sample in any run.
What would falsify the change: R1 FAIL where W holds (counting the losses does not reach the BEST_EFFORT stream), F's
reader still deciding no collapse there, or a WORSE in R2-R4.
Overall: PASS when T, C, W, R1-R4 all pass; VOID when C is MOVED, W fails or every cell is VOID; INCONCLUSIVE when
nothing failed and something is UNDECIDED; FAIL otherwise.

REPS 8 of 40 s each (hybrid_rx_writers_ab.sh's defaults) is what was run for it.

Usage: hybrid_rx_writers_ab.py <runs.txt>
"""
import math
import re
import statistics
import sys

ARMS = ("M", "A", "F")
FIXED = {"be_pct": 0.5, "rel_pct": 0.5, "ctl_pct": 0.5, "rel_p50": 0.05, "rel_p99": 0.05, "ctl_p50": 0.05,
         "cpu": 0.03}
DECIDES = {"be_pct": 2.0, "rel_pct": 2.0, "ctl_pct": 2.0, "rel_p50": 0.25, "rel_p99": 0.25, "ctl_p50": 0.25,
           "cpu": 0.10}
RELATIVE = {"rel_p50", "rel_p99", "ctl_p50", "cpu"}
HIGHER_IS_BETTER = {"be_pct", "rel_pct", "ctl_pct"}
METRICS = ("be_pct", "rel_pct", "rel_p50", "rel_p99", "cpu", "ctl_pct", "ctl_p50")


def fields(text):
    return dict(re.findall(r"(\w+)=([^\s|]+)", text))


def streams_of(cell):
    return int(cell.split("_")[0][1:])


def parse(path):
    runs = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line.startswith("cell="):
            continue
        parts = line.split(" | ")
        meta = fields(parts[0])
        run = {"cell": meta["cell"], "arm": meta["arm"], "rep": int(meta["rep"]), "void": None}
        if "VOID" in parts[0] or len(parts) < 4:
            run["void"] = parts[0] if "VOID" in parts[0] else "incomplete line"
            runs.append(run)
            continue
        pub, sub, reader = fields(parts[1]), fields(parts[2]), fields(parts[3])
        try:
            rel_pub, be_pub, ctl_pub = int(pub["rel_published"]), int(pub["be_published"]), int(pub["ctl_published"])
            if rel_pub == 0 or be_pub == 0 or ctl_pub == 0:
                run["void"] = "nothing published"
            elif (meta["arm"] == "F") != ("rx_writer_evictions" in reader):
                run["void"] = f"treatment: arm {meta['arm']} rx_writer_evictions {'present' if 'rx_writer_evictions' in reader else 'absent'}"
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
                    "collapses": int(reader.get("collapses", "0")),
                    "evictions": int(reader.get("rx_writer_evictions", "0")),
                    "reasm_oks": int(reader.get("ReasmOKs", "0")),
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
    if not floor <= DECIDES[metric]:
        return "UNDECIDED", d_f, floor
    return "HELD", d_f, floor


def valid(runs, cell, arm):
    return [r for r in runs if r["cell"] == cell and r["arm"] == arm and r["void"] is None]


def main():
    runs = parse(sys.argv[1])
    cells = sorted({r["cell"] for r in runs}, key=lambda c: (streams_of(c), float(c.split("_")[1][1:])))
    void_cells = set()
    print("cell          arm  valid  be_pct  rel_pct  rel_p50  rel_p99  cpu_us  ctl_pct  collapses(med)  reasm_oks  "
          "evictions  torn")
    for cell in cells:
        for arm in ARMS:
            got = valid(runs, cell, arm)
            if len(got) < 3:
                void_cells.add(cell)
            if not got:
                print(f"{cell:13s} {arm:4s} {0:5d}  (no valid run)")
                continue

            def avg(key, got=got):
                return statistics.fmean(r[key] for r in got)

            print(f"{cell:13s} {arm:4s} {len(got):5d}  {avg('be_pct'):6.2f}  {avg('rel_pct'):7.2f}  "
                  f"{avg('rel_p50'):7.0f}  {avg('rel_p99'):7.0f}  {avg('cpu'):6.1f}  {avg('ctl_pct'):7.2f}  "
                  f"{statistics.median(r['collapses'] for r in got):14.1f}  {avg('reasm_oks'):9.1f}  "
                  f"{avg('evictions'):9.1f}  {sum(r['torn'] for r in got)}")
    for run in runs:
        if run["void"] is not None:
            print(f"VOID run: {run['cell']} arm {run['arm']} rep {run['rep']}: {run['void']}")
    live = [c for c in cells if c not in void_cells]
    for cell in sorted(void_cells):
        print(f"VOID cell {cell}: fewer than 3 valid runs of some arm")

    table = {}
    print("\ncell          metric   d(F)        floor     n(F)  verdict")
    for cell in live:
        for metric in METRICS:
            v, d_f, floor = verdict(runs, cell, metric)
            table[(cell, metric)] = v
            n_f = len(deltas(runs, cell, "F", metric))
            print(f"{cell:13s} {metric:8s} {d_f:+10.4f} {floor:9.4f} {n_f:5d}  {v}{' (rel)' if metric in RELATIVE else ''}")

    results = {}
    torn = sum(r.get("torn", 0) for r in runs if r["void"] is None)
    results["T"] = "PASS" if torn == 0 else f"FAIL ({torn} torn)"
    control = [table[(c, m)] for c in live for m in ("ctl_pct", "ctl_p50")]
    control += [table[(c, m)] for c in live if streams_of(c) <= 4 for m in ("be_pct", "rel_pct")]
    if not control:
        results["C"] = "VOID"
    elif any(v in ("BETTER", "WORSE") for v in control):
        results["C"] = "MOVED"
    else:
        results["C"] = "HELD" if all(v == "HELD" for v in control) else "UNDECIDED"
    blind = [c for c in live if streams_of(c) > 4 and statistics.median(r["collapses"] for r in valid(runs, c, "M")) == 0]
    results["W"] = f"PASS ({', '.join(blind)})" if blind else "FAIL (M's reader decided a collapse in every cell)"
    if not blind:
        results["R1"] = "VOID (W)"
    else:
        be_blind = [table[(c, "be_pct")] for c in blind]
        be_all = [table[(c, "be_pct")] for c in live]
        if "WORSE" in be_all:
            results["R1"] = f"FAIL (WORSE in {', '.join(c for c in live if table[(c, 'be_pct')] == 'WORSE')})"
        elif "BETTER" in be_blind:
            results["R1"] = "PASS"
        elif all(v == "UNDECIDED" for v in be_blind):
            results["R1"] = "UNDECIDED (every blind cell's floor is wider than 2 points)"
        else:
            results["R1"] = f"FAIL ({', '.join(be_blind)})"
    for rule, metrics in (("R2", ("rel_pct",)), ("R3", ("rel_p50", "rel_p99")), ("R4", ("cpu",))):
        worse = [f"{c} {m}" for c in live for m in metrics if table[(c, m)] == "WORSE"]
        undecided = [f"{c} {m}" for c in live for m in metrics if table[(c, m)] == "UNDECIDED"]
        if not live:
            results[rule] = "VOID"
        elif worse:
            results[rule] = f"FAIL ({'; '.join(worse)})"
        elif undecided:
            results[rule] = f"UNDECIDED ({'; '.join(undecided)})"
        else:
            results[rule] = "PASS"
    print()
    order = ("T", "C", "W", "R1", "R2", "R3", "R4")
    for rule in order:
        print(f"{rule}: {results[rule]}")
    heads = {r: results[r].split()[0] for r in order}
    if not live or heads["C"] in ("MOVED", "VOID") or heads["W"] == "FAIL":
        overall = "VOID"
    elif all(heads[r] in ("PASS", "HELD") for r in order):
        overall = "PASS"
    elif all(heads[r] in ("PASS", "HELD", "UNDECIDED") for r in order):
        overall = "INCONCLUSIVE"
    else:
        overall = "FAIL"
    print(f"OVERALL: {overall}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

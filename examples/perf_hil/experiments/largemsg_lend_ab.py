#!/usr/bin/env python3
"""largemsg_lend_ab.py - the I4 A/B of lending large samples on the subscription (1e8ec600), read by rules written
before any rig run (2026-10-11).

The change: a subscription lends a type above a datagram where core assembled it (docs/RMW.md "Loaned messages"), so
a 4 MB Array4m is copied once (an ordinary take) or not at all (a loaned take) after its last fragment, where the
parent decoded it into a shell and copied it again. The hypothesis, from largemsg_wire_budget.py on A4 3b/3c:
rmw_tickle's 1.19 ms behind Fast DDS is 0.52 ms of wire and 0.67-0.70 ms off it, and the latter is that extra copy
(one cold 4 MiB memcpy: 0.57-0.82 ms on the PC).

Input: one rmw_keepall_rig.sh invocation (its OUT.txt, written by largemsg_rig_summary.py, and OUT.runs/), arms
tickle@head (the change), tickle@pre (its parent), fastdds, cyclonedds; one cell (Array4m, 15 Hz, cross-host).

RULES (enforced below):
  TREATMENT  a tickle@head run is VOID unless its subscriber's core traffic line shows lend_retains >= 95% of the
             samples it received (core kept each large sample for the take); tickle@pre must show lend_retains=0 in
             every run, or the whole reading is VOID (pre is not the parent). Which take ran is read, not assumed: a
             head run whose subscriber printed loans_in_place >= 95% of received took loans (rclcpp's loaned take: no
             copy after the last fragment), else ordinary takes (one copy). A run with no traffic line is VOID ("could
             not look" is not "off").
  USABLE     a run whose latency columns are '-' (the summary's clock rule) counts for delivery and CPU only. Fewer
             than 3 usable runs in either A/B arm: NO VERDICT.
  DELIVERY   every tickle@head run delivered 100% with lost 0, or the cell is LOSE whatever the latency says.
  A/B        (head vs pre) latency median, p99 lower bound, subscriber CPU ms/MB, subscriber peak RSS: d = mean(head) -
             mean(pre) over runs, SE Welch. BETTER iff d < -2 SE and |d| > the vendors' drift (the larger of
             fastdds's and cyclonedds's max-min of the same metric over their runs); WORSE symmetric; else HELD.
             A WORSE on any of the four blocks landing.
  HYPOTHESIS the latency median must come out BETTER with d <= -0.30 ms (a third of a copy below the smallest copy
             the PC measured); expected d, by the take that ran: ordinary -0.5..-0.85 ms, loaned -1.0..-1.7 ms.
             Otherwise FALSIFIED: the extra copy is not where the 0.67 ms goes, and the next candidate (the publish's
             encode before the first fragment) is read with this one's numbers.
  HEADLINE   head vs fastdds, latency median and p99 lower bound, by the range rule (WIN: every head run below every
             fastdds run; LOSE: every head run above; else DRAW). Predicted: loaned takes about -0.2 ms (WIN or DRAW),
             ordinary takes about +0.5 ms (LOSE: the wire's 0.52 ms, which this change does not touch). Printed with
             the prediction; it decides nothing about landing.
Exit 0 when the reading is complete (whatever it says), 2 when VOID or NO VERDICT, 1 on a missing input.
Usage: largemsg_lend_ab.py [--arm ARM] OUT  (OUT.txt and OUT.runs/ must exist; ARM, default tickle@head, is the arm
       read against tickle@pre - tickle@loans for rclcpp's loaned takes)
"""
import math
import re
import statistics
import sys
from pathlib import Path

HEAD, PRE, VENDORS = "tickle@head", "tickle@pre", ("fastdds", "cyclonedds")
RUN_RE = re.compile(r"^(\S+)\s+l(\d+)\s+(\S+)\s+r(\d+)\s+sent\s+(\d+)\s+recv\s+(\d+)\s+lost\s+(\d+)\s+end\s+-?\d+\s+"
                    r"\(\s*([\d.]+)%\).*?lat med\s+(\S+)\s+p99>=\s+(\S+).*?cpu sub\s+(\S+)\s+pub\s+(\S+)\s+"
                    r"rss\s+(\d+)/\s*(\d+)")
METRICS = (("latency median ms", "lat"), ("latency p99 (lower bound) ms", "p99"), ("sub CPU ms/MB", "cpu_sub"),
           ("sub peak RSS kB", "rss_sub"))


def num(s):
    try:
        return float(s)
    except ValueError:
        return None


def read_runs(txt):
    runs = []
    for line in txt.read_text(errors="replace").splitlines():
        m = RUN_RE.match(line.strip())
        if not m:
            continue
        runs.append({"topic": m[1], "arm": m[3], "rep": int(m[4]), "sent": int(m[5]), "recv": int(m[6]),
                     "lost": int(m[7]), "pct": float(m[8]), "lat": num(m[9]), "p99": num(m[10]),
                     "cpu_sub": num(m[11]), "rss_sub": float(m[13])})
    return runs


def treatment(runs_dir, run):
    """(lend_retains, loans_in_place) from the run's subscriber log; None for a value it did not print."""
    log = runs_dir / f"{run['arm']}_{run['topic']}_l0_r{run['rep']}_sub.log"
    if not log.exists():
        return None, None
    text = log.read_text(errors="replace")
    retains = [int(x) for x in re.findall(r"\blend_retains=(\d+)", text)]
    loans = [int(x) for x in re.findall(r"loans_in_place=(\d+)", text)]
    return (max(retains) if retains else None), (max(loans) if loans else None)


def welch(a, b):
    d = statistics.mean(a) - statistics.mean(b)
    se = math.sqrt(statistics.variance(a) / len(a) + statistics.variance(b) / len(b))
    return d, se


def verdict(d, se, drift):
    if d < -2 * se and abs(d) > drift:
        return "BETTER"
    if d > 2 * se and d > drift:
        return "WORSE"
    return "HELD"


def range_rule(head, other):
    if max(head) < min(other):
        return "WIN"
    if min(head) > max(other):
        return "LOSE"
    return "DRAW"


def main():
    global HEAD
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "--arm":
        HEAD = args[1]
        args = args[2:]
    if len(args) != 1:
        print(__doc__)
        return 1
    out = Path(args[0])
    txt, runs_dir = Path(f"{out}.txt"), Path(f"{out}.runs")
    if not txt.exists() or not runs_dir.is_dir():
        print(f"MISSING: {txt} or {runs_dir}")
        return 1
    runs = read_runs(txt)
    by_arm = {}
    for r in runs:
        by_arm.setdefault(r["arm"], []).append(r)
    print(f"=== lend A/B: {txt}: {HEAD} against {PRE} ===")
    print(f"runs: " + ", ".join(f"{a} {len(v)}" for a, v in sorted(by_arm.items())))
    # TREATMENT
    void_all = False
    loaned = ordinary = 0
    for r in by_arm.get(PRE, []):
        retains, _ = treatment(runs_dir, r)
        if retains is None or retains != 0:
            print(f"VOID overall: {PRE} r{r['rep']} lend_retains={retains} (the parent retains no large sample)")
            void_all = True
    head_ok = []
    for r in by_arm.get(HEAD, []):
        retains, loans = treatment(runs_dir, r)
        took = "loaned" if loans is not None and loans >= 0.95 * r["recv"] else "ordinary"
        ok = retains is not None and retains >= 0.95 * r["recv"]
        print(f"  {HEAD} r{r['rep']}: recv {r['recv']} lend_retains={retains} loans_in_place={loans} -> "
              f"{'treated, ' + took + ' takes' if ok else 'VOID (treatment not seen)'}")
        if ok:
            head_ok.append(r)
            loaned += took == "loaned"
            ordinary += took == "ordinary"
    if void_all:
        return 2
    path = "loaned" if loaned > ordinary else "ordinary"
    expected = (-1.7, -1.0) if path == "loaned" else (-0.85, -0.5)
    print(f"takes: {loaned} runs loaned, {ordinary} ordinary -> expected latency median d {expected[0]}..{expected[1]}"
          f" ms ({path})")
    # DELIVERY: no runs read is "could not look", never "complete".
    if not by_arm.get(HEAD) or not by_arm.get(PRE):
        print(f"VOID: no {HEAD} or no {PRE} run line read from {txt}")
        return 2
    incomplete = [r for r in by_arm[HEAD] if r["lost"] != 0 or r["pct"] < 100.0]
    print(f"DELIVERY {HEAD}: {len(by_arm[HEAD])} runs, "
          f"{'complete in every one' if not incomplete else 'INCOMPLETE -> cell LOSE'}")
    # A/B
    rc = 0
    blocked = []
    hyp = None
    for label, key in METRICS:
        h = [r[key] for r in head_ok if r[key] is not None]
        p = [r[key] for r in by_arm.get(PRE, []) if r[key] is not None]
        drift = 0.0
        for v in VENDORS:
            vals = [r[key] for r in by_arm.get(v, []) if r[key] is not None]
            if len(vals) >= 2:
                drift = max(drift, max(vals) - min(vals))
        if len(h) < 3 or len(p) < 3:
            print(f"  {label:30} NO VERDICT: usable runs head {len(h)}, pre {len(p)} (3 needed)")
            rc = 2
            continue
        d, se = welch(h, p)
        v = verdict(d, se, drift)
        print(f"  {label:30} head {statistics.mean(h):10.3f} pre {statistics.mean(p):10.3f} d {d:+8.3f} "
              f"SE {se:6.3f} vendor drift {drift:6.3f} -> {v}")
        if v == "WORSE":
            blocked.append(label)
        if key == "lat":
            hyp = (v == "BETTER" and d <= -0.30, d)
    print(f"LANDING: {'BLOCKED by WORSE ' + ', '.join(blocked) if blocked else 'not blocked (no WORSE)'}")
    if hyp is not None:
        ok, d = hyp
        inside = expected[0] <= d <= expected[1]
        print(f"HYPOTHESIS (the extra copy is the off-wire gap): {'HOLDS' if ok else 'FALSIFIED'} - d {d:+.3f} ms "
              f"({'inside' if inside else 'outside'} the expected {expected[0]}..{expected[1]})")
    # HEADLINE
    for label, key in METRICS[:2]:
        h = [r[key] for r in head_ok if r[key] is not None]
        f = [r[key] for r in by_arm.get("fastdds", []) if r[key] is not None]
        if h and f:
            pred = "about -0.2 (WIN or DRAW)" if path == "loaned" else "about +0.5 (LOSE: the wire's 0.52 ms)"
            print(f"HEADLINE {label}: head - fastdds {statistics.median(h) - statistics.median(f):+.3f} ms -> "
                  f"{range_rule(h, f)} (predicted {pred})")
    return rc


if __name__ == "__main__":
    sys.exit(main())

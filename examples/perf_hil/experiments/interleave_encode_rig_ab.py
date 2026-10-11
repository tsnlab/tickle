#!/usr/bin/env python3
"""interleave_encode_rig_ab.py - the I4 A/B of encoding a large sample while it is sent (docs/DESIGN.md section 8,
"Encoding while sending"; branch ab/interleave-encode), read by rules written before any rig run (2026-10-11).

The change: a large publish of a type whose wire bytes are its message (perf_test's Array4m) copies each range just
before the send call that carries it, instead of all 4 MB before the first datagram leaves. The hypothesis: that copy
(about 0.7 ms on a Pi 5; Fast DDS pays the same serialisation) comes off the latency path, because the socket's send
buffer holds more wire time (~1.7 ms at 1 Gb/s) than the rest of the copy takes.

Input: one rmw_keepall_rig.sh invocation (its OUT.txt from largemsg_rig_summary.py, and OUT.runs/), arms tickle@head
(the change), tickle@pre (its parent, ba324d3b), fastdds, cyclonedds; one cell (Array4m, 15 Hz, cross-host), RELIABLE
KEEP_LAST 10 or BEST_EFFORT KEEP_LAST 5.

RULES (enforced below):
  TREATMENT  a tickle@head run is VOID unless its publisher's core "large:" line shows encoded_by_range >= 95% of the
             samples it published (the publish really encoded by range); tickle@pre must show no encoded_by_range field
             or 0 in every run, or the whole reading is VOID. A head run with no "large:" line is VOID ("could not look"
             is not "off").
  USABLE     a run whose latency columns are '-' counts for delivery and CPU only. Fewer than 3 usable runs in either
             A/B arm: NO VERDICT.
  DELIVERY   RELIABLE: every tickle@head run delivered 100% with lost 0, or the cell is LOSE whatever the latency says.
             BEST_EFFORT: head's delivered % not below pre's by more than 2 SE.
  A/B        (head vs pre) latency median, p99 lower bound, sub CPU ms/MB, pub CPU ms/MB, sub peak RSS: d = mean(head) -
             mean(pre), SE Welch. BETTER iff d < -2 SE and |d| > the vendors' drift (the larger max-min of the metric
             over fastdds's and cyclonedds's runs); WORSE symmetric; else HELD. Any WORSE blocks landing.
  HYPOTHESIS latency median BETTER with d <= -0.35 ms (half the 0.7 ms moved); expected -0.5..-0.75 ms. Otherwise
             FALSIFIED: the wire queue hid the first datagram's wait, and the 0.7 ms was not on the latency path.
  HEADLINE   head vs fastdds, latency median and p99 lower bound, by the range rule (WIN: every head run below every
             fastdds run; LOSE: every head run above; else DRAW). Predicted about -0.2 ms (WIN or DRAW) with ordinary
             takes. Printed with the prediction; it decides nothing about landing.
Exit 0 when the reading is complete (whatever it says), 2 when VOID or NO VERDICT, 1 on a missing input.
Usage: interleave_encode_rig_ab.py [--reliable | --best-effort] OUT  (OUT.txt and OUT.runs/ must exist; the flag names
       the cell's QoS for the DELIVERY rule, default --reliable)
"""
import math
import re
import statistics
import sys
from pathlib import Path

HEAD, PRE, VENDORS = "tickle@head", "tickle@pre", ("fastdds", "cyclonedds")
RELIABLE_FLAG = True  # --reliable (default) or --best-effort: which cell this is, for the DELIVERY rule
RUN_RE = re.compile(r"^(\S+)\s+l(\d+)\s+(\S+)\s+r(\d+)\s+sent\s+(\d+)\s+recv\s+(\d+)\s+lost\s+(\d+)\s+end\s+-?\d+\s+"
                    r"\(\s*([\d.]+)%\).*?lat med\s+(\S+)\s+p99>=\s+(\S+).*?cpu sub\s+(\S+)\s+pub\s+(\S+)\s+"
                    r"rss\s+(\d+)/\s*(\d+)")
METRICS = (("latency median ms", "lat"), ("latency p99 (lower bound) ms", "p99"), ("sub CPU ms/MB", "cpu_sub"),
           ("pub CPU ms/MB", "cpu_pub"), ("sub peak RSS kB", "rss_sub"))


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
                     "cpu_sub": num(m[11]), "cpu_pub": num(m[12]), "rss_sub": float(m[13])})
    return runs


def treatment(runs_dir, run):
    """encoded_by_range from the run's publisher log: None when it printed no "large:" line, -1 when the line has no
    such field (a build without the change)."""
    log = runs_dir / f"{run['arm']}_{run['topic']}_l0_r{run['rep']}_pub.log"
    if not log.exists():
        return None
    text = log.read_text(errors="replace")
    lines = re.findall(r"large: published=.*", text)
    if not lines:
        return None
    counts = [int(x) for x in re.findall(r"\bencoded_by_range=(\d+)", text)]
    return max(counts) if counts else -1


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
    global RELIABLE_FLAG
    args = sys.argv[1:]
    if args and args[0] in ("--reliable", "--best-effort"):
        RELIABLE_FLAG = args[0] == "--reliable"
        args = args[1:]
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
    print(f"=== interleave-encode A/B: {txt}: {HEAD} against {PRE} ===")
    print("runs: " + ", ".join(f"{a} {len(v)}" for a, v in sorted(by_arm.items())))
    # TREATMENT
    void_all = False
    for r in by_arm.get(PRE, []):
        count = treatment(runs_dir, r)
        if count not in (-1, 0):
            print(f"VOID overall: {PRE} r{r['rep']} encoded_by_range={count} (the parent encodes no range)")
            void_all = True
    head_ok = []
    for r in by_arm.get(HEAD, []):
        count = treatment(runs_dir, r)
        ok = count is not None and count >= 0.95 * r["sent"]
        print(f"  {HEAD} r{r['rep']}: sent {r['sent']} encoded_by_range={count} -> "
              f"{'treated' if ok else 'VOID (treatment not seen)'}")
        if ok:
            head_ok.append(r)
    if void_all:
        return 2
    expected = (-0.75, -0.5)
    # DELIVERY: no runs read is "could not look", never "complete".
    if not by_arm.get(HEAD) or not by_arm.get(PRE):
        print(f"VOID: no {HEAD} or no {PRE} run line read from {txt}")
        return 2
    if RELIABLE_FLAG:
        incomplete = [r for r in by_arm[HEAD] if r["lost"] != 0 or r["pct"] < 100.0]
        print(f"DELIVERY {HEAD} (RELIABLE): {len(by_arm[HEAD])} runs, "
              f"{'complete in every one' if not incomplete else 'INCOMPLETE -> cell LOSE'}")
    else:
        h = [r["pct"] for r in by_arm[HEAD]]
        p = [r["pct"] for r in by_arm[PRE]]
        if len(h) >= 2 and len(p) >= 2:
            d, se = welch(h, p)
            print(f"DELIVERY {HEAD} (BEST_EFFORT): {statistics.mean(h):.2f}% vs pre {statistics.mean(p):.2f}% d {d:+.2f}"
                  f" SE {se:.2f} -> {'LOSE (below pre by more than 2 SE)' if d < -2 * se else 'held'}")
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
            hyp = (v == "BETTER" and d <= -0.35, d)
    if blocked:
        print(f"LANDING: BLOCKED by WORSE {', '.join(blocked)}")
    elif rc != 0:
        print("LANDING: NO VERDICT (a metric could not be read)")
    else:
        print("LANDING: not blocked (no WORSE)")
    if hyp is not None:
        ok, d = hyp
        inside = expected[0] <= d <= expected[1]
        print(f"HYPOTHESIS (the encode before the first datagram is on the latency path): {'HOLDS' if ok else 'FALSIFIED'}"
              f" - d {d:+.3f} ms "
              f"({'inside' if inside else 'outside'} the expected {expected[0]}..{expected[1]})")
    # HEADLINE
    for label, key in METRICS[:2]:
        h = [r[key] for r in head_ok if r[key] is not None]
        f = [r[key] for r in by_arm.get("fastdds", []) if r[key] is not None]
        if h and f:
            pred = "about -0.2 (WIN or DRAW)"
            print(f"HEADLINE {label}: head - fastdds {statistics.median(h) - statistics.median(f):+.3f} ms -> "
                  f"{range_rule(h, f)} (predicted {pred})")
    return rc


if __name__ == "__main__":
    sys.exit(main())

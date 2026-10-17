#!/usr/bin/env python3
"""Cross-host rmw round trip from creation time: reads rmw_crosshost_rtt.sh output files (docs/RESULTS.md rows 48-67).

Usage: rmw_crosshost_creation.py <rmw_crosshost_rtt.sh OUT .txt> [more .txt ...]

Why (the user's decision of 2026-10-17; TESTING.md section 4): latency is measured from the data's creation time.
rmw_perf_pingpong's ping_node (since 2026-10-17) records each answered ping's due time - when the sleep before it was
asked to end (block, locked and jitter waits), or its grid point (--phase random) - and appends to its RESULT line
rtt_p50_ms crt_p50_ms crt_p99_ms crt_avg_ms crt_kept gap_p50_us gap_min_us gap_max_us crt_no_due: crt = reply - due,
gap = send - due, over every answered round trip, the same round trips rtt_avg_ms is over. The ping is the same binary
for every rmw, so its sleep is the same for all of them; the control below checks that nothing else waits.

HOW TO READ IT, written 2026-10-17 before the first run and enforced below:
  C1 SAMPLES FIRST. Only rows the harness marked ok (identity, no loss, a RESULT naming the rmw) are read. Per row:
     recv, crt_kept and crt_no_due are printed before any value; a row with no crt_* fields (a ping_node built before
     the change), crt_no_due > 0 or crt_kept != recv is NOT READ - never a gap of 0. A mean or p50 is read with >= 50
     round trips per rep (the rows have 100), a p99 never (< 500). A framework with fewer than 2 readable reps in a
     cell is NOT READ there.
  C2 CONTROL per cell and rmw: creation_latency_summary.py's R4 (rtt_r4): g <= 0.2 ms, no gap below -0.02 ms, |d - g|
     <= the reps' stamp-p50 range + 1 us. It needs rmw_tickle and one scored vendor read; rmw_zenoh_cpp is printed and
     never decides it. A failed control makes the cell's creation verdicts NOT READ.
  C3 SCORING, each scored vendor separately (rmw_fastrtps_cpp, rmw_cyclonedds_cpp; rmw_zenoh_cpp is reference): rows
     48-67 are means, so the primary figure is the creation mean (crt_avg_ms), median of the reps. The verdict uses the
     rule each row was published with: block wait (W, rows 48-51), the range rule of TESTING.md section 4 (WIN when
     every rmw_tickle rep is below every vendor rep, LOSE when every vendor rep is below every rmw_tickle rep, else
     DRAW); poll wait (J, rows 52-67), overlap of the reps' interquartile ranges (WIN when rmw_tickle's Q3 is below the
     vendor's Q1, LOSE when the vendor's Q3 is below rmw_tickle's Q1, else DRAW). The other rule is printed beside it,
     and so is the send-stamp mean's verdict (the old figure, a diagnostic), so a verdict that moves with the
     definition shows.
"""
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from creation_latency_summary import range_verdict, rtt_r4  # noqa: E402

N_MED = 50
SUBJECT = "rmw_tickle"
SCORED = ("rmw_fastrtps_cpp", "rmw_cyclonedds_cpp")
REFERENCE = ("rmw_zenoh_cpp",)
ROW = re.compile(r"^(rmw_\w+) (\S+) (best_effort|reliable) rep(\d+)(.*?) \| (\S+) \| (.*)$")


def iqr_verdict(subj, other):
    """The J rows' rule: overlap of the reps' interquartile ranges."""
    if len(subj) < 2 or len(other) < 2:
        return "too few reps"
    sq = st.quantiles(subj, n=4, method="inclusive")
    oq = st.quantiles(other, n=4, method="inclusive")
    if sq[2] < oq[0]:
        return "WIN"
    if oq[2] < sq[0]:
        return "LOSE"
    return "DRAW"


def num(f, k):
    try:
        return float(f[k])
    except (KeyError, ValueError):
        return None


def read(paths):
    cells = {}
    for path in paths:
        for line in Path(path).read_text(errors="replace").splitlines():
            m = ROW.match(line)
            if not m:
                continue
            rmw, msg, qos, rep, tags, verdict, rest = m.groups()
            f = dict(re.findall(r"(\w+)=(\S+)", rest))
            wait = re.search(r"wait=(\S+)", tags)
            psleep = re.search(r"poll_sleep_us=(\S+)", tags)
            key = (msg, qos, wait.group(1) if wait else f.get("wait", "?"), psleep.group(1) if psleep else "-")
            cells.setdefault(key, {}).setdefault(rmw, []).append((int(rep), verdict, f, Path(path).name))
    return cells


def readable(rows):
    """C1: (readable field dicts, printed lines)."""
    good, lines = [], []
    for rep, verdict, f, src in sorted(rows, key=lambda r: (r[3], r[0])):
        recv, kept, no_due = num(f, "recv"), num(f, "crt_kept"), num(f, "crt_no_due")
        why = []
        if verdict != "ok":
            why.append(f"row {verdict}")
        elif kept is None or "crt_avg_ms" not in f:
            why.append("no crt_* fields")
        elif no_due or kept != recv:
            why.append(f"crt_kept {kept:.0f} != recv {recv} or crt_no_due {no_due}")
        elif kept < N_MED:
            why.append(f"{kept:.0f} round trips < {N_MED}")
        lines.append(f"    rep{rep} ({src}) recv {recv} crt_kept {kept} crt_no_due {no_due}"
                     + (f"  NOT READ: {'; '.join(why)}" if why else ""))
        if not why:
            good.append(f)
    return good, lines


def cell_report(key, by_rmw):
    msg, qos, wait, psleep = key
    rule = "range" if wait == "block" else "iqr"
    print(f"\n=== {msg} {qos} wait={wait} poll_sleep_us={psleep} (primary rule: {rule}) ===")
    vals, ctl_ok, read_ok = {}, True, []
    for rmw in (SUBJECT,) + SCORED + REFERENCE:
        if rmw not in by_rmw:
            continue
        good, lines = readable(by_rmw[rmw])
        print(f"  {rmw}:")
        print("\n".join(lines))
        if len(good) < 2:
            print(f"    NOT READ ({len(good)} readable reps)")
            continue
        v = {k: [num(f, k) for f in good] for k in ("crt_avg_ms", "crt_p50_ms", "rtt_avg_ms", "rtt_p50_ms",
                                                    "gap_p50_us")}
        gmin = min(num(f, "gap_min_us") for f in good) / 1e3
        ok, text = rtt_r4(v["rtt_p50_ms"], v["crt_p50_ms"], [g / 1e3 for g in v["gap_p50_us"]], gmin)
        print(f"    creation mean {st.median(v['crt_avg_ms']):.4f} [{min(v['crt_avg_ms']):.4f}-"
              f"{max(v['crt_avg_ms']):.4f}] p50 {st.median(v['crt_p50_ms']):.4f} ms | stamp mean "
              f"{st.median(v['rtt_avg_ms']):.4f} [{min(v['rtt_avg_ms']):.4f}-{max(v['rtt_avg_ms']):.4f}] ms | gap p50 "
              f"{st.median(v['gap_p50_us']):.1f} us")
        print(f"    control (R4): {text}")
        vals[rmw] = v
        if rmw not in REFERENCE:
            ctl_ok = ctl_ok and ok
            read_ok.append(rmw)
    if SUBJECT not in read_ok or not any(v in read_ok for v in SCORED):
        ctl_ok = False
    print(f"  CONTROL {'PASS' if ctl_ok else 'FAIL'}")
    out = {}
    for vendor in SCORED:
        if SUBJECT not in vals or vendor not in vals:
            print(f"  vs {vendor}: NOT READ")
            continue
        t, o = vals[SUBJECT], vals[vendor]
        rng = range_verdict(t["crt_avg_ms"], o["crt_avg_ms"])
        iqr = iqr_verdict(t["crt_avg_ms"], o["crt_avg_ms"])
        primary = rng if rule == "range" else iqr
        if not ctl_ok:
            primary = "NOT READ (control failed)"
        stamp = (range_verdict if rule == "range" else iqr_verdict)(t["rtt_avg_ms"], o["rtt_avg_ms"])
        print(f"  vs {vendor}: creation mean {primary} (range {rng}, iqr {iqr}); stamp mean (diagnostic, {rule}) "
              f"{stamp}")
        out[vendor] = (primary, stamp)
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cells = read(argv[1:])
    if not cells:
        print("no rows read")
        return 1
    for key in sorted(cells):
        cell_report(key, cells[key])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

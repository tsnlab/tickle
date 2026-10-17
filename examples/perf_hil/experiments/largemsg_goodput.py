#!/usr/bin/env python3
"""The L4 large-message table on the goodput basis, from existing raw runs (no new measurement; 2026-10-17).

Usage: largemsg_goodput.py <OUT.runs> [pub|good]

The user's decision (2026-10-17, TESTING.md section 4): CPU is scored per DELIVERED sample, saturating cells included.
This re-uses largemsg_rig_summary.py with every reading rule unchanged and changes ONE thing in mode "good": the
publisher's CPU is divided by the MB DELIVERED instead of the MB published. Mode "pub" changes nothing and must
reproduce the published letters - that is this script's control; a "good" result is only read beside a "pub" run that
reproduced RESULTS.md.

Definition, over each process's own steady seconds (the same warm-up and cool-down as today):
    cpu_pub_good = cpu_pub x (publisher's published rate / subscriber's delivered rate)
i.e. the publisher's CPU per steady second over the delivered MB per steady second. The subscriber's CPU (cpu_sub) is
already per MB delivered, and perf_test counts a sample only when the rmw hands it over whole, so a partly received
large sample is already 0 and delivered % already ranks as goodput does. A run that delivered nothing gets
cpu_pub_good = nan (never 0), as cpu_sub does today.

The rows RESULTS.md cites were produced by running both modes over
    l4main2_{i1rel,i1be,i4rel,i4be,i4s}_20261015-094605.runs and l4main3_i1rel_20261016-065517.runs
in ~/rig_results_safe/.
"""
import os
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import largemsg_rig_summary as L  # noqa: E402 - beside this file, found through the path set just above
from rmw_keepall_rig_summary import rows, whole_counts  # noqa: E402

orig_run_record = L.run_record


def run_record(runs, stem, fields, samehost, rate, wire=True):
    rec = orig_run_record(runs, stem, fields, samehost, rate, wire)
    rec["cpu_pub_good"] = float("nan")
    rec["pub_rate_hz"] = float("nan")
    if rec.get("void"):
        return rec
    pub, _ = rows(runs / f"{stem}_pub.log")
    whole_counts(pub)
    steady, _ = L.steady_window(pub, "n_sent")
    if steady:
        rec["pub_rate_hz"] = st.mean(L.counts(r, "sent") / r["D"] for r in steady)
    if rec.get("rate_hz", 0) > 0 and rec["pub_rate_hz"] == rec["pub_rate_hz"]:
        rec["cpu_pub_good"] = rec["cpu_pub"] * rec["pub_rate_hz"] / rec["rate_hz"]
    return rec


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.split("\n\n")[1])
    L.run_record = run_record
    mode = sys.argv[2] if len(sys.argv) > 2 else "good"
    if mode not in ("pub", "good"):
        sys.exit("mode is pub or good")
    if mode == "good":
        L.METRICS = tuple((("cpu_pub_good", "pub CPU ms/MB DELIVERED", "lower", False) if m[0] == "cpu_pub" else m)
                          for m in L.METRICS)
    meta, samehost, recs = L.load(Path(sys.argv[1]))
    print(f"== {sys.argv[1]} mode={mode}")
    for r in recs:
        if r["void"]:
            print(f"  run {r['arm']:16} r{r['rep']} VOID {r['void'][:80]}")
            continue
        print(f"  run {r['arm']:16} r{r['rep']} recv {r['recv']} sent {r['sent']} lost {r['lost']} offered "
              f"{r['offered']} sub_rate {r['rate_hz']:.3f} pub_rate {r['pub_rate_hz']:.3f} cpu_pub {r['cpu_pub']:.3f}"
              f" -> good {r['cpu_pub_good']:.3f} cpu_sub {r['cpu_sub']:.3f}")
    L.l4(recs, meta, samehost)


if __name__ == "__main__":
    main()

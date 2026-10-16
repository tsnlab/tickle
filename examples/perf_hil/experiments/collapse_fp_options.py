#!/usr/bin/env python3
"""The options table of the collapse-false-positive campaign (2026-10-17), from collapse_fp_sweep.sh labels:

  arm_o<opt>            test_frame_loss_never_moves_the_cap, seeds 1-N: seeds failing, collapses called
  sq_o<opt>_l<ppm>      cfp_squeeze: the real collapse - detected within the run, seconds from the first refused run to
                        the collapse, runs refused by then; "early" = called before any run was refused (false)
  pl_o<opt>_l<ppm>      cfp_plain (16 KB at 100 Hz, no squeeze): every collapse is false - per tick and per stream

  collapse_fp_options.py <out_dir>

Reading rules (pre-registered before the option runs were read): an option is better on false collapses when its
plain-loss rate is lower in every loss cell with at least one collapse in either arm, and not worse on detection when
its median delay is no longer and it detects in as many runs, in every squeeze cell. A cell whose runs did not all
report (missing CFP_R) is VOID, never read as a value.
"""
import glob
import os
import re
import statistics
import sys

R = re.compile(r"CFP_R (\w+) (.*)")


def results(out, label):
    rows, missing = [], 0
    verdicts = os.path.join(out, label + ".verdicts")
    seeds = [l.split()[0] for l in open(verdicts) if l[0].isdigit()]
    for seed in seeds:
        found = None
        for line in open(os.path.join(out, label, seed + ".log"), errors="replace"):
            m = R.search(line)
            if m:
                found = dict(kv.split("=") for kv in m.group(2).split())
        if found is None:
            missing += 1
        else:
            rows.append(found)
    return seeds, rows, missing


def main():
    out = sys.argv[1]
    print("arm: test_frame_loss_never_moves_the_cap")
    for path in sorted(glob.glob(os.path.join(out, "arm_o*.verdicts"))):
        label = os.path.basename(path)[:-len(".verdicts")]
        lines = [l.split() for l in open(path) if l[0].isdigit()]
        fails = [s for s, rc in lines if rc != "0"]
        print("  %-8s seeds %4d fail %3d  %s" % (label, len(lines), len(fails), " ".join(fails[:12])))
    print("squeeze: delay = collapsed_at - refused_at (s); refused = runs refused by the collapse")
    for path in sorted(glob.glob(os.path.join(out, "sq_o*_l*.verdicts"))):
        label = os.path.basename(path)[:-len(".verdicts")]
        seeds, rows, missing = results(out, label)
        done = [r for r in rows if int(r["collapsed_at"]) > 0 and int(r["refused_at"]) > 0]
        early = [r for r in rows if int(r["collapsed_at"]) > 0 and int(r["refused_then"]) == 0]
        delays = [int(r["collapsed_at"]) - int(r["refused_at"]) for r in done]
        refused = [int(r["refused_then"]) for r in done]
        never = [r for r in rows if int(r["collapsed_at"]) == 0]
        void = " VOID (%d missing)" % missing if missing else ""
        if delays:
            print("  %-14s runs %3d detected %3d never %3d early %2d delay median %.1f mean %.2f max %d  refused mean %.0f"
                  "%s" % (label, len(rows), len(done), len(never), len(early), statistics.median(delays),
                          statistics.mean(delays), max(delays), statistics.mean(refused), void))
        else:
            print("  %-14s runs %3d detected 0 never %3d%s" % (label, len(rows), len(never), void))
    print("plain loss, 16 KB at 100 Hz: false collapses")
    for path in sorted(glob.glob(os.path.join(out, "pl_o*_l*.verdicts"))):
        label = os.path.basename(path)[:-len(".verdicts")]
        seeds, rows, missing = results(out, label)
        collapses = sum(int(r["collapses"]) for r in rows)
        ticks = sum(int(r["ticks"]) for r in rows)
        streams = sum(1 for r in rows if int(r["collapses"]) > 0)
        void = " VOID (%d missing)" % missing if missing else ""
        print("  %-14s streams %3d with a collapse %3d  collapses %4d / %6d ticks = %.2e%s" %
              (label, len(rows), streams, collapses, ticks, collapses / ticks if ticks else 0, void))


if __name__ == "__main__":
    main()

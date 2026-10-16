#!/usr/bin/env python3
"""Reads a collapse_fp_sweep.sh label: every failing seed classified by cause, from the run's own decision trace.

  collapse_fp_read.py <out_dir> <label> [<label> ...]

Reading rules (pre-registered 2026-10-17, before the 1,000-seed run was read):
- A seed fails by its binary's exit code; within it, a test fails by its CFP_T failure count. A seed with rc != 0 and
  no failing CFP_T (a crash, a timeout) is its own class, "no-verdict", never folded into a cause.
- A collapse ("v=C") in a test whose link has no reassembly squeeze and no fragment filter is false by construction:
  test_frame_loss_never_moves_the_cap, test_best_effort_loss_never_tears, test_frame_loss_is_not_a_failed_path,
  test_receiver_drops_never_make_a_lossy_path_look_clean, cfp_plain. In test_a_reassembly_squeeze_end_to_end the
  first collapse is the real one; a later one is listed with its window and judged by the failing assertion's line.
- A false collapse is "baseline-low" when the bound it was tested against, p_hi (phi), was below the link's true p -
  the MTU baseline underestimated the loss - and "tail" when p_hi >= p: the window's own binomial tail against a
  baseline that was right.
- A failing test with no false collapse before its failing assertion is "other", printed with its assertion lines.
"""
import collections
import os
import re
import sys

TRUE_P = {
    "test_frame_loss_never_moves_the_cap": 0.01,
    "test_best_effort_loss_never_tears": 0.01,
    "test_frame_loss_is_not_a_failed_path": 0.05,
    "test_receiver_drops_never_make_a_lossy_path_look_clean": 0.05,
    "test_a_reassembly_squeeze_end_to_end": 0.001,
}
PLAIN = {k for k in TRUE_P if k != "test_a_reassembly_squeeze_end_to_end"}
DEC = re.compile(r"CFP_D (\S+) ([\d.]+) cap=(\d+)->(\d+) wn=(\d+) sent=(\d+) lost=(\d+) fr=(\d+) ev=(\d+) mf=(\d+) "
                 r"phi=(\S+) L0=(\S+) z=(\S+) v=(\w)")
TEST = re.compile(r"CFP_T (\S+) (\d+) (\d+)")
EXPECT = re.compile(r"test_hybrid_send\.c:(\d+): (.*)")


def read_seed(path):
    """Per test, in order: its decisions, its failing assertion lines, its failure count."""
    decisions, expects, results = collections.defaultdict(list), collections.defaultdict(list), {}
    pending_expects = []
    for line in open(path, errors="replace"):
        m = DEC.search(line)
        if m:
            decisions[m.group(1)].append(m.groups())
            continue
        m = EXPECT.search(line)
        if m:
            pending_expects.append(m.group(1) + ": " + m.group(2))
            continue
        m = TEST.search(line)
        if m:
            results[m.group(1)] = int(m.group(2))
            expects[m.group(1)] = pending_expects
            pending_expects = []
    return decisions, expects, results


def describe(d):
    return "t=%s cap %s->%s n=%s runs %s lost %s, MTU %s ev/%s fr, p_hi %s L0 %s z %s" % (
        d[1], d[2], d[3], d[4], d[5], d[6], d[8], d[9], d[10], d[11], d[12])


def classify(test, collapses):
    false = collapses if test in PLAIN else collapses[1:]
    if not false:
        return "other", []
    first = false[0]
    return ("baseline-low" if float(first[10]) < TRUE_P[test] else "tail"), false


def main():
    out = sys.argv[1]
    for label in sys.argv[2:]:
        verdicts = [l.split() for l in open(os.path.join(out, label + ".verdicts")) if l[0].isdigit()]
        failing = [(int(s), int(rc)) for s, rc in verdicts if rc != "0"]
        classes = collections.Counter()
        windows = collections.Counter()
        false_by_test = collections.Counter()
        print("== %s: %d seeds, %d fail" % (label, len(verdicts), len(failing)))
        for seed in [int(s) for s, _ in verdicts]:
            decisions, _, _ = read_seed(os.path.join(out, label, "%d.log" % seed))
            for test, ds in decisions.items():
                if test in PLAIN or test.startswith("cfp_plain"):
                    windows[test] += sum(1 for d in ds if int(d[5]) >= 16 and d[13] in "GUXCP")
                    false_by_test[test] += sum(1 for d in ds if d[13] == "C")
        for seed, rc in failing:
            decisions, expects, results = read_seed(os.path.join(out, label, "%d.log" % seed))
            bad = [t for t, n in results.items() if n > 0]
            if not bad:
                classes["no-verdict"] += 1
                print("seed %d rc %d: no failing test reported (crash or timeout)" % (seed, rc))
                continue
            for test in bad:
                collapses = [d for d in decisions[test] if d[13] == "C"]
                cls, false = classify(test, collapses)
                classes[(test, cls)] += 1
                print("seed %d %s: %s" % (seed, test, cls))
                for e in expects[test]:
                    print("    expect " + e)
                for d in false:
                    print("    false collapse " + describe(d))
                if cls == "other":
                    for d in collapses:
                        print("    collapse " + describe(d))
        print("-- classes")
        for k, v in sorted(classes.items(), key=str):
            print("   %s %d" % (k, v))
        print("-- false collapses per window, plain-loss tests (windows of >= 16 runs decided or gathering)")
        for test in sorted(windows):
            print("   %s %d / %d = %.2e" % (test, false_by_test[test], windows[test],
                                         false_by_test[test] / windows[test] if windows[test] else 0))


if __name__ == "__main__":
    main()

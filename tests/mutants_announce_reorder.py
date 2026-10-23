#!/usr/bin/env python3
"""Mutant sweep for announce_superseded() (tickle.c, 2026-10-23): an announce older than the one acted on from its
source, arriving soon after it, is ignored instead of replacing the newer list. Each mutant breaks one piece and must
turn tests/test_peer_discovery.c red; the control (no mutation) must pass, or a red mutant says nothing.

Works on a copy: src/, include/ and tests/ go to a scratch directory, one mutation at a time is applied there, the test
is built with gcc and run. Nothing in the tree is touched. Refuses to start (exit 2) unless every anchor occurs exactly
once in the tree's file, checked for all mutants before the control runs; every child is bounded (tests/mutant_guard.py:
2 GB of address space, a deadline, at most 256 KB of its output kept). Importing this file runs nothing. Run from
anywhere; exit 0 when every mutant is caught and the control passes."""
import os
import shutil
import sys
import tempfile

from mutant_guard import be_nice, check_anchors, killed_by, run_capped, usage_line

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_REL = "src/tickle.c"
CHECK = "    if (announce_superseded(node, source, generation)) {"
OLDER = "    bool older = generation == 0 || seq_delta(generation, applied) < 0;"
SEEN = "    if (!node->update_seen[source] || applied == 0) {"
WINDOW = "    return older && tt_get_ns() - node->update_applied_ns[source] < ANNOUNCE_REORDER_WINDOW_NS;"

MUTANTS = [
    ("control (no mutation)", None, None),
    ("never superseded: every other generation applied, as before", CHECK, "    if (false) {"),
    ("no window: an older generation ignored however late", WINDOW, "    return older;"),
    ("generation 0 ordered serially", OLDER, "    bool older = seq_delta(generation, applied) < 0;"),
    ("unsigned order: a wrapped generation is newer", OLDER,
     "    bool older = generation == 0 || generation < applied;"),
    ("a newer generation ignored too", OLDER, "    bool older = generation != applied;"),
    ("an applied generation 0 ordered serially", SEEN, "    if (!node->update_seen[source]) {"),
    ("the window timed from nothing", "    node->update_applied_ns[source] = tt_get_ns();\n",
     "    (void)0;\n"),
]


def run(work, old, new):
    """Builds and runs the test on a fresh copy with one mutation: (verdict, output)."""
    shutil.rmtree(work, ignore_errors=True)
    for d in ("src", "include", "tests"):
        shutil.copytree(os.path.join(REPO, d), os.path.join(work, d))
    if old is not None:
        target = os.path.join(work, SOURCE_REL)
        with open(target, encoding="utf-8") as f:
            text = f.read()
        with open(target, "w", encoding="utf-8") as f:
            f.write(text.replace(old, new, 1))
    binary = os.path.join(work, "t")
    build = run_capped(["gcc", "-O0", "-g", "-pthread", "-I", f"{work}/include", "-I", f"{work}/src", "-I",
                        f"{work}/tests", "-o", binary, f"{work}/tests/test_peer_discovery.c", f"{work}/src/encoding.c",
                        f"{work}/src/log.c", "-lm"], timeout=600)
    if build.returncode != 0:
        return "BUILD FAILED", build.output[-300:]
    result = run_capped([binary], timeout=120)
    if result.timed_out:
        return "failed (timeout)", result.output
    lines = [line for line in result.output.splitlines() if "expected" in line]
    return ("passed" if result.returncode == 0 else "failed") + (f": {lines[0]}" if lines else ""), result.output


def main():
    be_nice()
    check_anchors(REPO, [(name, SOURCE_REL, old, new) for name, old, new in MUTANTS if old is not None])
    scratch = tempfile.mkdtemp(prefix="mutants_announce_reorder_", dir=os.environ.get("TMPDIR"))
    work = os.path.join(scratch, "tree")
    failures = 0
    try:
        for name, old, new in MUTANTS:
            verdict, output = run(work, old, new)
            ok = verdict.startswith("passed") if old is None else verdict.startswith("failed")
            failures += 0 if ok else 1
            print(f"{'PASS' if ok else 'FAIL'}  {name}: {verdict}", flush=True)
            if old is None and not ok:
                print(output[-2000:])
                print("control did not pass; the sweep cannot decide anything")
                return 1
            if old is not None and ok:
                print(f"      killed by: {', '.join(killed_by(output, REPO)) or '(no failing check printed)'}")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print(usage_line())
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

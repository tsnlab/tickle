#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Mutants for a RELIABLE reader's declined or Heartbeat-only tail (2026-10-09).

Each mutant reverts one part of the fix in src/tickle.c and must make its test fail:
  empty_bitmap_stop  acknack_retry() stops on an empty received_bitmap again, ignoring a gap only a Heartbeat shows
  no_note_declined   a decline leaves no trace at all again: nothing asks for a tail no Heartbeat announced
  decline_sends      a decline sends an ACKNACK at once instead of arming the timer
  no_new_gap_acknack update_reliable_ack()'s new-gap ACKNACK removed: a decline's armed timer then holds a gap back
  rmw_held_small     both of the first two together, against rmw test_loaned_messages' held-small case (--rmw only:
                     it rebuilds rmw_tickle through make test-rmw, ~1 min a try, and needs the ROS workspace). That
                     case catches the old code in about half its runs (5 of 8, 2026-10-09: whether the tail is
                     declined depends on the timing), so the mutant gets RMW_TRIES tries and is killed by any one;
                     the unit test is the deterministic kill.
A control run of the unmutated source must pass every test first, so a test that fails for another reason cannot
kill a mutant. src/tickle.c is restored, with a fresh mtime so make rebuilds, whatever happens.

Usage: tests/mutants_declined_tail.py [--rmw]   (from the repository root)
"""

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "src", "tickle.c")
LINUX = os.path.join(REPO, "platform", "linux")
UNIT = "test_reliable_pubsub"
RMW = "rmw"
RMW_TRIES = 3

EMPTY_BITMAP_STOP = ("    if (highest_relevant_bit(proxy) < 0) {\n        // A DATA arrival already closed the gap",
                     "    if (bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy))) {\n"
                     "        // A DATA arrival already closed the gap")
NO_NOTE_DECLINED = ("        note_declined(node, sub, ctx);\n        return;\n", "        return;\n")

MUTANTS = [
    ("empty_bitmap_stop", UNIT, [EMPTY_BITMAP_STOP]),
    ("no_note_declined", UNIT, [NO_NOTE_DECLINED]),
    ("decline_sends", UNIT,
     [("    if (!proxy->acknack_scheduled) {\n        if (tt_Context_schedule(node, tt_get_ns() + "
       "reliable_retry_interval(node, proxy), acknack_retry, proxy)) {\n",
       "    maybe_arm_acknack_retry(node, proxy);\n    if (false) {\n        if (tt_Context_schedule(node, tt_get_ns() + "
       "reliable_retry_interval(node, proxy), acknack_retry, proxy)) {\n")]),
    ("no_new_gap_acknack", UNIT,
     [("    if (retry_already_armed && new_gap.low_bit >= 0) {\n", "    if (false && new_gap.low_bit >= 0) {\n")]),
    ("rmw_held_small", RMW, [EMPTY_BITMAP_STOP, NO_NOTE_DECLINED]),
]


def run_test(name):
    """Builds and runs one test - a unit test binary, or the rmw suite; True if it passed."""
    if name == RMW:
        run = subprocess.run(["make", "test-rmw"], cwd=REPO, capture_output=True, text=True, check=False)
        lines = [line for line in run.stdout.splitlines() if "held-small" in line or "run_rmw_suite:" in line]
        print("\n".join(f"  {line[:160]}" for line in lines))  # the witness's own words, kept whole enough to read
        return run.returncode == 0 and "run_rmw_suite: all tests passed" in run.stdout
    target = os.path.join("obj", "debug", "tests", name)
    build = subprocess.run(["make", "-s", target], cwd=LINUX, capture_output=True, text=True, check=False)
    if build.returncode != 0:
        print(f"  build of {name} failed:\n{build.stdout[-2000:]}{build.stderr[-2000:]}")
        return False
    run = subprocess.run([os.path.join(LINUX, target)], cwd=LINUX, capture_output=True, text=True, check=False)
    return run.returncode == 0


def main():
    with_rmw = "--rmw" in sys.argv[1:]
    mutants = [m for m in MUTANTS if with_rmw or m[1] != RMW]
    tests = sorted({m[1] for m in mutants})
    original = open(SOURCE, encoding="utf-8").read()
    failures = 0
    try:
        for name in tests:
            if not run_test(name):
                print(f"CONTROL FAILED: {name} fails on the unmutated source - no mutant can be judged")
                return 2
        print(f"control: {', '.join(tests)} pass on the unmutated source")
        for label, test, edits in mutants:
            mutated = original
            applied = True
            for before, after in edits:
                if mutated.count(before) != 1:
                    print(f"MUTANT {label}: its text occurs {mutated.count(before)} times, not once - not applied")
                    applied = False
                    break
                mutated = mutated.replace(before, after, 1)
            if not applied:
                failures += 1
                continue
            with open(SOURCE, "w", encoding="utf-8") as out:
                out.write(mutated)
            tries = RMW_TRIES if test == RMW else 1
            killed = any(not run_test(test) for _ in range(tries))
            print(f"mutant {label}: {'killed' if killed else 'SURVIVED'} by {test}")
            failures += 0 if killed else 1
    finally:
        with open(SOURCE, "w", encoding="utf-8") as out:
            out.write(original)  # a fresh mtime: make rebuilds from the restored source
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

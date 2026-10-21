#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Mutants for a full scheduler made safe and visible (2026-10-16, tests/test_scheduler_headroom.c).

Each mutant removes one part of the change in src/tickle.c and must make the test fail:
  no_room_check        a request is served with no scheduler room: its callback runs and the request is dropped
                       (the check is server_can_take_request()'s since a54ab490)
  inbox_uncounted      the room check ignores entries waiting in the scheduler inbox
  refusal_uncounted    tt_Context_schedule() refuses an entry without counting or warning
  not_counted          a refusal is not counted (sched_full)
  warn_every_time      the warning is not rate-limited
  peak_not_kept        the peak occupancy is not kept (sched_peak)
A control run of the unmutated source must pass first, so a test that fails for another reason cannot kill a mutant.

Refuses to start (exit 2) if any anchor is not in src/tickle.c exactly once - no_room_check matched nothing from
a54ab490, when the check moved into server_can_take_request(), and the sweep ran on counting it as one failure among
the rest - or if src/tickle.c is already modified. Every child is bounded (tests/mutant_guard.py: 2 GB of address space,
a deadline, at most 256 KB of its output kept); src/tickle.c is restored after each mutant, on an exception and on
SIGINT/SIGTERM/SIGHUP, with a fresh mtime so make rebuilds. A mutant whose test binary, debug sections aside, is the
control's (tests/mutant_digest.py) is reported DEAD (NOT_COMPILED) and counted as a failure. MUTANTS=<substring> runs
only the mutants whose label contains it.

Usage: tests/mutants_scheduler_headroom.py   (from the repository root)
"""

import os
import sys

from mutant_digest import NOT_COMPILED, code_digest, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_REL = "src/tickle.c"
SOURCE = os.path.join(REPO, SOURCE_REL)
LINUX = os.path.join(REPO, "platform", "linux")
TEST = "test_scheduler_headroom"
BIN = os.path.join(LINUX, "obj", "debug", "tests", TEST)

MUTANTS = [
    ("no_room_check",
     "    if (!sched_has_room(node)) {\n        sched_note_full(node);\n        return false;\n",
     "    if (!sched_has_room(node) && false) {\n        sched_note_full(node);\n        return false;\n"),
    ("inbox_uncounted",
     "    return (uint32_t)node->scheduler_tail + queued + 1U < (uint32_t)tt_MAX_SCHEDULER_LENGTH;\n",
     "    return (uint32_t)node->scheduler_tail + (queued * 0U) + 1U < (uint32_t)tt_MAX_SCHEDULER_LENGTH;\n"),
    ("refusal_uncounted",
     "        sched_heap_insert(node, time, function, param);\n    } else {\n        sched_note_full(node);\n    }\n",
     "        sched_heap_insert(node, time, function, param);\n    }\n"),
    ("not_counted",
     "    node->sched_full++;\n",
     "\n"),
    ("warn_every_time",
     "    if (node->sched_full_warned_ns != 0 &&\n        now - node->sched_full_warned_ns < discovery_sched_full_warn_interval_ns(node)) {\n",
     "    if (node->sched_full_warned_ns != 0 &&\n        now - node->sched_full_warned_ns < discovery_sched_full_warn_interval_ns(node) && false) {\n"),
    ("peak_not_kept",
     "        node->sched_peak = node->scheduler_tail;\n",
     "\n"),
]


def run_test(control=None):
    """Builds and runs the test binary: PASS, FAIL, HANG, BUILD_FAILED, or NOT_COMPILED if it is the control's."""
    build = run_capped(["make", "-s", "-j2", os.path.relpath(BIN, LINUX)], timeout=900, cwd=LINUX)
    if build.returncode != 0:
        # Not a kill: a mutant that does not compile is wrongly written, and says nothing about the test.
        print(f"  build of {TEST} failed:\n{build.output[-2000:]}")
        return "BUILD_FAILED", ""
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    run = run_capped([BIN], timeout=600, cwd=LINUX)
    if run.timed_out:
        return "HANG", run.output  # a kill too: the test did not pass
    return ("PASS" if run.returncode == 0 else "FAIL"), run.output


def main():
    be_nice()
    check_anchors(REPO, [(label, SOURCE_REL, before, after) for label, before, after in MUTANTS])
    only = os.environ.get("MUTANTS", "")
    failures = 0
    with SourceGuard(REPO, [SOURCE_REL]) as guard:
        verdict, output = run_test()
        if verdict != "PASS":
            print(output[-2000:])
            print(f"CONTROL FAILED: {TEST} fails on the unmutated source - no mutant can be judged")
            return 2
        control = code_digest(BIN)
        print("control: the test passes on the unmutated source", flush=True)
        original = guard.original(SOURCE_REL)
        for label, before, after in MUTANTS:
            if only and only not in label:
                continue
            with open(SOURCE, "w", encoding="utf-8") as out:
                out.write(original.replace(before, after, 1))
            try:
                verdict, output = run_test(control)
            finally:
                guard.restore()
            killed = verdict in ("FAIL", "HANG")
            outcome = "killed" if killed else ("DEAD (NOT_COMPILED)" if verdict == NOT_COMPILED else "SURVIVED")
            print(f"mutant {label}: {outcome} ({verdict})", flush=True)
            if killed:
                print(f"  killed by: {', '.join(killed_by(output, REPO)) or '(no failing check printed)'}")
            failures += 0 if killed else 1
    print(usage_line())
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

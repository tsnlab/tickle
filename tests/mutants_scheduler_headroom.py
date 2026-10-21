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
  inbox_uncounted      the room check ignores entries waiting in the scheduler inbox
  refusal_uncounted    tt_Context_schedule() refuses an entry without counting or warning
  not_counted          a refusal is not counted (sched_full)
  warn_every_time      the warning is not rate-limited
  peak_not_kept        the peak occupancy is not kept (sched_peak)
A control run of the unmutated source must pass first, so a test that fails for another reason cannot kill a mutant.
src/tickle.c is restored, with a fresh mtime so make rebuilds, whatever happens.

Usage: tests/mutants_scheduler_headroom.py   (from the repository root)
"""

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "src", "tickle.c")
LINUX = os.path.join(REPO, "platform", "linux")
TEST = "test_scheduler_headroom"

MUTANTS = [
    ("no_room_check",
     "        if (!sched_has_room(node)) {\n            sched_note_full(node);\n            return true;\n",
     "        if (!sched_has_room(node) && false) {\n            sched_note_full(node);\n            return true;\n"),
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


def run_test():
    """Builds and runs the test binary; True if it passed."""
    target = os.path.join("obj", "debug", "tests", TEST)
    build = subprocess.run(["make", "-s", target], cwd=LINUX, capture_output=True, text=True, check=False)
    if build.returncode != 0:
        print(f"  build of {TEST} failed:\n{build.stdout[-2000:]}{build.stderr[-2000:]}")
        return False
    run = subprocess.run([os.path.join(LINUX, target)], cwd=LINUX, capture_output=True, text=True, check=False,
                         timeout=600)
    return run.returncode == 0


def main():
    original = open(SOURCE, encoding="utf-8").read()
    failures = 0
    try:
        if not run_test():
            print(f"CONTROL FAILED: {TEST} fails on the unmutated source - no mutant can be judged")
            return 2
        print("control: the test passes on the unmutated source")
        for label, before, after in MUTANTS:
            if original.count(before) != 1:
                print(f"MUTANT {label}: its text occurs {original.count(before)} times, not once - not applied")
                failures += 1
                continue
            with open(SOURCE, "w", encoding="utf-8") as out:
                out.write(original.replace(before, after, 1))
            killed = not run_test()
            print(f"mutant {label}: {'killed' if killed else 'SURVIVED'}")
            failures += 0 if killed else 1
    finally:
        with open(SOURCE, "w", encoding="utf-8") as out:
            out.write(original)  # a fresh mtime: make rebuilds from the restored source
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

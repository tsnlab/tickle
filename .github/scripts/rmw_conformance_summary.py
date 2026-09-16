#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Turn test_rmw_implementation's own per-executable JUnit/gtest .xml output into one Markdown
table of rmw_tickle's conformance results, written to GITHUB_STEP_SUMMARY by check-all.yml's own
"Run the upstream rmw conformance suite" step - same treatment rmw-perf.yml's own
rmw_perf_summary.py already gives the performance numbers, so a run's Actions summary page shows
pass/skip/fail counts directly instead of only the raw `colcon test-result --verbose` console log.

Only `*__rmw_tickle.gtest.xml` files are read - this same workspace's own test_rmw_implementation
build also produces one `*__rmw_fastrtps_cpp.gtest.xml` per test via the identical
call_for_each_rmw_implementation() auto-discovery (see check-all.yml's own comment on this), and
re-summarizing FastDDS's already-covered-elsewhere conformance here isn't this step's concern.
"""

import glob
import os
import sys
import xml.etree.ElementTree as ET

FILENAME_SUFFIX = "__rmw_tickle.gtest.xml"


def load_rows(results_dir):
    rows = []
    pattern = os.path.join(results_dir, "**", f"*{FILENAME_SUFFIX}")
    for path in sorted(glob.glob(pattern, recursive=True)):
        name = os.path.basename(path)[: -len(FILENAME_SUFFIX)]
        root = ET.parse(path).getroot()
        tests = int(root.get("tests", 0))
        failures = int(root.get("failures", 0))
        errors = int(root.get("errors", 0))
        # <testsuites> itself carries no "skipped" total (only each inner <testsuite> does) -
        # sum those instead of re-deriving it from individual <testcase result="skipped"> tags.
        skipped = sum(int(suite.get("skipped", 0)) for suite in root.findall("testsuite"))
        passed = tests - failures - errors - skipped
        rows.append(
            {
                "name": name,
                "tests": tests,
                "passed": passed,
                "skipped": skipped,
                "failed": failures + errors,
                "time": float(root.get("time", 0.0)),
            }
        )
    return rows


def render_markdown(rows):
    if not rows:
        return (
            "No `test_rmw_implementation` results found for `rmw_tickle` - see the workflow's "
            "own \"Run the upstream rmw conformance suite\" step output for what actually ran.\n"
        )

    rows = sorted(rows, key=lambda r: r["name"])
    total_tests = sum(r["tests"] for r in rows)
    total_passed = sum(r["passed"] for r in rows)
    total_skipped = sum(r["skipped"] for r in rows)
    total_failed = sum(r["failed"] for r in rows)

    lines = [
        "| Test | Passed | Skipped | Failed | Time (s) |",
        "|---|---:|---:|---:|---:|",
    ]
    for r in rows:
        status = "❌" if r["failed"] else ("⏭️" if r["skipped"] == r["tests"] else "✅")
        lines.append(
            "| {status} `{name}` | {passed} | {skipped} | {failed} | {time:.3f} |".format(
                status=status, **r
            )
        )
    lines.append(
        "| **Total** | **{}** | **{}** | **{}** | |".format(
            total_passed, total_skipped, total_failed
        )
    )
    lines.append("")
    lines.append(
        f"{len(rows)} `test_rmw_implementation` executables ran against `rmw_tickle` "
        f"({total_tests} test cases total). See `rmw_tickle/PLAN.md`'s Milestone 15 for which "
        "skips are confirmed-legitimate documented gaps versus what would be a real regression."
    )
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <test_results_dir>", file=sys.stderr)
        return 1
    rows = load_rows(sys.argv[1])
    print(render_markdown(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())

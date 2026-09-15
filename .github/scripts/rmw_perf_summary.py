#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Turn buildfarm_perf_tests' own per-test .benchmark.json output into one Markdown table of
rmw_tickle's own numbers, written to GITHUB_STEP_SUMMARY by rmw-perf.yml so every run's own
Actions summary page shows the numbers directly - no separate dashboard needed for this first
pass (see rmw_tickle/PLAN.md's own benchmark plan on why raw artifacts, not a dashboard, were the
deliberate starting point). rmw-perf.yml's own PERF_TEST_RMW_IMPLEMENTATIONS is rmw_tickle-only -
this script never compares against rmw_fastrtps_cpp/rmw_cyclonedds_cpp; buildfarm_perf_tests'
"two-process" test shape always runs both sides as local child processes on one host (no remote-
host launch capability at all), which structurally favors any rmw with a same-host shared-memory
transport - not a fair comparison, so this rig tracks rmw_tickle's own before/after regressions
only. The `rmw` column stays in the output/parsing below purely because the filename already
carries it and dropping it would only lose information.

Each file is named performance_test_two_process_results_<rmw>_<sync>_<topic>.benchmark.json (see
buildfarm_perf_tests' own test/add_performance_tests.cmake) - the rmw/sync/topic triple has to be
parsed from the *filename*, not the JSON body, since the body's own single inner key (e.g.
"rmw_tickle_Array1k") never includes the sync mode at all.
"""

import glob
import json
import os
import re
import sys

FILENAME_RE = re.compile(
    r"^performance_test_two_process_results_(?P<rmw>rmw_\w+)_(?P<sync>async|sync)_(?P<topic>\w+)\.benchmark\.json$"
)


def load_rows(results_dir):
    rows = []
    for path in sorted(glob.glob(os.path.join(results_dir, "performance_test_two_process_results_*.benchmark.json"))):
        match = FILENAME_RE.match(os.path.basename(path))
        if not match:
            continue  # a single-process/spinning/cross-vendor result, or an unrecognized name - not this table's scope
        with open(path) as f:
            data = json.load(f)
        # One inner key per file (e.g. {"...performance_two_process": {"rmw_tickle_Array1k": {...}}}) -
        # its own name is redundant with the filename we already parsed, so just take whichever key is there.
        (group,) = data.values()
        (result,) = group.values()
        rows.append(
            {
                "topic": match.group("topic"),
                "sync": match.group("sync"),
                "rmw": match.group("rmw"),
                "latency_ms": result["average_single_trip_time"]["dblValue"],
                "throughput_mbit_s": result["throughput"]["dblValue"],
                "received": result["received_messages"]["dblValue"],
                "lost": result["lost_messages"]["intValue"],
            }
        )
    return rows


def render_markdown(rows):
    if not rows:
        return (
            "No `buildfarm_perf_tests` two-process results found - see the workflow's own "
            "\"Run the benchmark\" step output for what actually ran.\n"
        )

    rows = sorted(rows, key=lambda r: (r["topic"], r["sync"], r["rmw"]))
    lines = [
        "| Topic | Sync | rmw implementation | Latency (ms) | Throughput (Mbit/s) | Received (/ ~1000 sent) | Lost |",
        "|---|---|---|---:|---:|---:|---:|",
    ]
    for r in rows:
        lines.append(
            "| {topic} | {sync} | `{rmw}` | {latency_ms:.4f} | {throughput_mbit_s:.4f} | "
            "{received:.0f} | {lost:.0f} |".format(**r)
        )
    lines.append("")
    lines.append(
        "Same-host, rmw_tickle-only run - not a cross-vendor comparison and not a real-target-"
        "network-medium measurement. See `rmw_tickle/PLAN.md`'s benchmark plan for why this is "
        "useful for tracking rmw_tickle's own before/after regressions, not as an authoritative "
        "absolute latency figure or a fair fight against rmw_fastrtps_cpp/rmw_cyclonedds_cpp "
        "(which auto-negotiate a same-host shared-memory transport this same-host rig would "
        "hand them for free)."
    )
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <results_dir>", file=sys.stderr)
        return 1
    rows = load_rows(sys.argv[1])
    print(render_markdown(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render `make coverage`'s gcovr summary.json as a Markdown table for $GITHUB_STEP_SUMMARY.

Tracking, not enforcement: this prints the figures and never fails on their value. It does fail when the
summary is missing or empty, so "no report" cannot read as "a report with nothing in it".
"""

import json
import sys


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "platform/linux/obj/coverage/report/summary.json"
    try:
        with open(path, encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError) as e:
        print(f"coverage_summary: cannot read {path}: {e}", file=sys.stderr)
        return 1
    if not s.get("files") or not s.get("line_total"):
        print(f"coverage_summary: {path} lists no files - the unit tests produced no coverage data", file=sys.stderr)
        return 1

    def pct(covered: int, total: int) -> str:
        return f"{100.0 * covered / total:.1f}%" if total else "n/a"

    print("### Unit-test coverage of core (tracked, not enforced)")
    print()
    print("`make coverage`: `make test` built with `--coverage`, read by gcovr. HALs excluded (the unit tests link")
    print("the mock HAL). Per-line HTML and Cobertura XML are in the `coverage-report` artifact.")
    print()
    print("| File | Lines | Branches |")
    print("|---|---:|---:|")
    for f in sorted(s["files"], key=lambda f: f["filename"]):
        print(
            f"| `{f['filename']}` | {pct(f['line_covered'], f['line_total'])} ({f['line_covered']}/{f['line_total']}) "
            f"| {pct(f['branch_covered'], f['branch_total'])} ({f['branch_covered']}/{f['branch_total']}) |"
        )
    print(
        f"| **Total** | **{pct(s['line_covered'], s['line_total'])}** ({s['line_covered']}/{s['line_total']}) "
        f"| **{pct(s['branch_covered'], s['branch_total'])}** ({s['branch_covered']}/{s['branch_total']}) |"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Mutants for the call identity and the call retry bounds (2026-10-09).

Each mutant reverts one part of the fix in src/tickle.c and must make its test fail:
  per_client_seq   seq_no counted per Client again (two Clients calling in turn get the same seq_no)
  first_match      the answer goes to find_endpoint()'s first Client of the service again
  stamp_after_send the round trip timed from when the send returns
  ceiling_of_srtt  a wait's ceiling of srtt alone, below G for a same-host srtt
A control run of the unmutated source must pass both tests first, so a test that fails for another reason cannot
kill a mutant. src/tickle.c is restored, with a fresh mtime so make rebuilds, whatever happens.

Usage: tests/mutants_call_identity.py   (from the repository root)
"""

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "src", "tickle.c")
LINUX = os.path.join(REPO, "platform", "linux")
TESTS = ["test_two_clients_one_service", "test_call_retry_adaptive"]

MUTANTS = [
    ("per_client_seq", "test_two_clients_one_service",
     "    callrequest_header->seq_no = node->call_seq_no; //",
     "    callrequest_header->seq_no = (uint16_t)(node->call_seq_no / 2); //"),
    ("first_match", "test_two_clients_one_service",
     "            if (outstanding != NULL && outstanding->seq_no == seq_no) {\n                return client;\n"
     "            }\n",
     "            return (outstanding != NULL && outstanding->seq_no == seq_no) ? client : NULL;\n"),
    ("stamp_after_send", "test_call_retry_adaptive",
     "    client->cache_time = sent_at;\n",
     "    client->cache_time = tt_get_ns();\n"),
    ("ceiling_of_srtt", "test_call_retry_adaptive",
     "    uint64_t unit = srtt > granularity ? srtt : granularity;\n",
     "    uint64_t unit = srtt;\n"),
]


def run_test(name):
    """Builds and runs one test binary; True if it passed."""
    target = os.path.join("obj", "debug", "tests", name)
    build = subprocess.run(["make", "-s", target], cwd=LINUX, capture_output=True, text=True, check=False)
    if build.returncode != 0:
        print(f"  build of {name} failed:\n{build.stdout[-2000:]}{build.stderr[-2000:]}")
        return False
    run = subprocess.run([os.path.join(LINUX, target)], cwd=LINUX, capture_output=True, text=True, check=False)
    return run.returncode == 0


def main():
    original = open(SOURCE, encoding="utf-8").read()
    failures = 0
    try:
        for name in TESTS:
            if not run_test(name):
                print(f"CONTROL FAILED: {name} fails on the unmutated source - no mutant can be judged")
                return 2
        print("control: both tests pass on the unmutated source")
        for label, test, before, after in MUTANTS:
            if original.count(before) != 1:
                print(f"MUTANT {label}: its text occurs {original.count(before)} times, not once - not applied")
                failures += 1
                continue
            with open(SOURCE, "w", encoding="utf-8") as out:
                out.write(original.replace(before, after, 1))
            killed = not run_test(test)
            print(f"mutant {label}: {'killed' if killed else 'SURVIVED'} by {test}")
            failures += 0 if killed else 1
    finally:
        with open(SOURCE, "w", encoding="utf-8") as out:
            out.write(original)  # a fresh mtime: make rebuilds from the restored source
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

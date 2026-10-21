#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Mutants for a kept answer's lazy expiry (2026-10-21, tests/test_kept_answer_lazy_expiry.c).

Each mutant removes one part of the change in src/tickle.c and must make one of TESTS fail:
  never_expires          server_cache_live() ignores the expiry time: a stale answer is served past the horizon
  expired_not_released   an expired answer is reported dead but left in cache[]: it is never learnt from, and
                         counts as live where cache[] is read after the check
  hit_not_rearmed        a retry served from the cache no longer extends the answer's expiry
  new_answer_unarmed     a new answer's expiry is not set: it is dead (expires_at 0) as soon as it is kept
  pick_ignores_expiry    set_server_cache() takes every held slot as live: an expired slot is not reused, and a live
                         answer is evicted in its place
  find_ignores_expiry    the lookup serves a held answer without checking its expiry
A control run of the unmutated source must pass first, so a test that fails for another reason cannot kill a mutant.
src/tickle.c is restored, with a fresh mtime so make rebuilds, whatever happens.

Usage: tests/mutants_kept_answer_lazy_expiry.py   (from the repository root)
"""

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "src", "tickle.c")
LINUX = os.path.join(REPO, "platform", "linux")
TESTS = ["test_kept_answer_lazy_expiry", "test_service_retry_horizon", "test_process_callrequest", "test_server_cache"]

MUTANTS = [
    ("never_expires",
     "    if (now < server->cache_expires_at[slot]) {\n",
     "    if (now < server->cache_expires_at[slot] || true) {\n"),
    ("expired_not_released",
     "    server->cache[slot] = NULL; // expired\n",
     "\n"),
    ("hit_not_rearmed",
     "    server->cache_sent_at[slot] = now;\n    set_server_cache_expiry(server, slot, now);\n",
     "    server->cache_sent_at[slot] = now;\n"),
    ("new_answer_unarmed",
     "    set_server_cache_expiry(server, free_slot, now);\n",
     "\n"),
    ("pick_ignores_expiry",
     "        if (!server_cache_live(server, i, now)) {\n            if (server->cache_sent_at[i] == 0) {\n",
     "        if (server->cache[i] == NULL) {\n            if (server->cache_sent_at[i] == 0) {\n"),
    ("find_ignores_expiry",
     "        if (server->cache[i] != NULL && server_cache_slot_names(server, i, receiver, client_tag, seq_no) &&\n"
     "            server_cache_live(server, i, tt_get_ns())) {\n",
     "        if (server->cache[i] != NULL && server_cache_slot_names(server, i, receiver, client_tag, seq_no)) {\n"),
]


def run_tests():
    """Builds and runs every test binary; the names of those that failed (a failed build fails them all)."""
    targets = [os.path.join("obj", "debug", "tests", test) for test in TESTS]
    build = subprocess.run(["make", "-s"] + targets, cwd=LINUX, capture_output=True, text=True, check=False)
    if build.returncode != 0:
        print(f"  build failed:\n{build.stdout[-2000:]}{build.stderr[-2000:]}")
        return list(TESTS)
    failed = []
    for target, test in zip(targets, TESTS):
        run = subprocess.run([os.path.join(LINUX, target)], cwd=LINUX, capture_output=True, text=True, check=False,
                             timeout=600)
        if run.returncode != 0:
            failed.append(test)
    return failed


def main():
    original = open(SOURCE, encoding="utf-8").read()
    failures = 0
    try:
        failed = run_tests()
        if failed:
            print(f"CONTROL FAILED: {', '.join(failed)} fail on the unmutated source - no mutant can be judged")
            return 2
        print("control: every test passes on the unmutated source")
        for label, before, after in MUTANTS:
            if original.count(before) != 1:
                print(f"MUTANT {label}: its text occurs {original.count(before)} times, not once - not applied")
                failures += 1
                continue
            with open(SOURCE, "w", encoding="utf-8") as out:
                out.write(original.replace(before, after, 1))
            failed = run_tests()
            print(f"mutant {label}: {'killed by ' + ', '.join(failed) if failed else 'SURVIVED'}")
            failures += 0 if failed else 1
    finally:
        with open(SOURCE, "w", encoding="utf-8") as out:
            out.write(original)  # a fresh mtime: make rebuilds from the restored source
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Mutants for several calls outstanding per Client (2026-10-16, DESIGN.md section 7).

Each mutant removes one part of the change in src/tickle.c and must make its test fail:
  one_slot              a Client uses its first call slot only, so a second call is refused
  answer_unnamed        an answer's callback does not say which call it reports (callback_seq_no)
  timeout_unnamed       a timeout's callback does not say which call timed out
  first_slot_lookup     an answer is matched against each Client's first call slot only
  backoff_per_call      every call lost together doubles the estimate again
  ignored_answered      tt_CALL_IGNORED is encoded and sent as an answer with that code
  shares_overlap        attached storage is not split: every call keeps its request at the start of the area
  answers_replaced      the server replaces a Client's previous answer, as the one-call build does
  no_answer_bound       a Client's answers are not bounded (tt_CLIENT_CALL_WINDOW never reached)
  full_takes_oldest     a full cache takes the oldest answer overall, even from a Client at full depth
  window_open           a Client calls on while an outstanding call is tt_CLIENT_CALL_WINDOW calls old
  bound_by_sent_time    past the bound, a Client's answer sent longest ago goes, not the one to its oldest call
  duplicate_kept        a late duplicate's answer to a call older than all kept pushes out a live one
  newest_is_new         calls are aged from the new answer's seq_no, not from the Client's newest call
A control run of the unmutated source must pass every test first, so a test that fails for another reason cannot
kill a mutant. src/tickle.c is restored, with a fresh mtime so make rebuilds, whatever happens.

Usage: tests/mutants_multi_call.py   (from the repository root)
"""

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "src", "tickle.c")
LINUX = os.path.join(REPO, "platform", "linux")
TESTS = ["test_client_multi_call", "test_server_cache", "test_two_clients_one_service"]

MUTANTS = [
    ("one_slot", "test_client_multi_call",
     "        if (client->calls[i].cache == NULL) {\n            slot = i;\n",
     "        if (client->calls[i].cache == NULL && i == 0) {\n            slot = i;\n"),
    ("answer_unnamed", "test_client_multi_call",
     "    client->callback_seq_no = seq_no;\n    client->callback(client, callresponse_header->return_code, response);\n",
     "    client->callback(client, callresponse_header->return_code, response);\n"),
    ("timeout_unnamed", "test_client_multi_call",
     "    call->cache = NULL;\n    client->callback_seq_no = seq_no;\n    client->callback(client, tt_CALL_TIMEOUT, NULL);\n",
     "    call->cache = NULL;\n    client->callback(client, tt_CALL_TIMEOUT, NULL);\n"),
    ("first_slot_lookup", "test_client_multi_call",
     "            for (int i = 0; i < tt_CLIENT_MAX_OUTSTANDING; i++) {\n"
     "                const struct tt_SubmessageHeader* cache = client->calls[i].cache;\n",
     "            for (int i = 0; i < 1; i++) {\n"
     "                const struct tt_SubmessageHeader* cache = client->calls[i].cache;\n"),
    ("backoff_per_call", "test_client_multi_call",
     "    if (call->cache_time < client->latency_backed_off_at) {\n",
     "    if (call->cache_time < client->latency_backed_off_at && false) {\n"),
    ("ignored_answered", "test_client_multi_call",
     "        if (return_code == tt_CALL_IGNORED) {\n",
     "        if (return_code == tt_CALL_IGNORED && false) {\n"),
    ("shares_overlap", "test_client_multi_call",
     "    return client->cache_storage + ((size_t)slot * share);\n",
     "    return client->cache_storage;\n"),
    ("answers_replaced", "test_client_multi_call",
     "    if (tt_CLIENT_MAX_OUTSTANDING == 1) {\n        for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {\n",
     "    if (tt_CLIENT_MAX_OUTSTANDING >= 1) {\n        for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {\n"),
    ("no_answer_bound", "test_server_cache",
     "    if (pick.own_live >= tt_CLIENT_CALL_WINDOW) { // this Client's bound\n",
     "    if (pick.own_live >= tt_MAX_SERVER_CACHE_COUNT + 1) { // this Client's bound\n"),
    ("window_open", "test_client_multi_call",
     "    if (slot < 0 || client_window_closed(client)) {\n",
     "    if (slot < 0) {\n"),
    ("bound_by_sent_time", "test_server_cache",
     "        if (pick->own_oldest < 0 || age > pick->own_oldest_age) {\n",
     "        if (pick->own_oldest < 0 || server->cache_sent_at[i] < server->cache_sent_at[pick->own_oldest]) {\n"),
    ("duplicate_kept", "test_server_cache",
     "        if (pick.new_age >= pick.own_oldest_age) {\n",
     "        if (pick.new_age >= pick.own_oldest_age && false) {\n"),
    ("newest_is_new", "test_server_cache",
     "            newest_ahead = ahead > newest_ahead ? ahead : newest_ahead;\n",
     "            newest_ahead = ahead > newest_ahead ? 0 : newest_ahead;\n"),
    ("full_takes_oldest", "test_server_cache",
     "        free_slot = pick.own_live >= tt_CLIENT_MAX_OUTSTANDING ? pick.own_oldest : pick.oldest_live;\n",
     "        free_slot = pick.oldest_live;\n"),
]


def run_test(name):
    """Builds and runs one test binary; True if it passed."""
    target = os.path.join("obj", "debug", "tests", name)
    build = subprocess.run(["make", "-s", target], cwd=LINUX, capture_output=True, text=True, check=False)
    if build.returncode != 0:
        print(f"  build of {name} failed:\n{build.stdout[-2000:]}{build.stderr[-2000:]}")
        return False
    run = subprocess.run([os.path.join(LINUX, target)], cwd=LINUX, capture_output=True, text=True, check=False,
                         timeout=600)
    return run.returncode == 0


def main():
    original = open(SOURCE, encoding="utf-8").read()
    failures = 0
    try:
        for name in TESTS:
            if not run_test(name):
                print(f"CONTROL FAILED: {name} fails on the unmutated source - no mutant can be judged")
                return 2
        print("control: every test passes on the unmutated source")
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

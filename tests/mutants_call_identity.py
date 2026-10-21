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
  tag_not_sent     every CallRequest carries client_tag 0, so the server keeps one answer per context again
  answer_per_source  a new answer replaces any answer for its context, not only its own Client's
  lookup_without_tag  the cache answers (context, seq_no) whichever Client asks
  seq_no_shared_while_outstanding  a Client may take a seq_no another Client of the service still waits on
  first_match      the answer goes to find_endpoint()'s first Client of the service again
  stamp_after_send the round trip timed from when the send returns
  ceiling_of_srtt  a call wait's ceiling of srtt alone, below G for a same-host srtt (call_retry_bounds(); the same
                   line in retry_interval_of(), the reliable repair's, is not this mutant)
A control run of the unmutated source must pass both tests first, so a test that fails for another reason cannot
kill a mutant.

Refuses to start (exit 2) if any anchor is not in src/tickle.c exactly once - ceiling_of_srtt's matched twice from
2026-10-16, when retry_interval_of() took the same line, and the sweep ran on reporting it as a failure - or if
src/tickle.c is already modified. Every child is bounded (tests/mutant_guard.py: 2 GB of address space, a deadline,
at most 256 KB of its output kept); src/tickle.c is restored after each mutant, on an exception and on
SIGINT/SIGTERM/SIGHUP, with a fresh mtime so make rebuilds.

A mutant whose test binary, debug sections aside, is the control's (tests/mutant_digest.py) is reported DEAD
(NOT_COMPILED) and counted as a failure: it edited code that binary's build leaves out. MUTANTS=<substring> runs only
the mutants whose label contains it.

Usage: tests/mutants_call_identity.py   (from the repository root)
"""

import os
import sys

from mutant_digest import NOT_COMPILED, code_digest, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_REL = "src/tickle.c"
SOURCE = os.path.join(REPO, SOURCE_REL)
LINUX = os.path.join(REPO, "platform", "linux")
# A name with a directory is that build's binary: tests-one-call/ is built with tt_CLIENT_MAX_OUTSTANDING 1, where the
# server still replaces a Client's previous answer (since 2026-10-16 only there).
TESTS = ["test_two_clients_one_service", "test_call_retry_adaptive", "tests-one-call/test_two_clients_one_service"]

MUTANTS = [
    ("per_client_seq", "test_two_clients_one_service",
     "    uint16_t seq_no = node->call_seq_no;\n",
     "    uint16_t seq_no = (uint16_t)(node->call_seq_no / 2);\n"),
    ("tag_not_sent", "test_two_clients_one_service",
     "    callrequest_header->client_tag = client->client_tag;",
     "    callrequest_header->client_tag = 0;"),
    ("answer_per_source", "tests-one-call/test_two_clients_one_service",
     # set_server_cache()'s one-call replacement; server_cache_live() replaced the NULL test in 6133f98a.
     "            if (server_cache_live(server, i, now) && server->cache[i]->receiver == receiver &&\n"
     "                server->cache_client_tag[i] == client_tag) {\n",
     "            if (server_cache_live(server, i, now) && server->cache[i]->receiver == receiver) {\n"),
    ("lookup_without_tag", "test_two_clients_one_service",
     "    return submessage_header->receiver == receiver && server->cache_client_tag[slot] == client_tag &&\n",
     "    return submessage_header->receiver == receiver &&\n"),
    ("seq_no_shared_while_outstanding", "test_two_clients_one_service",
     "    while (find_calling_call(node, endpoint->id, seq_no) != NULL) {\n",
     "    while (find_calling_call(node, endpoint->id, seq_no) != NULL &&\n"
     "           find_calling_call(node, endpoint->id, seq_no)->client == client) {\n"),
    ("first_match", "test_two_clients_one_service",
     "                    return &client->calls[i];\n                }\n            }\n",
     "                    return &client->calls[i];\n                }\n            }\n            return NULL;\n"),
    ("stamp_after_send", "test_call_retry_adaptive",
     "    call->cache_time = sent_at;\n",
     "    call->cache_time = tt_get_ns();\n"),
    ("ceiling_of_srtt", "test_call_retry_adaptive",
     # The multiple is the service's since 30a33823 (tt_ServiceOptions, bundle 4), no longer the compiled constant.
     "    uint64_t unit = srtt > granularity ? srtt : granularity;\n"
     "    *ceiling = unit * service_call_retry_max_srtt_multiple(client->node);\n",
     "    uint64_t unit = srtt;\n    *ceiling = unit * service_call_retry_max_srtt_multiple(client->node);\n"),
]


def binary_of(name):
    return os.path.join(LINUX, "obj", "debug", name if "/" in name else os.path.join("tests", name))


def run_test(name, control=None):
    """Builds and runs one test binary: PASS, FAIL, HANG, BUILD_FAILED, or NOT_COMPILED if it is the control's."""
    target = os.path.relpath(binary_of(name), LINUX)
    build = run_capped(["make", "-s", "-j2", target], timeout=900, cwd=LINUX)
    if build.returncode != 0:
        # Not a kill: a mutant that does not compile is wrongly written, and says nothing about the test.
        print(f"  build of {name} failed:\n{build.output[-2000:]}")
        return "BUILD_FAILED"
    if unchanged(control, binary_of(name)):
        return NOT_COMPILED
    run = run_capped([binary_of(name)], timeout=300, cwd=LINUX)
    LAST_OUTPUT[0] = run.output
    if run.timed_out:
        return "HANG"  # a kill too: the test did not pass
    return "PASS" if run.returncode == 0 else "FAIL"


LAST_OUTPUT = [""]  # the last test run's kept output, for killed_by()


def main():
    be_nice()
    check_anchors(REPO, [(label, SOURCE_REL, before, after) for label, _test, before, after in MUTANTS])
    only = os.environ.get("MUTANTS", "")
    failures = 0
    digests = {}
    with SourceGuard(REPO, [SOURCE_REL]) as guard:
        for name in TESTS:
            if run_test(name) != "PASS":
                print(f"CONTROL FAILED: {name} fails on the unmutated source - no mutant can be judged")
                return 2
            # Each binary keeps its own control: tests-one-call/ is built with other defines than tests/.
            digests[name] = code_digest(binary_of(name))
        print("control: every test passes on the unmutated source", flush=True)
        original = guard.original(SOURCE_REL)
        for label, test, before, after in MUTANTS:
            if only and only not in label:
                continue
            with open(SOURCE, "w", encoding="utf-8") as out:
                out.write(original.replace(before, after, 1))
            try:
                verdict = run_test(test, digests[test])
            finally:
                guard.restore()
            killed = verdict in ("FAIL", "HANG")
            outcome = "killed" if killed else ("DEAD (NOT_COMPILED)" if verdict == NOT_COMPILED else "SURVIVED")
            print(f"mutant {label}: {outcome} by {test} ({verdict})", flush=True)
            if killed:
                print(f"  killed by: {', '.join(killed_by(LAST_OUTPUT[0], REPO)) or '(no failing check printed)'}")
            failures += 0 if killed else 1
    print(usage_line())
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

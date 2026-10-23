#!/usr/bin/env python3
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Mutants for a client's launch told apart without a wire change (2026-10-23, ROADMAP 5a).

Each mutant reverts one part of the change in src/tickle.c and must make its test fail:
  stale_ignores_port     server_cache_slot_is_stale() without the port test: a new launch asking with its
                         predecessor's seq_no is answered from the predecessor's kept response
  own_ignores_port       server_cache_slot_is_own() without it: a predecessor's answers count as the new launch's own,
                         and its first answer is sent and not kept
  port_not_kept          set_server_cache() keeps every answer with port 0 (unknown), so no port ever differs
  deferred_port_dropped  a deferred answer is kept with port 0, not its request's
  learn_without_port     learn_from_expired_response() not handed the port: a new launch's retry teaches a gap
  zero_is_a_port         a port of 0 (unknown) read as a port that differs
  equal_ports_differ     the port test fires on equal ports: a retry of one launch re-runs its callback
  seq_start_zero         every launch's call seq_no starts at 0 again
A control run of the unmutated source must pass every test first, so a test that fails for another reason cannot
kill a mutant.

Refuses to start (exit 2) if any anchor is not in src/tickle.c exactly once, or if src/tickle.c is already modified.
Every child is bounded (tests/mutant_guard.py: 2 GB of address space, a deadline, at most 256 KB of its output kept);
src/tickle.c is restored after each mutant, on an exception and on SIGINT/SIGTERM/SIGHUP, with a fresh mtime so make
rebuilds. A mutant whose test binary, debug sections aside, is the control's (tests/mutant_digest.py) is reported DEAD
(NOT_COMPILED) and counted as a failure. MUTANTS=<substring> runs only the mutants whose label contains it.

Usage: tests/mutants_client_incarnation.py   (from the repository root)
"""

import os
import sys

from mutant_digest import NOT_COMPILED, code_digest, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_REL = "src/tickle.c"
SOURCE = os.path.join(REPO, SOURCE_REL)
LINUX = os.path.join(REPO, "platform", "linux")
TESTS = ["test_process_callrequest", "test_client_incarnation"]

MUTANTS = [
    ("stale_ignores_port", "test_process_callrequest",
     "    if (server_port_differs(server->cache_client_port[slot], sender_port)) {\n"
     "        return true;\n"
     "    }\n"
     "    uint32_t cached_for = server->cache_client_entity[slot];\n",
     "    uint32_t cached_for = server->cache_client_entity[slot];\n"),
    ("own_ignores_port", "test_process_callrequest",
     "           server->cache_client_tag[slot] == client_tag &&\n"
     "           !server_port_differs(server->cache_client_port[slot], sender_port);\n",
     "           server->cache_client_tag[slot] == client_tag;\n"),
    ("port_not_kept", "test_client_incarnation",
     "    server->cache_client_port[free_slot] = sender_port;\n",
     "    server->cache_client_port[free_slot] = 0;\n"),
    ("deferred_port_dropped", "test_process_callrequest",
     "        encode_call_response(node, request_id.receiver, server->pending_client_tag[slot], sender_port, server,\n",
     "        encode_call_response(node, request_id.receiver, server->pending_client_tag[slot], 0, server,\n"),
    ("learn_without_port", "test_process_callrequest",
     "        learn_from_expired_response(server, header->source, client_tag, seq_no, sender_port);\n",
     "        learn_from_expired_response(server, header->source, client_tag, seq_no, 0);\n"),
    ("zero_is_a_port", "test_process_callrequest",
     "    return cached != asking && cached != 0 && asking != 0;\n",
     "    return cached != asking;\n"),
    ("equal_ports_differ", "test_process_callrequest",
     "    return cached != asking && cached != 0 && asking != 0;\n",
     "    return cached != 0 && asking != 0;\n"),
    ("seq_start_zero", "test_client_incarnation",
     "    node->call_seq_no = (uint16_t)(node->entity_id_base ^ (node->entity_id_base >> 16));\n",
     "    node->call_seq_no = 0;\n"),
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

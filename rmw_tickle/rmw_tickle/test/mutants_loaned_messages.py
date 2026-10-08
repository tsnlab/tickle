#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""Mutant sweep for rmw_tickle's loaned messages (docs/RMW.md; test/test_loaned_messages.c).

Each entry bends one decision of the loan code and names the case of test_loaned_messages that must go red for it.
A mutant that survives is a decision no test makes. The unmutated source runs first and must pass: a sweep whose
control fails says nothing about the mutants.

    mutants_loaned_messages.py '<command>'

<command> rebuilds rmw_tickle from this checkout and runs test_loaned_messages verbosely in a private network
namespace, exiting non-zero when it fails (for instance a workspace script's `rmwonly` then `test
test_loaned_messages -V`). A mutant is killed when the command fails AND its output names the expected case - the
line the test prints for every case that passed stops before it - so a mutant killed by something else does not count
for the decision it names. Every file is restored whatever happens, with a fresh mtime, so the next build recompiles it.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
SUB = "rmw_tickle/rmw_tickle/src/rmw_subscription.c"
PUB = "rmw_tickle/rmw_tickle/src/rmw_publisher.c"

# (what the mutant removes, file, original, replacement, the case whose line must be missing from the output)
MUTANTS = [
    (
        "ring slots are lent only when asked for",
        SUB,
        "    if (!sub_impl->loan_ring_slots && !in_receive_buffer(sub_impl->node->context_impl, view->payload)) {",
        "    if (false) {",
        "ring: ok",
    ),
    (
        "a loan is read in place only where its type is aligned",
        SUB,
        "    if (((uintptr_t)message & (callbacks->ros_struct_align - 1U)) != 0) {\n        return false;",
        "    if (false) {\n        return false;",
        "udp: ok",
    ),
    (
        "a returned loan gives its sample back to core",
        SUB,
        "    pthread_mutex_unlock(&sub_impl->queue_mutex);\n    release_sample(sub_impl, &loan.sample);\n    return RMW_RET_OK;",
        "    pthread_mutex_unlock(&sub_impl->queue_mutex);\n    return RMW_RET_OK;",
        "pinned: ok",
    ),
    (
        "a dequeued sample leaves the per-subscription count",
        SUB,
        "        if (NULL != entry->in_place) {\n            sub_impl->retained_queued--;\n        }",
        "",
        "pinned: ok",
    ),
    (
        "only the type's exact in-place size is read in place",
        SUB,
        "    if (0 == header || view->length - header != callbacks->inplace_bytes) {",
        "    if (0 == header) {",
        None,  # nothing on the wire of this test is any other size: expected to SURVIVE, listed so it is said
    ),
    (
        "a kept publisher buffer is lent again rather than a new one made",
        PUB,
        "        if (!pub_impl->loan_out[i]) {\n            pub_impl->loan_out[i] = true;",
        "        if (false) {\n            pub_impl->loan_out[i] = true;",
        "borrow: contract",
    ),
    (
        "a published loan goes back to the publisher",
        PUB,
        "    if (i < pub_impl->loan_count) {\n        pub_impl->loan_out[i] = false;\n    }\n    pthread_mutex_unlock(&pub_impl->loan_mutex);\n    return ret;",
        "    pthread_mutex_unlock(&pub_impl->loan_mutex);\n    return ret;",
        "borrow: contract",
    ),
    (
        "a returned shell goes back to the pool",
        SUB,
        "    if (NULL != loan.shell) {\n        shell_pool_push(sub_impl, loan.shell);\n    }\n    pthread_mutex_unlock",
        "    pthread_mutex_unlock",
        "take: contract",
    ),
]


def run(command):
    result = subprocess.run(command, shell=True, capture_output=True, text=True, check=False)
    return result.returncode, result.stdout + result.stderr


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    command = sys.argv[1]
    code, output = run(command)
    if code != 0 or "test_loaned_messages: PASS" not in output:
        print("CONTROL FAILED: the unmutated source does not pass; no mutant can be judged")
        print(output[-3000:])
        return 1
    print("control: PASS")
    survivors = 0
    for what, rel, old, new, case in MUTANTS:
        path = ROOT / rel
        original = path.read_text()
        if original.count(old) != 1:
            print(f"STALE   {what}: the original text is not in {rel} exactly once")
            survivors += 1
            continue
        try:
            path.write_text(original.replace(old, new))
            code, output = run(command)
        finally:
            path.write_text(original)  # a fresh mtime: the next build recompiles it
        failed = code != 0 or "test_loaned_messages: PASS" not in output
        if case is None:
            print(f"{'KILLED' if failed else 'SURVIVED (expected)'}  {what}")
            continue
        # The case's line, at the start of a line (ctest -V prefixes "NN: "): "pinned:" alone is inside "held-pinned:",
        # and "borrow:" inside the assertion text "check_borrow:".
        printed = re.search(r"^(\d+: )?" + re.escape(case), output, re.MULTILINE) is not None
        killed = failed and not printed
        assertion = next((line for line in output.splitlines() if "Assertion" in line), "")
        print(f"{'KILLED ' if killed else 'SURVIVED'} {what} (expects {case} to fail) {assertion.strip()[-160:]}")
        survivors += 0 if killed else 1
    print(f"mutants: {survivors} unexpected survivor(s)")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())

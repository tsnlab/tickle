#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""Mutant sweep for the same-host kill test (test/test_shm_kill.c): every check of it must be able to fail.

Each entry bends one thing - a decision in core, or (marked "by hand") the property itself in the test, where no
plausible core defect removes it - and names the verdict line that must then read FAIL. The unmutated source runs
first, over every arm the mutants use, and must pass: a sweep whose control fails says nothing about the mutants.

    mutants_shm_kill.py '<command>'

<command> rebuilds rmw_tickle from this checkout and runs test_shm_kill in a private network namespace with the
arguments in place of {args} (for instance `build.sh && run_in_netns.sh test_shm_kill {args}`). A mutant is killed when
its named verdict line reads FAIL - not when the run merely fails, so a mutant killed by something else does not count
for the check it names. Every file is restored whatever happens, with a fresh mtime, so the next build recompiles it.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
CORE = "src/tickle.c"
HAL = "src/hal_linux.c"
TEST = "rmw_tickle/rmw_tickle/test/test_shm_kill.c"
SLEEP_INCLUDE = (CORE, "#include <errno.h>\n", "#include <errno.h>\n#include <time.h>\n")
PAUSE = "{ struct timespec pause_ = {0, 400000000L}; nanosleep(&pause_, NULL); }"
# A hang, not a hiccup: PAUSE (400 ms) here held the context's polling thread but showed in the subscriber's own polls
# as a 200 ms gap, under R1's bound - R1 watches the application's calls, which wait on that thread only in part.
HANG = "{ struct timespec hang_ = {1, 0L}; nanosleep(&hang_, NULL); }"

# (what is bent, [(file, original, replacement)], test arguments, the verdict line that must read FAIL)
MUTANTS = [
    (
        "a dead peer's ring is handed back when it is presumed dead",
        [(CORE, "    forget_same_host_peer(node, source);\n    release_peer_segment(node, source);\n",
          "    forget_same_host_peer(node, source);\n")],
        "sub slow kill",
        "sub-kill 10Hz K2",
    ),
    (
        "a dead context's files are swept when a segment is built",
        [(HAL, "    sweep_orphaned_segments(path);\n", "")],
        "sub fast kill",
        "sub-kill 500Hz K4",
    ),
    (
        "the same, for a killed publisher's own segment",
        [(HAL, "    sweep_orphaned_segments(path);\n", "")],
        "pub fast kill",
        "pub-kill 500Hz R4",
    ),
    (
        "a full ring drops rather than waits (a publisher that waits on a dead reader)",
        [SLEEP_INCLUDE,
         (CORE, "        node->segment_full_dropped++;\n#if tt_SAMPLE_LENDING\n        if (segment_full_by_hold(segment)) {",
          "        node->segment_full_dropped++;\n        " + PAUSE +
          "\n#if tt_SAMPLE_LENDING\n        if (segment_full_by_hold(segment)) {")],
        "sub fast kill",
        "sub-kill 500Hz K1",
    ),
    (
        "a silent peer is presumed dead",
        [(CORE, "            presume_node_dead(node, (uint8_t)i, time - last);", "            (void)last;")],
        "pub fast kill",
        "pub-kill 500Hz R2",
    ),
    (
        "handling a dead peer does not stall the context (a subscriber that hangs on a dead publisher)",
        [SLEEP_INCLUDE,
         (CORE, "    forget_same_host_peer(node, source);\n    release_peer_segment(node, source);\n",
          "    forget_same_host_peer(node, source);\n    release_peer_segment(node, source);\n    " + HANG + "\n")],
        "pub fast kill",
        "pub-kill 500Hz R1",
    ),
    (
        "by hand: the restarted subscriber is one the publisher matches",
        [(TEST, "    rmw_subscription_t* sub = rmw_create_subscription(node, &message_handle, TOPIC, &qos, &sub_options);",
          "    rmw_subscription_t* sub =\n        rmw_create_subscription(node, &message_handle, slot == 1 ? \"/shm_kill_elsewhere\" : TOPIC, &qos,"
          " &sub_options);")],
        "sub fast kill",
        "sub-kill 500Hz K3",
    ),
    (
        "by hand: the restarted publisher is one the subscriber matches",
        [(TEST, "    rmw_publisher_t* pub = rmw_create_publisher(node, &message_handle, TOPIC, &qos, &pub_options);",
          "    rmw_publisher_t* pub =\n        rmw_create_publisher(node, &message_handle, generation == 2 ? \"/shm_kill_elsewhere\" : TOPIC, &qos,"
          " &pub_options);")],
        "pub fast kill",
        "pub-kill 500Hz R3",
    ),
]


def run(command, args):
    result = subprocess.run(command.replace("{args}", args), shell=True, capture_output=True, text=True, check=False)
    return result.returncode, result.stdout + result.stderr


def verdict(output, line):
    """PASS, FAIL or None (not printed) for one verdict line, matched on its arm and check columns."""
    arm, check = line.rsplit(" ", 1)
    found = re.search(r"^" + re.escape(arm) + r" +" + re.escape(check) + r" +(PASS|FAIL)", output, re.MULTILINE)
    return found.group(1) if found else None


def main():
    if len(sys.argv) != 2 or "{args}" not in sys.argv[1]:
        print(__doc__)
        return 2
    command = sys.argv[1]
    control_args = sorted({args for _, _, args, _ in MUTANTS})
    for args in control_args:
        code, output = run(command, args)
        if code != 0 or "test_shm_kill: PASS" not in output:
            print(f"CONTROL FAILED ({args}): the unmutated source does not pass; no mutant can be judged")
            print(output[-3000:])
            return 1
        print(f"control ({args}): PASS")
    survivors = 0
    for what, edits, args, line in MUTANTS:
        originals = {}
        stale = False
        for rel, old, _ in edits:
            path = ROOT / rel
            text = originals.get(rel, path.read_text())
            originals.setdefault(rel, text)
            if text.count(old) != 1:
                stale = True
        if stale:
            print(f"STALE    {what}: an original text is not in its file exactly once")
            survivors += 1
            continue
        try:
            for rel, old, new in edits:
                path = ROOT / rel
                path.write_text(path.read_text().replace(old, new))
            code, output = run(command, args)
        finally:
            for rel, text in originals.items():
                (ROOT / rel).write_text(text)  # a fresh mtime: the next build recompiles it
        got = verdict(output, line)
        killed = got == "FAIL"
        shown = next((l for l in output.splitlines() if l.startswith(line.rsplit(" ", 1)[0]) and
                      f" {line.rsplit(' ', 1)[1]} " in l), "verdict line not printed")
        print(f"{'KILLED  ' if killed else 'SURVIVED'} {what}\n         {shown.strip()[:200]}")
        survivors += 0 if killed else 1
    print(f"mutants: {survivors} unexpected survivor(s)")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())

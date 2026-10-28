#!/usr/bin/env python3
"""Mutant sweep for the match point a VOLATILE reader is owed (tt_PeerAck.first_owed_seq_no, 2026-10-07; C8,
2026-10-29).

Each entry breaks one part of it and must turn tests/test_volatile_match_point.c red. A mutant that still passes means
the test does not test what it says, or the condition decides nothing and should not be in the code.

Not mutated, and why:
  - forget_peer_ack()'s match_heartbeats_left = 0: the entry's context_id goes invalid in the same place, and every
    reader of the count skips an invalid entry first, so the line decides nothing on its own.
  - C8's fix as first drafted, the last one counted down too once the reader had acknowledged anything (tt_PeerAck.
    acked): that variant survived this sweep. A reader that has acknowledged has a writer proxy and asks for what it
    lacks, so its ack passes the match point soon after anyway; the condition decided nothing and was not kept.

Run from the repository root. Restores the file it touches, whatever happens. MUTANTS=<substring> runs only the mutants
whose name contains it (the control always runs). A mutant whose binary, debug sections aside, is the control's
(tests/mutant_digest.py) is reported DEAD (NOT_COMPILED) and counted as a survivor: it edited code this build leaves out.
"""
import os
import pathlib
import subprocess
import sys

from mutant_digest import NOT_COMPILED, code_digest, unchanged

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = "src/tickle.c"
TEST = "obj/debug/tests/test_volatile_match_point"

# (name, old, new)
MUTANTS = [
    (
        "no match Heartbeat armed",
        "    ack->match_heartbeats_left = tt_MATCH_HEARTBEATS;\n",
        "    ack->match_heartbeats_left = 0;\n",
    ),
    (
        "C8: the last one counted down too, heard or not",
        "        if (ack->match_heartbeats_left > 1) {\n            ack->match_heartbeats_left--;",
        "        if (true) {\n            ack->match_heartbeats_left--;",
    ),
    (
        "C8: the last match Heartbeat stays FINAL (a healthy reader never answers)",
        "    uint8_t flags = solicit ? 0 : tt_HEARTBEAT_FLAG_FINAL;",
        "    uint8_t flags = tt_HEARTBEAT_FLAG_FINAL;",
    ),
    (
        "the reader's ack not ending it",
        "        ack->match_heartbeats_left = 0; // it has the owed sample: its baseline is at or below it\n",
        "",
    ),
    (
        "the periodic and idle HEARTBEATs carry no match point",
        "static bool match_point_owed(const struct tt_Publisher* pub, uint32_t* owed) {\n    bool any = false;\n",
        "static bool match_point_owed(const struct tt_Publisher* pub, uint32_t* owed) {\n    bool any = false;\n"
        "    return false;\n",
    ),
]


def build_and_run(control=None):
    env = dict(os.environ, TMPDIR=os.environ.get("TMPDIR", "/home/semih/tmp_build"))
    build = subprocess.run(["make", "-s", "-C", str(ROOT / "platform/linux"), TEST], capture_output=True, text=True,
                           env=env)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    if unchanged(control, ROOT / "platform/linux" / TEST):
        return NOT_COMPILED, ""
    try:
        run = subprocess.run(["bash", "-c", f"ulimit -v 2097152; exec ./{TEST}"], cwd=ROOT / "platform/linux",
                             capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return "FAILED", "timeout"
    return ("PASSED" if run.returncode == 0 else "FAILED"), run.stdout[-1500:]


def main():
    path = ROOT / SRC
    original = path.read_text()
    only = os.environ.get("MUTANTS", "")
    survivors = []
    try:
        verdict, out = build_and_run()
        print(f"control: {verdict}")
        if verdict != "PASSED":
            print(out)
            return 2
        control = code_digest(ROOT / "platform/linux" / TEST)
        for name, old, new in MUTANTS:
            if only and only not in name:
                continue
            if original.count(old) != 1:
                print(f"{name}: the text to mutate occurs {original.count(old)} times - the sweep is stale")
                return 2
            path.write_text(original.replace(old, new))  # a new mtime: make rebuilds the test
            verdict, out = build_and_run(control)
            killed = verdict == "FAILED"
            print(f"{name}: {'killed' if killed else ('DEAD (NOT_COMPILED)' if verdict == NOT_COMPILED else verdict)}")
            if not killed:
                survivors.append(name)
    finally:
        path.write_text(original)
    # The restored file has a new mtime, so the next build is of the real code again.
    if survivors:
        print(f"{len(survivors)} mutant(s) survived: {survivors}")
        return 1
    print("every mutant killed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

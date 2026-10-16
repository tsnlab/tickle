#!/usr/bin/env python3
"""Mutant sweep for the idle HEARTBEAT (config.h's tt_IDLE_HEARTBEAT_PERIOD, DESIGN.md section 6, 2026-10-16).

Each entry breaks one condition of the mechanism and must turn tests/test_idle_heartbeat.c red. A mutant that still
passes means the test does not test what it says, or the condition decides nothing and should not be in the code.

Not mutated, and why:
  - idle_heartbeat_note_sent()'s "to every matched reader" test: with the one reader the test pairs, every HEARTBEAT
    goes to every matched reader; the condition guards a two-reader case (an eviction HEARTBEAT to one of them) whose
    only effect is a later idle HEARTBEAT, never a missing one.

Run from the repository root. Restores the file it touches, whatever happens. MUTANTS=<substring> runs only the mutants
whose name contains it (the control always runs).
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = "src/tickle.c"
TEST = "obj/debug/tests/test_idle_heartbeat"

# (name, old, new)
MUTANTS = [
    (
        "owed: never owed (nothing unacknowledged)",
        "ack->reliable_reader && seq_delta(ack->ack_seq_no, next) < 0) {",
        "ack->reliable_reader && false) {",
    ),
    (
        "owed: a BEST_EFFORT reader waited on",
        "ack->context_id != tt_CONTEXT_ID_INVALID && ack->reliable_reader && seq_delta",
        "ack->context_id != tt_CONTEXT_ID_INVALID && seq_delta",
    ),
    (
        "owed: asked with no matched reader",
        "    if (!pub->reliable || pub->idle_heartbeat_ns == 0 || publisher_unmatched(pub)) {\n        return false;\n"
        "    }\n    uint32_t next",
        "    if (!pub->reliable || pub->idle_heartbeat_ns == 0) {\n        return false;\n    }\n    uint32_t next",
    ),
    (
        "publish: never arms or moves it",
        "    if (now >= pub->idle_check_ns) {\n        idle_heartbeat_on_publish(pub, now);",
        "    if (false) {\n        idle_heartbeat_on_publish(pub, now);",
    ),
    (
        "publish: arms it but never pushes it back",
        "    if (now >= pub->idle_check_ns) {\n        idle_heartbeat_on_publish(pub, now);",
        "    if (!pub->idle_heartbeat_armed && now >= pub->idle_check_ns) {\n        idle_heartbeat_on_publish(pub, now);",
    ),
    (
        "publish: pushed back only at the deadline (threshold at due)",
        "        pub->idle_check_ns = due - (period / 2U);\n        return;",
        "        pub->idle_check_ns = due;\n        return;",
    ),
    (
        "schedule: a second entry instead of moving the first",
        "    if (pub->idle_heartbeat_armed && sched_postpone_locked(pub->node, idle_heartbeat_fire, pub, due)) {",
        "    if (false) {",
    ),
    (
        "postpone: the moved entry not re-sifted",
        "            if (time >= was) {\n                sched_sift_down(node, i);",
        "            if (time >= was) {\n                (void)node;",
    ),
    (
        "postpone: an earlier time not sifted up",
        "            } else {\n                sched_sift_up(node, i);\n            }\n            return true;",
        "            }\n            return true;",
    ),
    (
        "fire: a later HEARTBEAT does not push it back",
        "    if (pub->last_heartbeat_ns != 0 && now - pub->last_heartbeat_ns < period) {",
        "    if (false) {",
    ),
    (
        "fire: not re-armed after a send",
        "    send_idle_heartbeat(node, pub);\n    idle_heartbeat_schedule(pub, now + period, period);",
        "    send_idle_heartbeat(node, pub);",
    ),
    (
        "send: FINAL (asks for no answer)",
        "                           unicast ? count : 0, 0);\n    pub->idle_heartbeats++;",
        "                           unicast ? count : 0, tt_HEARTBEAT_FLAG_FINAL);\n    pub->idle_heartbeats++;",
    ),
    (
        "note: a HEARTBEAT sent never restarts the clock",
        "    pub->last_heartbeat_ns = tt_get_ns();\n}",
        "}",
    ),
    (
        "period: no floor",
        "    return pub->idle_heartbeat_ns > floor ? pub->idle_heartbeat_ns : floor;",
        "    return pub->idle_heartbeat_ns;",
    ),
    (
        "destroy: left in the scheduler",
        "    if (pub->idle_heartbeat_armed) {\n        tt_Context_unschedule(node, idle_heartbeat_fire, pub);",
        "    if (pub->idle_heartbeat_armed) {\n        (void)node;",
    ),
]


def build_and_run():
    env = dict(os.environ, TMPDIR=os.environ.get("TMPDIR", "/home/semih/tmp_build"))
    build = subprocess.run(["make", "-s", "-C", str(ROOT / "platform/linux"), TEST], capture_output=True, text=True,
                           env=env)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        # A mutant can loop and log without end: its output goes through `tail`, so at most the end of it is held, and
        # the run is cut off at the timeout (the 2026-10-11 runaway that filled memory captured a child whole).
        run = subprocess.run(["bash", "-c", f"ulimit -v 2097152; set -o pipefail; timeout 240 ./{TEST} 2>&1 | tail -c 4000"],
                             cwd=ROOT / "platform/linux", capture_output=True, text=True, timeout=300)
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
        for name, old, new in MUTANTS:
            if only and only not in name:
                continue
            if original.count(old) != 1:
                print(f"{name}: the text to mutate occurs {original.count(old)} times - the sweep is stale")
                return 2
            path.write_text(original.replace(old, new))  # a new mtime: make rebuilds the test
            verdict, out = build_and_run()
            killed = verdict == "FAILED"
            print(f"{name}: {'killed' if killed else verdict}")
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

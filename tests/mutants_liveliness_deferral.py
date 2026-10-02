#!/usr/bin/env python3
"""Mutant sweep for the liveliness deferral (the 2026-09-30 CI failure, run 36787898559).

A node must not judge its peers dead from a window it spent descheduled: the liveliness check runs as
a scheduler entry, poll_once_nonblocking() runs every due entry BEFORE reading the socket, and a
starved process therefore wakes with its peers' datagrams queued and declares them dead without
reading one. Both sides of a starved pair do it to each other, which is what CI showed.

The deferral half was NOT the fix for the starved case, and the commit that added it said it was.
On Linux tt_rx_buffered() counts datagrams already pulled into our own batch, so it is zero exactly
on a starved wake-up; test_mock.h gives the same function the other meaning, which is why four
mutants died against a condition that cannot be true in production. The lateness allowance
(unobserved_ns) is the fix for that case, and its mutants are at the end of this list.

Each of these removes exactly one claim. Two of the four survived a
first attempt and are the reason this file exists rather than a single test: the cap was unreachable
because drain_rx() empties the queue in one pass, and the counter's reset was uncovered because every
arm used a fresh context.

Run from the repository root. Restores every file it touches, whatever happens.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_liveliness"

GUARD = "    if (tt_rx_buffered(node) > 0 && node->liveliness_deferrals < tt_LIVELINESS_MAX_DEFERRALS) {"
RESET = "    node->liveliness_deferrals = 0;\n    if (node->liveliness_check_scheduled) {"

MUTANTS = [
    (
        "deferring at all while datagrams are unread",
        "src/tickle.c",
        GUARD,
        "    if (false) {",
        "test_a_peer_with_datagrams_waiting_unread_is_not_dead",
    ),
    (
        "the cap on how long a busy socket may defer",
        "src/tickle.c",
        GUARD,
        "    if (tt_rx_buffered(node) > 0) {",
        "test_a_busy_socket_cannot_postpone_a_real_death_for_ever",
    ),
    (
        "resetting the count once a run judges",
        "src/tickle.c",
        RESET,
        "    if (node->liveliness_check_scheduled) {",
        "test_a_busy_socket_cannot_postpone_a_real_death_for_ever",
    ),
    (
        "asking whether anything is buffered at all",
        "src/tickle.c",
        GUARD,
        "    if (node->liveliness_deferrals < tt_LIVELINESS_MAX_DEFERRALS) {",
        "test_expires_peer_after_missed_intervals",
    ),
    # The lateness allowance: the window a node spent descheduled is not evidence of anyone's silence.
    # The second of these is the one that matters most - a guard that widens unconditionally forgives
    # the real deaths too, and from the surviving peer alone that looks identical to widening correctly.
    (
        "the allowance at all",
        "src/tickle.c",
        "    uint64_t limit = tt_LIVELINESS_SILENCE_NS + unobserved_ns;",
        "    uint64_t limit = tt_LIVELINESS_SILENCE_NS;",
        "test_a_descheduled_node_does_not_blame_its_peers",
    ),
    (
        "granting it from lateness rather than unconditionally",
        "src/tickle.c",
        "    if (node->liveliness_check_scheduled && time > node->liveliness_check_ns) {\n        unobserved_ns = time - node->liveliness_check_ns;\n    }",
        "    if (true) {\n        unobserved_ns = tt_LIVELINESS_SILENCE_NS * 100;\n    }",
        "test_an_ordinary_run_does_not_widen_the_limit",
    ),
    (
        "measuring the lateness at all",
        "src/tickle.c",
        "        unobserved_ns = time - node->liveliness_check_ns;",
        "        unobserved_ns = 0;",
        "test_a_descheduled_node_does_not_blame_its_peers",
    ),
]

def build_and_run():
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True, cwd=ROOT / "platform/linux",
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    run = subprocess.run([str(BIN)], capture_output=True, text=True)
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    backup = tempfile.mkdtemp(prefix="shm_mutants_")
    touched = {m[1] for m in MUTANTS}
    for rel in touched:
        shutil.copy2(ROOT / rel, pathlib.Path(backup) / rel.replace("/", "_"))

    def restore():
        for rel in touched:
            # copy(), not copy2(), and then touch: copy2 preserves the ORIGINAL mtime, so a restored
            # file looks older than the object files built from the mutant while it was applied, and
            # make then considers the build up to date. The next `make test` after a sweep ran a
            # binary compiled from mutated source and reported failures that were not in the tree -
            # a check answering about something other than the thing being checked, in the harness
            # whose whole job is to prevent exactly that.
            shutil.copy(pathlib.Path(backup) / rel.replace("/", "_"), ROOT / rel)
            (ROOT / rel).touch()

    failures = 0
    try:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                      -> {verdict}")
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1

        for name, rel, old, new, test in MUTANTS:
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                # The substitution that matches nothing is the sweep's own version of a test that
                # cannot fail, so it is an error rather than a skip.
                print(f"[ERROR]    {name:40s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            assert path.read_text().count(old) == 0, "substitution did not apply"
            verdict, output = build_and_run()
            restore()
            expected = "FAIL"
            ok = verdict == expected
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:40s} -> {verdict} (wanted {expected}, guard: {test})")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)

    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

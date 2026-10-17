#!/usr/bin/env python3
"""Mutant sweep for discovery replies sent without waiting for room (tickle.c send_unicast_update_unless_full()).

Each entry undoes one part of the change and must turn platform/linux/discovery_reply_check.sh red (FAIL, exit 1): a
reply that waits again fails its bound on a poll of A while the send buffer is full; a reply that never goes fails its
convergence. A mutant that still passes means the check does not test what it says.

Not mutated, and why:
  - send_unicast_update_unless_full() leaving reply_unless_full set: every later flush would skip on a full buffer,
    data included. The check's context has no endpoints, so it sends no data to see dropped; the flag is cleared on the
    line after the one send it covers, which a reading settles.
  - answer_discovery_request()'s blocking fallback when the broadcast before it did not go: flush_pending_broadcast()
    fails only on an error other than a full buffer (a blocking send waits), which the check cannot make.

Run from the repository root. Needs passwordless `sudo ip` (the namespaces). Restores every file it touches, whatever
happens. MUTANTS=<substring> runs only the mutants whose name contains it (the control always runs). A mutant whose
binary, debug sections aside, is the control's (tests/mutant_digest.py) is reported DEAD (NOT_COMPILED) and counted as
not caught.
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged

ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = "src/tickle.c"
BINARIES = [ROOT / "platform/linux/discovery_reply_check"]

# (name, file, old, new)
MUTANTS = [
    (
        "blocking: neither reply skips on a full buffer",
        CORE,
        "    node->large.reply_unless_full = true;\n",
        "    node->large.reply_unless_full = false;\n",
    ),
    (
        "blocking: the reply to an announce waits for room",
        CORE,
        "    send_unicast_update_unless_full(node, &reply_to);\n",
        "    build_and_send_update(node, &reply_to, 1);\n",
    ),
    (
        "blocking: the answer to a request waits for room",
        CORE,
        "    send_unicast_update_unless_full(node, &requester); //",
        "    build_and_send_update(node, &requester, 1); //",
    ),
    (
        "dropped: no reply ever goes",
        CORE,
        "        int32_t sent = large_send_batch(node, &datagram, 1, &dest);\n",
        "        int32_t sent = 0;\n        (void)datagram;\n        (void)dest;\n",
    ),
    (
        "dropped: once one reply was skipped, none goes again",
        CORE,
        "        int32_t sent = large_send_batch(node, &datagram, 1, &dest);\n",
        "        int32_t sent =\n"
        "            node->large.discovery_replies_skipped != 0 ? 0 : large_send_batch(node, &datagram, 1, &dest);\n",
    ),
    (
        "dropped: the answer to a request never goes",
        CORE,
        "    send_unicast_update_unless_full(node, &requester); //",
        "    (void)requester; //",
    ),
]


def build_and_run(control=None):
    build = subprocess.run(["make", "-s", "-C", str(ROOT / "platform/linux"), *(path.name for path in BINARIES)],
                           capture_output=True, text=True)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    if unchanged(control, *BINARIES):
        return NOT_COMPILED, ""
    try:
        run = subprocess.run(["bash", "-c", "ulimit -v 2097152; exec timeout 120 ./discovery_reply_check.sh"],
                             cwd=ROOT / "platform/linux", capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode == 2:
        return "COULD_NOT_RUN", run.stdout[-1000:] + run.stderr[-1000:]
    if run.returncode == 3:
        return "VOID", run.stdout[-1000:]
    if run.returncode < 0 or run.returncode in (124, 134, 139):
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    files = sorted({m[1] for m in MUTANTS})
    backup = pathlib.Path(tempfile.mkdtemp(prefix="discovery_reply_mutants_"))
    for name in files:
        (backup / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, backup / name)

    def restore():
        # copy(), then touch: copy2 keeps the original mtime and make would reuse the mutant's binary.
        for name in files:
            shutil.copy(backup / name, ROOT / name)
            (ROOT / name).touch()

    signal.signal(signal.SIGTERM, lambda _s, _f: sys.exit(1))
    only = os.environ.get("MUTANTS", "")
    failures = 0
    try:
        verdict, output = build_and_run()
        print(f"[control]  discovery_reply_check.sh unmutated -> {verdict}")
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1
        control = code_digest(*BINARIES)
        for name, path, old, new in MUTANTS:
            if only and only not in name:
                continue
            text = (backup / path).read_text()
            if text.count(old) != 1:
                print(f"[ANCHOR]   {name}: matched {text.count(old)} times, not once")
                failures += 1
                continue
            (ROOT / path).write_text(text.replace(old, new))
            try:
                verdict, output = build_and_run(control)
            finally:
                restore()
            caught = verdict in ("FAIL", "CRASH", "HANG")  # VOID, BUILD_FAILED, COULD_NOT_RUN decide nothing
            why = next((line for line in output.splitlines() if "FAIL -" in line), "")[:150]
            print(f"[{label(verdict, caught, 'caught', 'MISSED')}]   {name:<52} -> {verdict} {why}")
            failures += 0 if caught else 1
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"{len(MUTANTS)} mutants, {failures} not caught" if failures else f"all {len(MUTANTS)} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

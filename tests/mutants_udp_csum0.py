#!/usr/bin/env python3
"""Mutant sweep for UDP checksum 0 (hal_linux.c "UDP checksum 0", on by default, off with TT_UDP_CSUM0=0).

Each entry breaks one condition of the option and must turn platform/linux/udp_offload_check.sh red: its arms F and G
send with it on (F by default, G with TT_UDP_CSUM0=1) and the receiver's ingress counts the datagrams that carry UDP checksum 0; B, A and C are the
controls with TT_UDP_CSUM0=0. A mutant that still passes means the check does not test what it says, or the condition decides
nothing and should not be in the code.

Not mutated, and why - each decides no outcome the check can observe:
  - send_batch_flags()'s EINVAL retry with SO_NO_CHECK turned off (csum0_retries): it answers a UDP_SEGMENT call made
    after another thread turned it on, a race a single-threaded check cannot open. The check requires csum0_retries=0,
    so a retry taken where none is due is caught (the "clear" mutant reaches it, and is caught by that count).
  - csum0_setup()'s -1 when the kernel refuses SO_NO_CHECK: every kernel since 2.6 takes it.
  - csum0_sent()'s `result <= 0`: every call of the check succeeds.
  - `+= call->carried[i]` (not `+= 1`): a checksum-0 call never carries a UDP_SEGMENT run, so carried[i] is 1 in each.
  - csum0_retry()'s `node->hal.csum0 <= 0`: with the option off it sends a refused UDP_SEGMENT call to gso_refused()
    as before; no arm of the check has a UDP_SEGMENT call refused with the option off.

Run from the repository root. Needs passwordless `sudo ip` (the namespaces). Restores every file it touches, whatever
happens. MUTANTS=<substring> runs only the mutants whose name contains it (the control always runs). A mutant whose two
binaries, debug sections aside, are both the control's (tests/mutant_digest.py) is reported DEAD (NOT_COMPILED) and
counted as not caught: it edited code these builds leave out.
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
HAL = "src/hal_linux.c"
# Both binaries udp_offload_check.sh runs; a mutant is NOT_COMPILED only if neither changed.
BINARIES = [ROOT / "platform/linux/udp_offload_check", ROOT / "platform/linux/udp_offload_check_flap"]

# (name, file, old, new)
MUTANTS = [
    (
        "set: SO_NO_CHECK never turned on for a call",
        HAL,
        "        csum0_set(node, call->zero);",
        "        csum0_set(node, false);",
    ),
    (
        "clear: SO_NO_CHECK left on for a UDP_SEGMENT call",
        HAL,
        "        csum0_set(node, call->zero);",
        "        csum0_set(node, call->zero || node->hal.csum0_on);",
    ),
    (
        "switch: setsockopt() whatever the socket's state",
        HAL,
        "    if (node->hal.csum0_on == enable) {\n        return;\n    }\n",
        "",
    ),
    (
        "state: a switch not remembered",
        HAL,
        "        node->hal.csum0_on = enable;\n        node->hal.csum0_switches++;",
        "        node->hal.csum0_switches++;",
    ),
    (
        "count: datagrams counted with the option off",
        HAL,
        "    if (!node->hal.csum0_on || result <= 0) {",
        "    if (node->hal.csum0 <= 0 || result <= 0) {",
    ),
    (
        "split: a UDP_SEGMENT run joins a checksum-0 call",
        HAL,
        "if ((fragmented && call->segmented) || (run > 1 && call->zero)) {",
        "if (false) {",
    ),
    (
        "split: an IP-fragmented datagram joins a UDP_SEGMENT call",
        HAL,
        "if ((fragmented && call->segmented) || (run > 1 && call->zero)) {",
        "if (run > 1 && call->zero) {",
    ),
    (
        "scope: every datagram counts as IP-fragmented",
        HAL,
        "const bool fragmented = first->head_len + first->body_len > tt_ETHERNET_UDP_PAYLOAD;",
        "const bool fragmented = first->head_len + first->body_len > 0U;",
    ),
    (
        "scope: a full 1472-byte datagram counts as IP-fragmented",
        HAL,
        "const bool fragmented = first->head_len + first->body_len > tt_ETHERNET_UDP_PAYLOAD;",
        "const bool fragmented = first->head_len + first->body_len >= tt_ETHERNET_UDP_PAYLOAD - 4U;",
    ),
    (
        "count: datagrams with checksum 0 not counted",
        HAL,
        "        node->hal.csum0_datagrams += call->carried[i];\n",
        "        (void)call->carried[i];\n",
    ),
    (
        "env: TT_UDP_CSUM0=0 ignored, always on",
        HAL,
        '    if (setting != NULL && strcmp(setting, "0") == 0) {\n        return; // every datagram keeps its checksum\n    }\n    int enable = 1;',
        "    if (false) {\n        return; // every datagram keeps its checksum\n    }\n    int enable = 1;",
    ),
    (
        "env: the default ignored, never on",
        HAL,
        '    if (setting != NULL && strcmp(setting, "0") == 0) {\n        return; // every datagram keeps its checksum\n    }\n    int enable = 1;',
        "    if (true) {\n        return; // every datagram keeps its checksum\n    }\n    int enable = 1;",
    ),
    (
        "treatment: csum0= reported 1 whatever the setting",
        "platform/linux/udp_offload_check.c",
        "#define CSUM0_STATE ((int)g_node.hal.csum0)",
        "#define CSUM0_STATE 1",
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
        run = subprocess.run(["bash", "-c", "ulimit -v 2097152; exec timeout 600 ./udp_offload_check.sh"],
                             cwd=ROOT / "platform/linux", capture_output=True, text=True, timeout=700)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode == 2:
        return "COULD_NOT_RUN", run.stdout[-1000:] + run.stderr[-1000:]
    if run.returncode < 0 or run.returncode in (124, 134, 139):
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    files = sorted({m[1] for m in MUTANTS})
    backup = pathlib.Path(tempfile.mkdtemp(prefix="udp_csum0_mutants_"))
    for name in files:
        (backup / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, backup / name)

    def restore():
        # copy(), then touch: copy2 would keep the original mtime, older than the mutant's objects, and make would then
        # reuse a binary built from mutated source (tests/mutants_shm_stage1.py, 2026-10-02).
        for name in files:
            shutil.copy(backup / name, ROOT / name)
            (ROOT / name).touch()

    signal.signal(signal.SIGTERM, lambda _s, _f: sys.exit(1))
    only = os.environ.get("MUTANTS", "")
    failures = 0
    try:
        verdict, output = build_and_run()
        print(f"[control]  udp_offload_check.sh unmutated -> {verdict}")
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
            caught = verdict in ("FAIL", "CRASH", "HANG")  # BUILD_FAILED / COULD_NOT_RUN: the mutant itself is wrong
            why = next((line for line in output.splitlines() if "FAIL -" in line), "")[:150]
            print(f"[{label(verdict, caught, 'caught', 'MISSED')}]   {name:<62} -> {verdict} {why}")
            failures += 0 if caught else 1
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"{len(MUTANTS)} mutants, {failures} not caught" if failures else f"all {len(MUTANTS)} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

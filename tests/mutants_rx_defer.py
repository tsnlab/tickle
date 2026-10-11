#!/usr/bin/env python3
"""Mutant sweep for receive coalescing while a large sample arrives (DESIGN.md section 2, "Receive path").

Each entry breaks one condition of core's large_expect() (src/tickle.c) or the HAL's deferral (src/hal_linux.c) and
names the check that must go red for it: tests/test_rx_defer.c for core, platform/linux/rx_defer_check.sh (the HAL
against real sockets in private namespaces, with and without epoll, and end to end) for the HAL and for core's call
site. A mutant that still passes means the check does not test what it says, or the condition decides nothing and
should not be in the code.

Run from the repository root; needs passwordless `sudo ip` for the HAL entries. Restores every file it touches,
whatever happens. MUTANTS=<substring> runs only the mutants whose name contains it (the controls always run).
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LINUX = ROOT / "platform/linux"
CORE = "src/tickle.c"
HAL = "src/hal_linux.c"

MUTANTS = [
    # core: large_expect()
    ("core: no budget still reads the clock", CORE,
     "    if (placed == 0 || node->rx_defer_budget == 0) {", "    if (placed == 0) {",
     "unit", "test_no_budget_reads_no_clock"),
    ("core: an idle wait does not break the chain", CORE,
     "        large->wait_last = NULL; // nothing arrived since the last wait", "        ; // nothing arrived since the last wait",
     "unit", "test_an_idle_wait_breaks_the_measurement"),
    ("core: another assembly measured against the last", CORE,
     "    if (last != NULL && last == large->wait_last && last->claimed", "    if (last != NULL && last->claimed",
     "unit", "test_a_steady_sample_defers_the_budget: a context's first pass, wait_last still NULL"),
    ("core: a reused entry is the same sample", CORE,
     " && last->claimed == large->wait_claimed && last->buffer != NULL) {", " && last->buffer != NULL) {",
     "unit", "test_a_reused_entry_is_another_sample"),
    ("core: an abandoned sample deferred for", CORE,
     " && last->claimed == large->wait_claimed && last->buffer != NULL) {", " && last->claimed == large->wait_claimed) {",
     "unit", "test_an_abandoned_sample_is_not_deferred_for"),
    ("core: what is to come counts the holes", CORE,
     "        const uint32_t ahead = (uint32_t)last->frag_count - last->next_index;",
     "        const uint32_t ahead = (uint32_t)last->frag_count - last->received;",
     "unit", "test_holes_behind_the_front_are_not_waited_for"),
    ("core: the front is never moved", CORE,
     "    if (index >= assembly->next_index) {\n        assembly->next_index", "    if (false) {\n        assembly->next_index",
     "unit", "test_the_last_stretch_defers_half_of_it"),
    ("core: the last stretch deferred whole", CORE,
     "        const uint32_t half = ahead / 2U;", "        const uint32_t half = ahead;",
     "unit", "test_the_last_stretch_defers_half_of_it"),
    ("core: a single one deferred for", CORE,
     "        if (k >= 2U) {\n            const uint64_t defer", "        if (k >= 1U) {\n            const uint64_t defer",
     "unit", "test_a_single_one_is_not_deferred_for"),
    ("core: no cap at the fastest rate", CORE,
     "            node->rx_defer_ns = defer < cap ? defer : cap;", "            node->rx_defer_ns = defer;",
     "unit", "test_the_fastest_rate_seen_caps_the_wait"),
    ("core: the fastest is the latest", CORE,
     "        if (interval > 0 && (large->fastest_ns == 0 || interval < large->fastest_ns)) {", "        if (interval > 0) {",
     "unit", "test_the_fastest_rate_seen_caps_the_wait"),
    ("core: never asked before a wait", CORE,
     "#if tt_LARGE_SAMPLES\n    large_expect(node);\n#endif\n", "",
     "hal", "rx_defer_check.sh e2e deferral (rx_defers > 0)"),
    # HAL: rx_defer() and its sets
    ("hal: the wake eventfd not in the deferral's set", HAL,
     "    if (defer_fd >= 0 && !wait_set_add(defer_fd, node->hal.wake_fd, EPOLLIN, RX_READY_WAKE)) {",
     "    if (defer_fd < 0) {",
     "hal", "rx_defer_check wake"),
    ("hal: the bell not in the deferral's set", HAL,
     "    if (node->hal.defer_fd_plus1 > 0 && epoll_ctl(node->hal.defer_fd_plus1 - 1, EPOLL_CTL_ADD, bell, &event) != 0) {",
     "    if (false) {",
     "hal", "rx_defer_check bell"),
    ("hal: ppoll() deferral without the wake eventfd", HAL,
     "            {.fd = node->hal.wake_fd, .events = POLLIN, .revents = 0},          // NOLINT(misc-include-cleaner)\n",
     "            {.fd = -1, .events = POLLIN, .revents = 0},\n",
     "hal", "rx_defer_check wake (under refuse_epoll.so)"),
    ("hal: ppoll() deferral without the bell", HAL,
     "            {.fd = node->hal.bell_fd_plus1 - 1, .events = POLLIN, .revents = 0}, // NOLINT(misc-include-cleaner)\n",
     "            {.fd = -1, .events = POLLIN, .revents = 0},\n",
     "hal", "rx_defer_check bell (under refuse_epoll.so)"),
    ("hal: the request outlives its wait", HAL,
     "    uint64_t defer = node->rx_defer_ns;\n    node->rx_defer_ns = 0;\n", "    uint64_t defer = node->rx_defer_ns;\n",
     "hal", "rx_defer_check once"),
    ("hal: a deferral past the caller's deadline", HAL,
     "    if (timeout > 0 && defer > (uint64_t)timeout) {\n        defer = (uint64_t)timeout;\n    }\n", "",
     "hal", "rx_defer_check deadline"),
    ("hal: the deadline reached still waits", HAL,
     "    if (left <= 0) {\n        return true;\n    }", "    if (left <= 0) {\n        return false;\n    }",
     "hal", "rx_defer_check deadline"),
    ("hal: the deferral not taken out of the timeout", HAL,
     "    *timeout_io = left;\n", "",
     "hal", "rx_defer_check rest"),
    ("hal: the deferral never waits", HAL,
     "        (void)epoll_pwait2(node->hal.defer_fd_plus1 - 1, events, 2, &defer_ts, NULL);", "        (void)events;",
     "hal", "rx_defer_check defers"),
    ("hal: never deferred", HAL,
     "    if (node->rx_defer_ns > 0 && rx_defer(node, &timeout)) {", "    if (false && rx_defer(node, &timeout)) {",
     "hal", "rx_defer_check defers"),
    ("hal: deferrals not counted", HAL,
     "    node->rx_defers++;\n    if (timeout <= 0) {", "    if (timeout <= 0) {",
     "hal", "rx_defer_check defers"),
    ("hal: TT_RX_DEFER=0 ignored", HAL,
     "    if (setting != NULL && strcmp(setting, \"0\") == 0) {\n        return;\n    }\n    int granted",
     "    int granted",
     "hal", "rx_defer_check budget (off)"),
    ("hal: the budget counts a datagram at its size", HAL,
     "(uint32_t)granted / (2U * (uint32_t)tt_CONTROL_MAX_LENGTH) / RX_DEFER_SHARE;",
     "(uint32_t)granted / (uint32_t)tt_CONTROL_MAX_LENGTH / RX_DEFER_SHARE;",
     "hal", "rx_defer_check budget"),
    ("hal: half the buffer, not a quarter", HAL,
     "#define RX_DEFER_SHARE 4U", "#define RX_DEFER_SHARE 2U",
     "hal", "rx_defer_check budget"),
]


def run(cmd, timeout):
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=LINUX)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1500:]
    if done.returncode < 0:
        return "CRASH", done.stdout[-1500:] + done.stderr[-1500:]
    return ("PASS" if done.returncode == 0 else "FAIL"), done.stdout + done.stderr


def build_and_run(kind):
    targets = ["obj/debug/tests/test_rx_defer"] if kind == "unit" else ["rx_defer_check", "refuse_epoll.so",
                                                                         "large_sample_bench"]
    build = subprocess.run(["make", "-s", *targets], capture_output=True, text=True, cwd=LINUX)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    if kind == "unit":
        return run(["./obj/debug/tests/test_rx_defer"], 60)
    return run(["./rx_defer_check.sh"], 120)


def failing_lines(output):
    return [line for line in output.splitlines() if "FAIL" in line or "test_rx_defer.c:" in line][:3]


def main():
    backup = pathlib.Path(tempfile.mkdtemp(prefix="rx_defer_mutants_"))
    for rel in (CORE, HAL):
        shutil.copy2(ROOT / rel, backup / pathlib.Path(rel).name)

    def restore():
        # copy(), then touch: copy2 would keep the original mtime, older than the mutant's objects, and make would then
        # reuse a binary built from mutated source (tests/mutants_shm_stage1.py, 2026-10-02).
        for rel in (CORE, HAL):
            shutil.copy(backup / pathlib.Path(rel).name, ROOT / rel)
            (ROOT / rel).touch()

    signal.signal(signal.SIGTERM, lambda _s, _f: sys.exit(1))
    failures = 0
    try:
        for kind in ("unit", "hal"):
            verdict, output = build_and_run(kind)
            print(f"[control]  unmutated, {kind:4s}                                        -> {verdict}", flush=True)
            if verdict != "PASS":
                print(output[-2000:])
                print("a control did not pass; the sweep cannot decide anything")
                return 1
        only = os.environ.get("MUTANTS", "")
        for name, rel, old, new, kind, guard in MUTANTS:
            if only and only not in name:
                continue
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name:55s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            verdict, output = build_and_run(kind)
            restore()
            ok = verdict in ("FAIL", "CRASH", "HANG")
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:55s} -> {verdict} (guard: {guard})", flush=True)
            for line in failing_lines(output) if ok else []:
                print(f"           {line}")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

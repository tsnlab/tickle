#!/usr/bin/env python3
"""Mutant sweep for receiving runs in place (DESIGN.md section 8, "Receiving runs in place").

Each entry breaks one condition the read in place relies on and names what must go red: core's half
(tests/test_rx_direct.c, mock HAL) or the HAL's (platform/linux/rx_direct_check.c, real sockets, run in a private
network namespace of its own). A mutant that still passes means the test does not test what it says, or the condition
decides nothing and should not be in the code.

Not mutated, and why - each decides no outcome a test can observe:
  - large_place()'s `fragment->payload != place`: a copy of a unit onto itself is the same bytes; what it decides is
    the cost, which rx_gap_pc.sh measures (instructions per MB) and direct_runs counts.
  - direct_read()'s second look at direct_bytes before the read (`bytes == 0 ||`): the window between the relaxed and
    the sequentially consistent load is a race no single-threaded check can open.
  - direct_read()'s `got > framing` before the copy back: without it the bytes copied lie past the datagram's end,
    where nothing reads.
  - tt_receive()'s clear of direct_split on TT_RX_DROP_PERCENT's drop: compiled only into that experiment's builds.
  - tt_try_receive()'s -1 while a kept drain runs: without it the pass reads a socket the drain is also reading, an
    order a datagram's arrival time decides - the drain's task work takes it on arrival in every run a check can make.
  - large_run_batched()'s compatibility check: no test reaches an RxO-incompatible large writer, in either path.

Run from the repository root. Needs passwordless `sudo ip` (the namespace). Restores every file it touches, whatever
happens. MUTANTS=<substring> runs only the mutants whose name contains it (the controls always run).
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "platform/linux/obj/debug/tests"
CORE = "test_rx_direct"
HAL = "rx_direct_check"
DRAIN = "rx_drain_check"  # run twice: the buffer ring this kernel gives, and classic buffers (refuse_pbuf_ring.so)

# (name, file, old, new, binary that must fail)
MUTANTS = [
    (
        "aim: no hint withdrawn when a unit lands another way in the hinted assembly",
        "src/tickle.c",
        "        if (assembly == node->large.direct) {\n            // A unit landed here another way",
        "        if (false) {\n            // A unit landed here another way",
        CORE,
    ),
    (
        "aim: a hint over a landed unit",
        "src/tickle.c",
        "            if (large_has_landed(assembly, next + k)) {\n                units = 0;",
        "            if (large_has_landed(assembly, next + k)) {\n                units = units;",
        CORE,
    ),
    (
        "aim: a reverse-endian writer hinted",
        "src/tickle.c",
        "    } else if (assembly != NULL && large->direct_allowed && tt_is_native_endian(header)) {",
        "    } else if (assembly != NULL && large->direct_allowed) {",
        CORE,
    ),
    (
        "aim: TT_RX_DIRECT=0 ignored",
        "src/tickle.c",
        "    } else if (assembly != NULL && large->direct_allowed && tt_is_native_endian(header)) {",
        "    } else if (assembly != NULL && tt_is_native_endian(header)) {",
        CORE,
    ),
    (
        "aim: no run landed, hinted anyway (a stale assembly)",
        "src/tickle.c",
        "    } else if (assembly != NULL && large->direct_allowed && tt_is_native_endian(header)) {",
        "    } else if ((assembly = assembly != NULL ? assembly : &node->large.assemblies[0]) != NULL &&\n"
        "               large->direct_allowed && tt_is_native_endian(header)) {",
        CORE,
    ),
    (
        "aim: units not bounded by the last run's length",
        "src/tickle.c",
        "        units = count - 1U - next < run_count ? count - 1U - next : run_count;",
        "        units = count - 1U - next;",
        CORE,
    ),
    (
        "aim: units past the last unit (which always goes alone)",
        "src/tickle.c",
        "        units = count - 1U - next < run_count ? count - 1U - next : run_count;",
        "        units = count - next < run_count ? count - next : run_count;",
        CORE,
    ),
    (
        "aim: the expected seq_no not moved on",
        "src/tickle.c",
        "    expected.data.seq_no = run->data.seq_no + run_count;",
        "    expected.data.seq_no = run->data.seq_no;",
        CORE,
    ),
    (
        "aim: the expected frag_index not moved on",
        "src/tickle.c",
        "    expected.frag_index = (uint16_t)next;",
        "    expected.frag_index = (uint16_t)index;",
        CORE,
    ),
    (
        "aim: the expected run_count the last one's",
        "src/tickle.c",
        "    expected.run_count = (uint16_t)units;",
        "    expected.run_count = (uint16_t)run_count;",
        CORE,
    ),
    (
        "aim: the place the last run's",
        "src/tickle.c",
        "    large->direct_dest = assembly->buffer + tt_LARGE_CDR_OFFSET + large_offset(next);",
        "    large->direct_dest = assembly->buffer + tt_LARGE_CDR_OFFSET + large_offset(index);",
        CORE,
    ),
    (
        "back-off: none",
        "src/tickle.c",
        "    if (large->direct_skip != 0) {\n        large->direct_skip--;",
        "    if (false) {\n        large->direct_skip--;",
        CORE,
    ),
    (
        "back-off: a miss not noticed",
        "src/tickle.c",
        "    } else if (large->direct_missed != large->direct_missed_seen) {",
        "    } else if (false) {",
        CORE,
    ),
    (
        "back-off: not started over after a run in place",
        "src/tickle.c",
        "    if (in_place) {\n        large->direct_streak = 0;\n    } else if",
        "    if (in_place) {\n    } else if",
        CORE,
    ),
    (
        "aim: placed not cleared before a run (a stale assembly from an MTU unit)",
        "src/tickle.c",
        "        node->large.direct_runs++;\n    }\n    node->large.placed = NULL;",
        "        node->large.direct_runs++;\n    }",
        CORE,
    ),
    (
        "place: the assembly a unit landed in not recorded",
        "src/tickle.c",
        "#if LARGE_DIRECT\n    node->large.placed = assembly;\n#endif",
        "",
        CORE,
    ),
    (
        "take: the run's payload read from the receive buffer, not from where the HAL put it",
        "src/tickle.c",
        "        payload = node->large.direct_split;\n        node->large.direct_split = NULL;",
        "        node->large.direct_split = NULL;",
        CORE,
    ),
    (
        "take: a run read in place for a freed assembly taken anyway",
        "src/tickle.c",
        "        if (node->large.direct == NULL) {\n            // Read into an assembly freed since",
        "        if (false) {\n            // Read into an assembly freed since",
        CORE,
    ),
    (
        "take: a split nothing took left for the next datagram",
        "src/tickle.c",
        "    node->large.direct_split = NULL; // a run read in place that nothing took",
        "    // a run read in place that nothing took",
        CORE,
    ),
    (
        "free: a buffer the HAL may be reading into given back at once",
        "src/tickle.c",
        "    if (node->large.direct == assembly && large_direct_withdraw(node)) {",
        "    if (node->large.direct == assembly && large_direct_withdraw(node) && false) {",
        CORE,
    ),
    (
        "free: a parked buffer never given back after the read",
        "src/tickle.c",
        "    large_direct_release_parked(node); // the HAL's read is over: a buffer parked during it goes back",
        "    // the HAL's read is over: a buffer parked during it goes back",
        CORE,
    ),
    (
        "batch: a run across the watermark taken as a span",
        "src/tickle.c",
        "    if (seq_delta(first->seq_no, proxy->ack_seq_no) < 0) {\n        return false;\n    }\n"
        "    if (first->seq_no - proxy->ack_seq_no + run_count",
        "    if (first->seq_no - proxy->ack_seq_no + run_count",
        CORE,
    ),
    (
        "batch: a run past the window taken as a span",
        "src/tickle.c",
        "    if (first->seq_no - proxy->ack_seq_no + run_count > proxy_window_bits(proxy)) {",
        "    if (false) {",
        CORE,
    ),
    (
        "batch: no assembly held, taken anyway",
        "src/tickle.c",
        "    if (assembly == NULL || assembly->frag_count != first->count) {",
        "    if (assembly != NULL && assembly->frag_count != first->count) {",
        CORE,
    ),
    (
        "batch: a count that contradicts the assembly taken",
        "src/tickle.c",
        "    if (assembly == NULL || assembly->frag_count != first->count) {",
        "    if (assembly == NULL) {",
        CORE,
    ),
    (
        "batch: the units recorded as one, not as their span",
        "src/tickle.c",
        "    node->rx_seq_span = (uint16_t)run_count;",
        "    node->rx_seq_span = 1U;",
        CORE,
    ),
    (
        "batch: no drain after the run",
        "src/tickle.c",
        "        proxy->large_arrival_ns = rx_now(node);\n        drain_reorder(node, sub, proxy);\n    }\n    return true;",
        "        proxy->large_arrival_ns = rx_now(node);\n    }\n    return true;",
        CORE,
    ),
    (
        "batch: with a best-effort Subscriber",
        "src/tickle.c",
        "            !((const struct tt_Subscriber*)endpoint)->reliable) {\n            return false;",
        "            !((const struct tt_Subscriber*)endpoint)->reliable) {\n            continue;",
        CORE,
    ),
    (
        "batch: with a best-effort assembly held",
        "src/tickle.c",
        "    if (large_assembly_find(node, NULL, header->source, entity_id, seq_no - index) != NULL) {\n        return false;",
        "    if (large_assembly_find(node, NULL, header->source, entity_id, seq_no - index) != NULL) {\n        (void)0;",
        CORE,
    ),
    (
        "batch: a run it cannot take dropped rather than taken a unit at a time",
        "src/tickle.c",
        "            large_accept_reliable(node, sub, &unit);\n            unit.payload += unit.length;",
        "            unit.payload += unit.length;",
        CORE,
    ),
    (
        "drain: an empty pass stops it (not kept)",
        "src/hal_linux.c",
        "            if (keep && hal->drain_armed == drain_sockets_open(hal)) {",
        "            if (false) {",
        DRAIN,
    ),
    (
        "drain: kept though an overrun ended a socket's recvmsg",
        "src/hal_linux.c",
        "            if (keep && hal->drain_armed == drain_sockets_open(hal)) {",
        "            if (keep) {",
        DRAIN,
    ),
    (
        "drain: a kept drain whose buffers are now too short not set up again",
        "src/hal_linux.c",
        "        if (datagram_bytes == 0 || (drain_size_for(hal, datagram_bytes) <= hal->drain_size && !grows)) {",
        "        if (datagram_bytes == 0 || !grows) {",
        DRAIN,
    ),
    (
        "drain: a kept drain that overran not given more buffers",
        "src/hal_linux.c",
        "        if (datagram_bytes == 0 || (drain_size_for(hal, datagram_bytes) <= hal->drain_size && !grows)) {",
        "        if (datagram_bytes == 0 || drain_size_for(hal, datagram_bytes) <= hal->drain_size) {",
        DRAIN,
    ),
    (
        "drain: the cancellations never submitted with the stop's wait",
        "src/hal_linux.c",
        "        long entered = syscall(__NR_io_uring_enter, hal->drain_fd, to_submit, 1U,",
        "        long entered = syscall(__NR_io_uring_enter, hal->drain_fd, 0U, 1U,",
        DRAIN,
    ),
    (
        "read: the framing before the unit count not compared",
        "src/hal_linux.c",
        "           memcmp(buf, large->direct_framing, at) == 0 &&",
        "           true &&",
        HAL,
    ),
    (
        "read: the framing after the unit count not compared",
        "src/hal_linux.c",
        "           memcmp(buf + after, large->direct_framing + after, framing - after) == 0;",
        "           true;",
        HAL,
    ),
    (
        "read: a count of 0 taken",
        "src/hal_linux.c",
        "    return count != 0 && payload <= bytes && got == (size_t)framing + payload &&",
        "    return payload <= bytes && got == (size_t)framing + payload &&",
        HAL,
    ),
    (
        "read: more units than hinted taken",
        "src/hal_linux.c",
        "    return count != 0 && payload <= bytes && got == (size_t)framing + payload &&",
        "    return count != 0 && got == (size_t)framing + payload &&",
        HAL,
    ),
    (
        "read: the length not checked against the count",
        "src/hal_linux.c",
        "    return count != 0 && payload <= bytes && got == (size_t)framing + payload &&",
        "    return count != 0 && payload <= bytes && got > (size_t)framing &&",
        HAL,
    ),
    (
        "read: a hint withdrawn during the read not looked at again",
        "src/hal_linux.c",
        "                __atomic_load_n(&large->direct_bytes, __ATOMIC_SEQ_CST) == bytes) {",
        "                true) {",
        HAL,
    ),
    (
        "read: the socket read again by the drain after a run in place (the empty read per wake-up)",
        "src/hal_linux.c",
        "                node->hal.rx_idle |= socket_fd == node->hal.data_sock ? TT_RX_IDLE_DATA : TT_RX_IDLE_WELL_KNOWN;",
        "",
        HAL,
    ),
    (
        "read: a miss not put back together",
        "src/hal_linux.c",
        "                    memcpy((uint8_t*)buf + framing, dest, back);",
        "                    (void)back;",
        HAL,
    ),
    (
        "read: aimed into a buffer too short for the hint",
        "src/hal_linux.c",
        "    if (bytes == 0 || len < (size_t)framing + bytes) {",
        "    if (bytes == 0) {",
        HAL,
    ),
    (
        "bind: TT_RX_DIRECT=0 ignored",
        "src/hal_linux.c",
        '        node->large.direct_allowed = direct == NULL || strcmp(direct, "0") != 0;',
        "        node->large.direct_allowed = true;",
        HAL,
    ),
]


def run_hal(binary, env=""):
    namespace = f"rxd-mut-{os.getpid()}"
    subprocess.run(["sudo", "-n", "ip", "netns", "add", namespace], check=True)
    try:
        subprocess.run(["sudo", "-n", "ip", "-n", namespace, "link", "set", "lo", "up"], check=True)
        subprocess.run(["sudo", "-n", "ip", "-n", namespace, "route", "add", "default", "dev", "lo"], check=True)
        return subprocess.run(
            ["sudo", "-n", "ip", "netns", "exec", namespace, "runuser", "-u", os.environ.get("USER", "semih"), "--",
             "bash", "-c", f"ulimit -v 2097152; cd {ROOT / 'platform/linux'}; exec env {env} timeout 120 ./{binary}"],
            capture_output=True, text=True, timeout=180,
        )
    finally:
        subprocess.run(["sudo", "-n", "ip", "netns", "del", namespace], check=False)


def build_and_run(binary):
    targets = [binary] if binary in (HAL, DRAIN) else [f"obj/debug/tests/{binary}"]
    if binary == DRAIN:
        targets.append("refuse_pbuf_ring.so")
    build = subprocess.run(["make", "-s", "-C", str(ROOT / "platform/linux"), *targets], capture_output=True, text=True)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        if binary == DRAIN:
            run = run_hal(binary)
            if run.returncode == 0:
                run = run_hal(binary, "TT_REFUSE_PBUF_RING=all TT_EXPECT_DRAIN_RING=classic "
                                      "LD_PRELOAD=./refuse_pbuf_ring.so")
        elif binary == HAL:
            run = run_hal(binary)
        else:
            run = subprocess.run(["bash", "-c", f"ulimit -v 2097152; exec {TESTS / binary}"], capture_output=True,
                                 text=True, timeout=300)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode < 0 or run.returncode in (124, 134, 139):
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    files = sorted({m[1] for m in MUTANTS})
    backup = pathlib.Path(tempfile.mkdtemp(prefix="rx_direct_mutants_"))
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
        for binary in sorted({m[4] for m in MUTANTS}):
            verdict, output = build_and_run(binary)
            print(f"[control]  {binary:<20} unmutated -> {verdict}")
            if verdict != "PASS":
                print(output[-2000:])
                print("control did not pass; the sweep cannot decide anything")
                return 1
        for name, path, old, new, binary in MUTANTS:
            if only and only not in name:
                continue
            text = (backup / path).read_text()
            if text.count(old) != 1:
                print(f"[ANCHOR]   {name}: matched {text.count(old)} times, not once")
                failures += 1
                continue
            (ROOT / path).write_text(text.replace(old, new))
            try:
                verdict, _ = build_and_run(binary)
            finally:
                restore()
            caught = verdict in ("FAIL", "CRASH", "HANG")  # BUILD_FAILED: the mutant itself is wrong
            print(f"[{'caught' if caught else 'MISSED'}]   {name:<84} {binary} -> {verdict}")
            failures += 0 if caught else 1
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"{len(MUTANTS)} mutants, {failures} not caught" if failures else f"all {len(MUTANTS)} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

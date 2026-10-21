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
  - tt_sys_result()'s -1 for a failure (rather than -errno): every caller of the raw calls tests `< 0` and reads errno,
    so the two cannot be told apart; -1 is the library's contract, kept for the next caller.
  - hal_recvmmsg(): reached only where tt_RX_BATCH > 1, which rx_direct_check's 64 KB buffers are not (one read a
    datagram); test-linux's two-node runs read through it - run by hand on 2026-10-15, a recvmmsg that says nothing
    came failed test-linux's perf tier (52.3% of the stream lost, limit 2%).

Run from the repository root. Needs passwordless `sudo ip` (the namespace). MUTANTS=<substring> runs only the mutants
whose name contains it (the controls always run). Refuses to start (exit 2) if any anchor is not in its file exactly
once - "batch: no drain after the run" matched nothing from 9d716d60, when the arrival stamp became
note_large_arrival(), and the sweep ran on reporting it as one failure among the rest - or if a file it edits is
already modified. Every child is bounded (tests/mutant_guard.py: 2 GB of address space, a deadline, at most 256 KB of
its output kept; the namespace runs keep their own ulimit -v and timeout inside); every file is restored after each
mutant, on an exception and on SIGINT/SIGTERM/SIGHUP, with a fresh mtime so make rebuilds.
"""
import os
import pathlib
import subprocess
import sys

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "platform/linux/obj/debug/tests"
CORE = "test_rx_direct"
HAL = "rx_direct_check"
HAL_RAW = "rx_direct_check_raw"  # the same check over the HAL's own system calls (13r for 13)
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
        "        note_large_arrival(node, proxy, rx_now(node));\n        drain_reorder(node, sub, proxy);\n    }\n    return true;",
        "        note_large_arrival(node, proxy, rx_now(node));\n    }\n    return true;",
        CORE,
    ),
    (
        "batch: a best-effort Subscriber tracking the writer not given the run",
        "src/tickle.c",
        "        if (!sub->reliable) {\n            best_effort_seen = true;\n            continue;",
        "        if (!sub->reliable) {\n            continue;",
        CORE,
    ),
    (
        "batch: a best-effort Subscriber nobody's writer tracks not given the run",
        "src/tickle.c",
        "    best_effort_seen = best_effort_seen || (!tracked && any_best_effort);",
        "",
        CORE,
    ),
    (
        "batch: a held best-effort assembly not given the units",
        "src/tickle.c",
        "        if (best_effort_seen || large_assembly_find(node, NULL, header->source, entity_id, seq_no - index) != NULL) {",
        "        if (best_effort_seen) {",
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
        "    if (keep && hal->drain_armed == drain_sockets_open(hal)) {",
        "    if (false) {",
        DRAIN,
    ),
    (
        "drain: kept though an overrun ended a socket's recvmsg",
        "src/hal_linux.c",
        "    if (keep && hal->drain_armed == drain_sockets_open(hal)) {",
        "    if (keep) {",
        DRAIN,
    ),
    (
        "drain: a kept drain whose buffers are now too short not set up again",
        "src/hal_linux.c",
        "    if (datagram_bytes == 0 || (drain_size_for(hal, datagram_bytes) <= hal->drain_size && !grows)) {",
        "    if (datagram_bytes == 0 || !grows) {",
        DRAIN,
    ),
    (
        "drain: a kept drain that overran not given more buffers",
        "src/hal_linux.c",
        "    if (datagram_bytes == 0 || (drain_size_for(hal, datagram_bytes) <= hal->drain_size && !grows)) {",
        "    if (datagram_bytes == 0 || drain_size_for(hal, datagram_bytes) <= hal->drain_size) {",
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
        "           memcmp(buf, large->direct_framing, count_at) == 0 &&",
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
        "        __atomic_load_n(&large->direct_bytes, __ATOMIC_SEQ_CST) == bytes) {",
        "        true) {",
        HAL,
    ),
    (
        "read: the socket read again by the drain after a run in place (the empty read per wake-up)",
        "src/hal_linux.c",
        "        node->hal.rx_idle |= socket_fd == node->hal.data_sock ? TT_RX_IDLE_DATA : TT_RX_IDLE_WELL_KNOWN;",
        "",
        HAL,
    ),
    (
        "read: a miss not put back together",
        "src/hal_linux.c",
        "        memcpy(buf + framing, dest, back);",
        "        (void)back;",
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
        '    node->large.direct_allowed = socket_switch("TT_RX_DIRECT", node->opts.socket.rx_direct);',
        "    node->large.direct_allowed = true;",
        HAL,
    ),
    # The deadline timer's rule (hal_linux.c rx_wait_epoll(), counted by plain waits since 2026-10-14): 12b and 12c.
    (
        "lazy timer: a fire does not start the run again",
        "src/hal_linux.c",
        "    hal->timer_fires++;\n    hal->timer_data_streak = 0;\n",
        "    hal->timer_fires++;\n",
        HAL,
    ),
    (
        "lazy timer: a plain wait counts a datagram that came after a fire",
        "src/hal_linux.c",
        "            return count; // the run of waits that ended for data is not grown: this wait cost a read",
        "            node->hal.timer_data_streak += (count > 0 && (*ready & RX_READY_FOR_DATA) != 0) ? 1U : 0U;\n"
        "            return count;",
        HAL,
    ),
    (
        "lazy timer: a plain wait counts a datagram that came with a fire",
        "src/hal_linux.c",
        "        *ready &= ~RX_READY_TIMER;\n        if (*ready != 0) {\n            return count - 1;\n        }\n"
        "        if (until != UINT64_MAX) {\n            const uint64_t now = tt_get_ns();",
        "        *ready &= ~RX_READY_TIMER;\n        if (*ready != 0) {\n            node->hal.timer_data_streak++;\n"
        "            return count - 1;\n        }\n"
        "        if (until != UINT64_MAX) {\n            const uint64_t now = tt_get_ns();",
        HAL,
    ),
    (
        "lazy timer: a plain wait that ended without data does not start the run again",
        "src/hal_linux.c",
        "        hal->timer_data_streak = 0; // a timeout, a wake, only room to send",
        "        (void)0; // a timeout, a wake, only room to send",
        HAL,
    ),
    (
        "lazy timer: a timed wait that ended without data does not start the run again",
        "src/hal_linux.c",
        "        node->hal.timer_data_streak = 0; // a timeout, a wake, only room to send",
        "        (void)0; // a timeout, a wake, only room to send",
        HAL,
    ),
    (
        "lazy timer: plain waits do not count",
        "src/hal_linux.c",
        "            hal->timer_data_streak++;\n",
        "",
        HAL,
    ),
    (
        "again: tt_send() does not send again after EAGAIN",
        "src/hal_linux.c",
        "    again.to = node->hal.broadcast_addr;\n"
        "    int32_t sent = (int32_t)hal_sendto(again.fd, buf, len, 0, "
        "(const struct sockaddr*)&again.to, sizeof(again.to));\n"
        "    if (__builtin_expect(sent < 0, 0)) {",
        "    again.to = node->hal.broadcast_addr;\n"
        "    int32_t sent = (int32_t)hal_sendto(again.fd, buf, len, 0, "
        "(const struct sockaddr*)&again.to, sizeof(again.to));\n"
        "    if (false) {",
        HAL,
    ),
    (
        "again: tt_send_to() does not send again after EAGAIN",
        "src/hal_linux.c",
        "    again.fd = node->hal.data_sock;\n"
        "    int32_t sent = (int32_t)hal_sendto(again.fd, buf, len, 0, "
        "(const struct sockaddr*)&again.to, sizeof(again.to));\n"
        "    if (__builtin_expect(sent < 0, 0)) {",
        "    again.fd = node->hal.data_sock;\n"
        "    int32_t sent = (int32_t)hal_sendto(again.fd, buf, len, 0, "
        "(const struct sockaddr*)&again.to, sizeof(again.to));\n"
        "    if (false) {",
        HAL,
    ),
    (
        "again: tt_send_iov() does not send again after EAGAIN",
        "src/hal_linux.c",
        "    if (__builtin_expect(sent < 0, 0)) {\n        sent = sendmsg_again(&again);",
        "    if (false) {\n        sent = sendmsg_again(&again);",
        HAL,
    ),
    (
        "again: sendto_again() waits and sends again after any refusal, not only a full buffer",
        "src/hal_linux.c",
        "    while (send_wait_for_room(again->fd)) {\n        sent = (int32_t)hal_sendto(",
        "    while (errno = EAGAIN, send_wait_for_room(again->fd)) {\n        sent = (int32_t)hal_sendto(",
        HAL,
    ),
    (
        "again: sendmsg_again() waits and sends again after any refusal, not only a full buffer",
        "src/hal_linux.c",
        "    while (send_wait_for_room(again->fd)) {\n        sent = (int32_t)hal_sendmsg(",
        "    while (errno = EAGAIN, send_wait_for_room(again->fd)) {\n        sent = (int32_t)hal_sendmsg(",
        HAL,
    ),
    (
        "again: sendto_again() sends other bytes than the refused send's",
        "src/hal_linux.c",
        "again->len, 0, (const struct sockaddr*)&again->to,",
        "again->len - 1U, 0, (const struct sockaddr*)&again->to,",
        HAL,
    ),
    (
        "raw: a failure's errno not the system call's",
        "src/hal_linux.c",
        "        errno = (int)-ret;\n        return -1;",
        "        errno = EIO;\n        return -1;",
        HAL_RAW,
    ),
    (
        "raw: sendto() with a length one short",
        "src/hal_linux.c",
        "(long)len, flags, (long)dest, (long)dest_len",
        "(long)len - 1, flags, (long)dest, (long)dest_len",
        HAL_RAW,
    ),
    (
        "raw: sendmsg() of another message",
        "src/hal_linux.c",
        "tt_sys6(SYS_sendmsg, socket_fd, (long)msg, flags, 0, 0, 0)",
        "tt_sys6(SYS_sendmsg, socket_fd, (long)msg, flags, 0, 0, 0) - 1",
        HAL_RAW,
    ),
    (
        "raw: recvfrom() of one byte",
        "src/hal_linux.c",
        "(long)len, flags, (long)from, (long)from_len",
        "1L, flags, (long)from, (long)from_len",
        HAL_RAW,
    ),
    (
        "raw: the wait without its timeout",
        "src/hal_linux.c",
        "RX_WAIT_FDS, (long)timeout_ts, 0, 0)",
        "RX_WAIT_FDS, 0L, 0, 0)",
        HAL_RAW,
    ),
]


def run_hal(binary, env=""):
    namespace = f"rxd-mut-{os.getpid()}"
    subprocess.run(["sudo", "-n", "ip", "netns", "add", namespace], check=True)
    try:
        subprocess.run(["sudo", "-n", "ip", "-n", namespace, "link", "set", "lo", "up"], check=True)
        subprocess.run(["sudo", "-n", "ip", "-n", namespace, "route", "add", "default", "dev", "lo"], check=True)
        # The inner `timeout 120` is the deadline that works: the outer one's kill cannot reach root-owned sudo.
        return run_capped(
            ["sudo", "-n", "ip", "netns", "exec", namespace, "runuser", "-u", os.environ.get("USER", "semih"), "--",
             "bash", "-c", f"ulimit -v 2097152; cd {ROOT / 'platform/linux'}; exec env {env} timeout 120 ./{binary}"],
            timeout=180,
        )
    finally:
        subprocess.run(["sudo", "-n", "ip", "netns", "del", namespace], check=False)


def targets_of(binary):
    """The make targets, relative to platform/linux, that a run of `binary` uses."""
    targets = [binary] if binary in (HAL, HAL_RAW, DRAIN) else [f"obj/debug/tests/{binary}"]
    if binary == DRAIN:
        targets.append("refuse_pbuf_ring.so")
    return targets


def built(binary):
    """Every file the build of `binary` made: a mutant is NOT_COMPILED only if all of them are the control's."""
    return [ROOT / "platform/linux" / target for target in targets_of(binary)]


def build_and_run(binary, control=None):
    targets = targets_of(binary)
    build = run_capped(["make", "-s", "-j4", *targets], timeout=900, cwd=ROOT / "platform/linux")
    if build.returncode != 0:
        return "BUILD_FAILED", build.output[-2000:]
    if unchanged(control, *built(binary)):
        return NOT_COMPILED, ""
    if binary == DRAIN:
        run = run_hal(binary)
        if run.returncode == 0:
            run = run_hal(binary, "TT_REFUSE_PBUF_RING=all TT_EXPECT_DRAIN_RING=classic "
                                  "LD_PRELOAD=./refuse_pbuf_ring.so")
    elif binary in (HAL, HAL_RAW):
        run = run_hal(binary)
    else:
        run = run_capped([str(TESTS / binary)], timeout=300)
    if run.timed_out or run.returncode == 124:  # 124: the namespace run's own `timeout 120`
        return "HANG", run.output[-1000:]
    if run.returncode is None or run.returncode < 0 or run.returncode in (134, 139):
        return "CRASH", run.output[-2000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.output


def main():
    be_nice()
    check_anchors(ROOT, [(name, path, old, new) for name, path, old, new, _binary in MUTANTS])
    files = sorted({m[1] for m in MUTANTS})
    only = os.environ.get("MUTANTS", "")
    failures = 0
    digests = {}
    with SourceGuard(ROOT, files) as guard:
        for binary in sorted({m[4] for m in MUTANTS}):
            verdict, output = build_and_run(binary)
            print(f"[control]  {binary:<20} unmutated -> {verdict}", flush=True)
            if verdict != "PASS":
                print(output[-2000:])
                print("control did not pass; the sweep cannot decide anything")
                return 1
            digests[binary] = code_digest(*built(binary))
        for name, path, old, new, binary in MUTANTS:
            if only and only not in name:
                continue
            (ROOT / path).write_text(guard.original(path).replace(old, new, 1))
            try:
                verdict, output = build_and_run(binary, digests[binary])
            finally:
                guard.restore()
            caught = verdict in ("FAIL", "CRASH", "HANG")  # BUILD_FAILED: the mutant itself is wrong
            print(f"[{label(verdict, caught, 'caught', 'MISSED')}]   {name:<84} {binary} -> {verdict}", flush=True)
            if caught:
                print(f"           killed by: {', '.join(killed_by(output, ROOT)) or '(no failing check printed)'}")
            else:
                print(output[-1500:])
            failures += 0 if caught else 1
    print(usage_line())
    print(f"{len(MUTANTS)} mutants, {failures} not caught" if failures else f"all {len(MUTANTS)} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

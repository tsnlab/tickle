#!/usr/bin/env python3
"""Mutant sweep for the wait ring (src/hal_linux.c, "The wait ring") against platform/linux/wr_check.c.

Each entry breaks one decision the ring rests on - who owns it, when it moves, which recvmsg is in flight into which
buffer, what each completion means, which member is polled - and wr_check must fail on it (exit 1). The mutant is
compiled into a copy of the HAL in a temporary directory; src/ is never edited. Every build is wr_check as
platform/linux/Makefile builds it (WR_CHECK_DEFS), the default build or rmw's (large samples: the writable watch), and
runs in a private network namespace (examples/perf_hil/experiments/wr_check_netns.sh), bounded by
tests/mutant_guard.py (2 GB, a deadline, 256 KB of output kept) as well as by the check's own alarm.

The controls first: both builds from the unmutated HAL must pass, or a failure below says nothing. A mutant whose
binary is the control's (tests/mutant_digest.py) is DEAD: it edited code that build leaves out, and counts as a
survivor.

Not mutated, and why - each decides no outcome wr_check can observe:
  - wr_completion()'s `if (!more)` re-arm of the multishot timer and bell polls: a multishot poll ends only on an error
    or an overflowed completion queue, which no check can bring about on demand.
  - wr_use()'s refusals while receive offload is on, a direct read is aimed or the drain runs: what they keep from the
    ring is merged datagrams (UDP_GRO's cmsg), a run read in place and the drain's order - udp_offload_check,
    rx_direct_check and rx_drain_check (test-linux, test-samehost) read through tt_receive() in those states, with the
    ring on where the build's default turns it on.
  - wr_wait()'s loop after completions that report nothing, and its clock for a wait with a timeout: what it decides
    is how long a wait woken only by stale completions goes on, a window no check can aim at.
  - wr_completion()'s recvmsg error left to the read after the wait (`cqe->res >= 0 ?`): the kernel fails a UDP read on
    an unconnected socket only for a bad buffer, the caller's misuse (survived the first sweep, 2026-10-22, as it must).
  - wr_wait()'s wr_arm() on every round of its loop rather than once: a single-edit mutant cannot move it out of the
    loop; "bell moved: the sleeping owner not nudged" covers the nudge it serves, and wr_check's check 8 hangs (alarm)
    when the round after a nudge does not arm the new bell (run by hand, 2026-10-22).
  - wr_sqe()'s full-ring refusal: the ring has room for every request one wait needs (WR_ENTRIES), so it decides
    nothing unless a submission fails.

Run from the repository root. Needs passwordless `sudo ip` (the namespace). MUTANTS=<substring> runs only the mutants
whose name contains it (the controls always run). Exits 2 before anything runs if an anchor is not in the HAL exactly
once.
"""
import os
import pathlib
import sys
import tempfile

from mutant_digest import code_digest, unchanged
from mutant_guard import be_nice, check_anchors, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
HAL = "src/hal_linux.c"
RUNNER = ROOT / "examples/perf_hil/experiments/wr_check_netns.sh"
WR_CHECK_DEFS = ["-DTT_RX_RING_MOVE_WAITS=8U", "-DTT_RX_RING_MOVE_MAX=64U"]  # platform/linux/Makefile's
LARGE_DEFINES = ["-Dtt_MAX_BUFFER_LENGTH=65507", "-Dtt_LARGE_SAMPLES=1"]
DEFAULT = "default"
LARGE = "large"

# (name, old, new, build that must fail)
MUTANTS = [
    ("owner: every thread waits on the ring",
     "    if (__builtin_expect(hal->wr_owner == self, 1)) {\n        hal->wr_streak = 0;",
     "    if (__builtin_expect(true, 1)) {\n        hal->wr_streak = 0;", DEFAULT),
    ("owner: no thread waits on the ring",
     "    if (__builtin_expect(hal->wr_owner == self, 1)) {\n        hal->wr_streak = 0;\n        return true;",
     "    if (__builtin_expect(hal->wr_owner == self, 1)) {\n        hal->wr_streak = 0;\n        return false;", DEFAULT),
    ("first waiter: the ring never enabled",
     "    if (hal->wr_owner == 0) {\n        return wr_enable(hal, self);",
     "    if (hal->wr_owner == 0) {\n        return false;", DEFAULT),
    ("move: never", "    if (++hal->wr_streak < hal->wr_move_after) {",
     "    if (++hal->wr_streak < UINT32_MAX) {", DEFAULT),
    ("move: the run a move needs not doubled", "        hal->wr_move_after *= 2U;", "        hal->wr_move_after *= 1U;",
     DEFAULT),
    ("move: a run of waits not reset by the owner's wait",
     "    if (__builtin_expect(hal->wr_owner == self, 1)) {\n        hal->wr_streak = 0;",
     "    if (__builtin_expect(hal->wr_owner == self, 1)) {\n        hal->wr_streak += 0;", DEFAULT),
    ("recvmsg: kept in flight for another buffer",
     "        if (hal->wr_recv_buf == hal->wr_buf && hal->wr_recv_len == hal->wr_len) {",
     "        if (true) {", DEFAULT),
    ("recvmsg: a new one beside the old, not cancelled",
     "        if (!wr_retire(hal, IORING_OP_ASYNC_CANCEL,",
     "        if (false && !wr_retire(hal, IORING_OP_ASYNC_CANCEL,", DEFAULT),
    ("recvmsg: a cancelled one's completion taken",
     "        if (gen != hal->wr_recv || gen == 0) {", "        if (gen == 0) {", DEFAULT),
    ("take: the ring's datagram read again from the socket",
     "    int32_t ret = taken ? wr_take(node, buf, ip, port) : rx_read_one(node, read_fd, buf, len, ip, port);",
     "    int32_t ret = false ? wr_take(node, buf, ip, port) : rx_read_one(node, read_fd, buf, len, ip, port);",
     DEFAULT),
    ("take: the sender's port not given", "    *port = ntohs(hal->wr_from.sin_port);", "    *port = 0;", DEFAULT),
    ("wake: a datagram read with a wake dropped for the wake",
     "        if ((ready & RX_READY_WAKE) != 0 && !taken) {", "        if ((ready & RX_READY_WAKE) != 0) {", DEFAULT),
    ("arm: the wake eventfd not polled",
     "    if ((hal->wr_armed & WR_ARMED_WAKE) == 0 && wr_poll(",
     "    if ((hal->wr_armed & WR_ARMED_WAKE) == 0 && false && wr_poll(", DEFAULT),
    ("arm: the well-known socket not polled",
     "    if ((hal->wr_armed & WR_ARMED_WELL_KNOWN) == 0 && wr_poll(",
     "    if ((hal->wr_armed & WR_ARMED_WELL_KNOWN) == 0 && false && wr_poll(", DEFAULT),
    ("arm: a one-shot poll never armed again",
     "    case WR_KIND_WAKE:\n        hal->wr_armed &= (uint8_t)~WR_ARMED_WAKE;",
     "    case WR_KIND_WAKE:\n        hal->wr_armed &= (uint8_t)~0U;", DEFAULT),
    ("arm: the deadline timer not polled", "    if ((hal->wr_armed & WR_ARMED_TIMER) == 0 && hal->timer_fd_plus1 > 0 &&",
     "    if ((hal->wr_armed & WR_ARMED_TIMER) == 0 && false &&", DEFAULT),
    ("arm: the bell not polled", "    if ((hal->wr_armed & WR_ARMED_BELL) == 0 && hal->bell_fd_plus1 > 0 &&",
     "    if ((hal->wr_armed & WR_ARMED_BELL) == 0 && false &&", DEFAULT),
    ("arm: a bell made again not polled", "    if (hal->wr_bell_epoch != bell_epoch) {", "    if (false) {",
     DEFAULT),
    ("bell moved: the sleeping owner not nudged", "        (void)!write(nudge, &one, sizeof(one));",
     "        (void)one;", DEFAULT),
    ("arm: the writable watch not polled", "    if ((hal->wr_armed & WR_ARMED_WATCH) == 0 && hal->wr_watch_fd_plus1 > 0 &&",
     "    if ((hal->wr_armed & WR_ARMED_WATCH) == 0 && false &&", LARGE),
    ("watch: the ring's watch set not given the watch", "        return wr_watch(hal, EPOLL_CTL_ADD, &event);",
     "        return true;", LARGE),
    ("EEXIST: a ring that is another thread's not made anew", "    if (err == EEXIST) {", "    if (false) {",
     DEFAULT),
    ("setup: DEFER_TASKRUN not asked for",
     "    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_R_DISABLED;",
     "    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED;", DEFAULT),
    ("setup: the switch ignored", "    if (hal->epoll_fd_plus1 == 0 || !wr_switch(node)) {",
     "    if (hal->epoll_fd_plus1 == 0) {", DEFAULT),
]


def build(hal_path, kind, out):
    sources = [ROOT / "platform/linux/wr_check.c", ROOT / "src/tickle.c", ROOT / "src/encoding.c", ROOT / "src/log.c",
               hal_path]
    argv = ["gcc", "-O0", "-g", "-Wall", "-Wextra", f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}", *WR_CHECK_DEFS,
            *(LARGE_DEFINES if kind == LARGE else []), "-o", str(out), *map(str, sources), "-lm", "-lpthread", "-ldl"]
    result = run_capped(argv, timeout=300, env={**os.environ, "TMPDIR": os.environ.get("TMPDIR", "/tmp")})
    if result.returncode != 0:
        print(result.output[-2000:])
    return result.returncode == 0


def run(binary):
    result = run_capped([str(RUNNER), str(binary)], timeout=240)
    said = [line for line in result.output.splitlines() if line.startswith("wr_check:")]
    return result.returncode, (said[0] if said else result.output.strip().splitlines()[-1:] or ["(no output)"])


def main():
    sys.stdout.reconfigure(line_buffering=True)  # a detached run's log is read while it runs
    be_nice()
    only = os.environ.get("MUTANTS")
    check_anchors(ROOT, [(name, HAL, old, new) for name, old, new, _ in MUTANTS])
    pristine = (ROOT / HAL).read_text()
    work = pathlib.Path(tempfile.mkdtemp(prefix="mutants_wait_ring_", dir=os.environ.get("TMPDIR")))
    binaries = {kind: work / f"wr_check_{kind}" for kind in (DEFAULT, LARGE)}
    control = {}
    for kind, binary in binaries.items():
        if not build(ROOT / HAL, kind, binary):
            print(f"FATAL: the control ({kind}) does not build")
            return 1
        code, said = run(binary)
        if code != 0:
            print(f"FATAL: the control ({kind}) does not pass: exit {code}: {said}")
            return 1
        print(f"PASS     control ({kind}): {said}")
        control[kind] = code_digest(binary)
    survivors = 0
    ran = 0
    for name, old, new, kind in MUTANTS:
        if only and only not in name:
            continue
        ran += 1
        mutant = work / "hal_linux.c"
        mutant.write_text(pristine.replace(old, new))
        binary = binaries[kind]
        if not build(mutant, kind, binary):
            print(f"FATAL: mutant '{name}' does not build")
            return 1
        if unchanged(control[kind], binary):
            survivors += 1
            print(f"DEAD     {name}: NOT_COMPILED - its binary is the control's ({kind})")
            continue
        code, said = run(binary)
        caught = code == 1
        survivors += 0 if caught else 1
        print(f"{'CAUGHT ' if caught else 'SURVIVED'} {name} ({kind}): exit {code}: {said}")
    print(f"{ran - survivors} of {ran} mutants caught")
    print(usage_line())
    return 0 if survivors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Mutant sweep for tt_receive()'s wait on io_uring (src/hal_linux.c, "One wait that returns data").

Each entry breaks one condition the wait relies on and names the check that must go red: platform/linux/rx_uring_check.c,
built as rmw_tickle builds core (the only build with the wait), run in a private network namespace of its own. A
mutant that still passes means the check does not test what it says, or the condition decides nothing and should not be
in the code.

Not mutated, and why - each decides no outcome a single-threaded check on loopback can observe:
  - wait_before_read()'s cancellation of a recvmsg armed on the socket about to be read by a system call: no path reads
    such a socket (tt_try_receive() skips it, tt_receive() reads only what the wait reported); it is the backstop that
    keeps one reader per socket if one ever did.
  - tt_close()'s cancellation before the ring is closed: what it prevents is the kernel's exit work completing a
    recvmsg into a context already freed - after the check has gone.
  - gro_maybe_settle()'s cancellation before the option goes off (C2): the window is a datagram merged before the
    setsockopt() and taken after it by a recvmsg already armed - the recvmsg's task work runs at the exit of the very
    system call that queued the datagram on loopback, so a single thread cannot open it.
  - wait_ready()'s rule that a poll on a socket the ring reads says nothing: such a poll is armed only by
    tt_rx_maybe_ready() on a socket with no recvmsg, and its completion arrives with the recvmsg's, never alone, in
    every run a check can make.
  - wait_ring_setup()'s refusals (no epoll set, no EXT_ARG, the probe, no multishot poll): this kernel has all of them;
    the refused case (refuse_io_uring.so) checks the path they lead to.

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
CHECK = "rx_uring_check"

# (name, file, old, new, binary that must fail)
MUTANTS = [
    (
        "wait: never on the ring (the dispatch removed)",
        "src/hal_linux.c",
        "    if (node->hal.wait_on) {\n        return rx_wait_ring(node, timeout_ts, ready);",
        "    if (false) {\n        return rx_wait_ring(node, timeout_ts, ready);",
        CHECK,
    ),
    (
        "setup: TT_RX_URING_WAIT=0 ignored (no control arm)",
        "src/hal_linux.c",
        '    if (setting != NULL && strcmp(setting, "0") == 0) {\n        hal->wait_state = 0;',
        '    if (false) {\n        hal->wait_state = 0;',
        CHECK,
    ),
    (
        "on: the sockets left in the epoll set",
        "src/hal_linux.c",
        "    if (epoll_ctl(hal->epoll_fd_plus1 - 1, EPOLL_CTL_DEL, hal->sock, NULL) != 0 ||\n"
        "        epoll_ctl(hal->epoll_fd_plus1 - 1, EPOLL_CTL_DEL, hal->data_sock, NULL) != 0) {",
        "    if (false) {",
        CHECK,
    ),
    (
        "note: a large sample's datagram does not put the wait on the ring",
        "src/hal_linux.c",
        "        if (!hal->wait_on) {\n            wait_ring_on(node);",
        "        if (false) {\n            wait_ring_on(node);",
        CHECK,
    ),
    (
        "note: small datagrams never counted (the wait stays on the ring)",
        "src/hal_linux.c",
        "        hal->wait_quiet++;",
        "",
        CHECK,
    ),
    (
        "off: the sockets not put back in the epoll set",
        "src/hal_linux.c",
        "    wait_sockets_to_epoll(node);\n    hal->wait_on = false;",
        "    hal->wait_on = false;",
        CHECK,
    ),
    (
        "setup: the epoll fd's poll left armed while the sockets are in the epoll set",
        "src/hal_linux.c",
        "    wait_epoll_cancel(hal); // armed when the wait goes on the ring",
        "    // armed when the wait goes on the ring",
        CHECK,
    ),
    (
        "look: a recvmsg's datagram not held",
        "src/hal_linux.c",
        "                hal->wait_held |= (uint8_t)(1U << socket);",
        "",
        CHECK,
    ),
    (
        "look: the epoll fd's report dropped",
        "src/hal_linux.c",
        "            hal->wait_epoll_seen = true;\n",
        "",
        CHECK,
    ),
    (
        "read: a datagram the ring holds not handed out (read by a system call instead)",
        "src/hal_linux.c",
        "    if (node->hal.wait_on && wait_before_read(node, socket_fd, buf)) {",
        "    if (false && wait_before_read(node, socket_fd, buf)) {",
        CHECK,
    ),
    (
        "prepare: a recvmsg armed into another buffer left armed",
        "src/hal_linux.c",
        "    if (hal->wait_target[1] != buf || hal->wait_target_len[1] != len) {\n        wait_cancel(",
        "    if (false) {\n        wait_cancel(",
        CHECK,
    ),
    (
        "yield: another datagram put in the buffer the data socket's recvmsg reads into",
        "src/hal_linux.c",
        "    if ((hal->wait_recv_armed & WAIT_BIT_DATA) != 0) {\n        wait_cancel(node, WAIT_BIT_DATA);\n    }\n"
        "    if ((hal->wait_held & WAIT_BIT_DATA) != 0",
        "    if ((hal->wait_held & WAIT_BIT_DATA) != 0",
        CHECK,
    ),
    (
        "yield: the data socket's datagram, held in the buffer, overwritten by another",
        "src/hal_linux.c",
        "    if ((hal->wait_held & WAIT_BIT_DATA) != 0 && hal->wait_target[1] == buf) {",
        "    if (false) {",
        CHECK,
    ),
    (
        "try: a socket the ring reads asked by the drain",
        "src/hal_linux.c",
        "        if ((node->hal.wait_recv_armed & ring_bit) != 0 && (node->hal.wait_held & ring_bit) == 0) {",
        "        if (false) {",
        CHECK,
    ),
    (
        "drain: started with the wait's recvmsgs armed",
        "src/hal_linux.c",
        "    wait_cancel(node, hal->wait_recv_armed);\n    if (hal->wait_held != 0) {\n        return false;",
        "    if (hal->wait_held != 0) {\n        return false;",
        CHECK,
    ),
    (
        "C2: the recvmsg without room for UDP_GRO's segment size",
        "src/hal_linux.c",
        "    msg->msg_control = area->control[socket].bytes;\n    msg->msg_controllen = sizeof(area->control[socket].bytes);",
        "",
        CHECK,
    ),
    (
        "C2: receive offload on read by readiness, as in C1",
        "src/hal_linux.c",
        "    if (node->hal.gro_state == TT_GRO_SETTLING) {\n        return false;",
        "    if (node->hal.gro_state != TT_GRO_PLAIN) {\n        return false;",
        CHECK,
    ),
    (
        "C2: a merged read the ring took handed out as one datagram",
        "src/hal_linux.c",
        "    if (segment > 0 && res > segment) {",
        "    if (false) {",
        CHECK,
    ),
    (
        "hint: an armed recvmsg not counted as covering its socket",
        "src/hal_linux.c",
        "    const uint8_t covered = (uint8_t)(hal->uring_armed | hal->wait_recv_armed);",
        "    const uint8_t covered = hal->uring_armed;",
        CHECK,
    ),
]


def run_hal(binary, env=""):
    namespace = f"rxu-mut-{os.getpid()}"
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
    build = subprocess.run(["make", "-s", "-C", str(ROOT / "platform/linux"), binary], capture_output=True, text=True)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        run = run_hal(binary)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode < 0 or run.returncode in (124, 134, 139):
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    files = sorted({m[1] for m in MUTANTS})
    backup = pathlib.Path(tempfile.mkdtemp(prefix="rx_uring_mutants_"))
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

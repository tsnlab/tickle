#!/usr/bin/env python3
"""Mutant sweep for traffic classes and send time (network profiles items D and E; DESIGN.md section 2, "Traffic
classes and send time"): tt_tx_class() and the resend marks in src/tickle.c, the socket and per-send marks in
src/hal_linux.c, and the options' defaults and checks.

Each mutant breaks one decision and must turn tests/test_tx_class.c (mock HAL: what core sends is of which class, and
publish_at's time) or tests/test_socket_marks.c (the real HAL: what reaches setsockopt() and each send's control
messages) red. Both are built from a mutated COPY in a temporary directory (-DTICKLE_C / -DHAL_LINUX_C), so nothing in
src/ is edited and nothing needs restoring. The controls - both tests from the unmutated sources - must pass first.
A mutant whose two binaries, debug sections aside, are the control's (tests/mutant_digest.py) is DEAD (NOT_COMPILED):
it edited code these builds leave out, and counts as not caught.

Not mutated, and why:
  - send_batch_flags()'s copy without send offload (a build without large samples): neither test compiles it. Its
    marks are one call to tx_marks_put(), the function the offload path's mutants below break.
  - the warnings: a log line decides nothing a test can observe.
  - tx_priority_probe()'s "anything else is past the control messages": on this PC the probe always reaches the route
    (127.0.0.1); the answers that matter, EINVAL and EPERM, are each made below.

Run from the repository root. Children are bounded by tests/mutant_guard.py (2 GB, a deadline, capped output).
MUTANTS=<substring> runs only the mutants whose name contains it (the controls always run).
"""
import os
import pathlib
import sys
import tempfile

from mutant_digest import NOT_COMPILED, code_digest, label
from mutant_guard import be_nice, check_anchors, killed_by, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
LINUX = ROOT / "platform/linux"
TICKLE = "src/tickle.c"
HAL = "src/hal_linux.c"
TESTS = ["test_tx_class", "test_socket_marks"]
TIMEOUT_S = 120

# (name, file, old, new)
MUTANTS = [
    # ---- tickle.c: the classifier.
    ("classify: a HEARTBEAT is a user's", TICKLE,
     "    case tt_SUBMESSAGE_TYPE_HEARTBEAT:\n    case tt_SUBMESSAGE_TYPE_ACKNACK:\n        return TX_KIND_CONTROL;",
     "    case tt_SUBMESSAGE_TYPE_ACKNACK:\n        return TX_KIND_CONTROL;"),
    ("classify: an ACKNACK is a user's", TICKLE,
     "    case tt_SUBMESSAGE_TYPE_HEARTBEAT:\n    case tt_SUBMESSAGE_TYPE_ACKNACK:\n        return TX_KIND_CONTROL;",
     "    case tt_SUBMESSAGE_TYPE_HEARTBEAT:\n        return TX_KIND_CONTROL;"),
    ("classify: a discovery DATA or FRAG_FIRST is a user's", TICKLE,
     "            _tt_memcpy(&entity_id, body + offsetof(struct tt_DataHeader, entity_id), sizeof(entity_id));\n"
     "        }\n        return entity_id == tt_DISCOVERY_ENTITY_ID ? TX_KIND_CONTROL : TX_KIND_USER;",
     "            _tt_memcpy(&entity_id, body + offsetof(struct tt_DataHeader, entity_id), sizeof(entity_id));\n"
     "        }\n        return TX_KIND_USER;"),
    ("classify: a discovery FRAG_CONT is a user's", TICKLE,
     "            _tt_memcpy(&entity_id, body + offsetof(struct tt_FragContHeader, entity_id), sizeof(entity_id));\n"
     "        }\n        return entity_id == tt_DISCOVERY_ENTITY_ID ? TX_KIND_CONTROL : TX_KIND_USER;",
     "            _tt_memcpy(&entity_id, body + offsetof(struct tt_FragContHeader, entity_id), sizeof(entity_id));\n"
     "        }\n        return TX_KIND_USER;"),
    ("classify: a call request's retry count ignored", TICKLE,
     "                       body[offsetof(struct tt_CallRequestHeader, retry)] != 0",
     "                       body[offsetof(struct tt_CallRequestHeader, retry)] != 0 && false"),
    ("classify: a call response's retry count ignored", TICKLE,
     "                       body[offsetof(struct tt_CallResponseHeader, retry)] != 0",
     "                       body[offsetof(struct tt_CallResponseHeader, retry)] != 0 && false"),
    ("classify: the single form not recognised", TICKLE,
     "        (head[0] == tt_SINGLE_MARKER_LE || head[0] == tt_SINGLE_MARKER_BE)) {\n        return submessage_tx_kind(",
     "        false) {\n        return submessage_tx_kind("),
    ("classify: the classic form takes the least important submessage", TICKLE,
     "        kind = this_kind > kind ? this_kind : kind;",
     "        kind = this_kind < kind ? this_kind : kind;"),
    ("classify: a fence or bell is a user's", TICKLE,
     "    if (head_len == 0 || (head_len == tt_SEGMENT_FENCE_LENGTH && head[0] == tt_SEGMENT_FENCE_MARKER)) {\n"
     "        return TX_KIND_CONTROL;",
     "    if (head_len == 0 || (head_len == tt_SEGMENT_FENCE_LENGTH && head[0] == tt_SEGMENT_FENCE_MARKER)) {\n"
     "        return TX_KIND_USER;"),
    ("classify: core's resend mark ignored", TICKLE,
     "    return kind == TX_KIND_RETRY || node->tx_class == tt_TX_CLASS_REPAIR ? tt_TX_CLASS_REPAIR : tt_TX_CLASS_DATA;",
     "    return kind == TX_KIND_RETRY ? tt_TX_CLASS_REPAIR : tt_TX_CLASS_DATA;"),
    ("classify: control while resending is REPAIR", TICKLE,
     "    if (kind == TX_KIND_CONTROL) {\n        return tt_TX_CLASS_CONTROL;\n    }\n    return kind == TX_KIND_RETRY",
     "    if (kind == TX_KIND_CONTROL && node->tx_class != tt_TX_CLASS_REPAIR) {\n        return tt_TX_CLASS_CONTROL;\n"
     "    }\n    return kind == TX_KIND_RETRY"),
    # ---- tickle.c: the resend marks and the send time.
    ("mark: a NACKed resend not marked", TICKLE,
     "    const uint8_t outer_class = tx_class_enter(node, tt_TX_CLASS_REPAIR); // what follows is resent",
     "    const uint8_t outer_class = tx_class_enter(node, tt_TX_CLASS_DATA); // what follows is resent"),
    ("mark: a durability backlog not marked", TICKLE,
     "    const uint8_t outer_class = tx_class_enter(node, tt_TX_CLASS_REPAIR); // a backlog is resent",
     "    const uint8_t outer_class = tx_class_enter(node, tt_TX_CLASS_DATA); // a backlog is resent"),
    ("mark: never left", TICKLE,
     "    node->tx_class = outer;", "    (void)outer;"),
    ("publish_at: the time not set", TICKLE,
     "    node->tx_time_ns = tx_time_ns;\n    tt_ret_t result = tt_Publisher_publish(pub, data);",
     "    (void)tx_time_ns;\n    tt_ret_t result = tt_Publisher_publish(pub, data);"),
    ("publish_at: the time not cleared", TICKLE,
     "    tt_ret_t result = tt_Publisher_publish(pub, data);\n    node->tx_time_ns = 0;",
     "    tt_ret_t result = tt_Publisher_publish(pub, data);"),
    # ---- tickle.c: the options.
    ("option: txtime's default on", TICKLE,
     "    if (socket->txtime == tt_SWITCH_DEFAULT) {\n        socket->txtime = tt_SWITCH_OFF;",
     "    if (socket->txtime == tt_SWITCH_DEFAULT) {\n        socket->txtime = tt_SWITCH_ON;"),
    ("check: a DSCP above 63 taken", TICKLE,
     "            return reject_option(\"socket.tx_class[].dscp\"", "            (void)reject_option(\"socket.tx_class[].dscp\""),
    ("check: a DSCP of 63 refused", TICKLE,
     "        if (socket->tx_class[tx_class].dscp > tt_TX_DSCP_MAX) {",
     "        if (socket->tx_class[tx_class].dscp >= tt_TX_DSCP_MAX) {"),
    ("check: a txtime that is no switch taken", TICKLE,
     "        return reject_option(\"socket.txtime\"", "        (void)reject_option(\"socket.txtime\""),
    # ---- hal_linux.c: the socket.
    ("socket: DATA's DSCP not set", HAL,
     "    if (data->dscp != 0 && setsockopt(hal->data_sock, IPPROTO_IP, IP_TOS,",
     "    if (false && setsockopt(hal->data_sock, IPPROTO_IP, IP_TOS,"),
    ("socket: the priority left to the TOS", HAL,
     "    if ((data->dscp != 0 || data->priority != 0) &&", "    if ((data->priority != 0) &&"),
    ("socket: no setup at all", HAL, "    tx_marks_setup(node);\n", "\n"),
    # ---- hal_linux.c: which classes go per datagram.
    ("differs: the TOS not compared", HAL,
     "        hal->tx_differs[cls] = hal->tx_tos[cls] != hal->tx_tos[0] || hal->tx_priority[cls] != hal->tx_priority[0];",
     "        hal->tx_differs[cls] = hal->tx_priority[cls] != hal->tx_priority[0];"),
    ("differs: the priority not compared", HAL,
     "        hal->tx_differs[cls] = hal->tx_tos[cls] != hal->tx_tos[0] || hal->tx_priority[cls] != hal->tx_priority[0];",
     "        hal->tx_differs[cls] = hal->tx_tos[cls] != hal->tx_tos[0];"),
    ("differs: no mark", HAL,
     "            hal->tx_marks |= TT_TX_MARK_CLASS;", "            (void)0;"),
    ("probe: the kernel's answer ignored", HAL,
     "    int refused = *unknown ? EINVAL : tx_priority_probe(hal->data_sock, hal->tx_priority[cls]);",
     "    int refused = 0;"),
    ("probe: a refused priority still sent", HAL,
     "    hal->tx_priority[cls] = hal->tx_priority[0];\n}", "}"),
    # ---- hal_linux.c: the per-send control messages.
    ("send: no IP_TOS", HAL,
     "            end = tx_cmsg_put(control, end, IPPROTO_IP, IP_TOS, &hal->tx_tos[tx_class], sizeof(int));",
     "            (void)0;"),
    ("send: no SO_PRIORITY", HAL, "            if (hal->tx_priority_cmsg) {\n                // NOLINTNEXTLINE",
     "            if (false) {\n                // NOLINTNEXTLINE"),
    ("send: SO_PRIORITY where the kernel takes none", HAL,
     "            if (hal->tx_priority_cmsg) {\n                // NOLINTNEXTLINE",
     "            if (true) {\n                // NOLINTNEXTLINE"),
    ("send: every class's own marks", HAL, "        if (hal->tx_differs[tx_class]) {",
     "        if (true) {"),
    ("send: tt_send() unmarked", HAL,
     "        int32_t marked = sendto_marked(node, &again); // a class's or a time's control messages",
     "        int32_t marked = (int32_t)hal_sendto(again.fd, buf, len, 0, (const struct sockaddr*)&again.to, "
     "sizeof(again.to));"),
    ("send: tt_send_to() unmarked", HAL,
     "        int32_t marked = sendto_marked(node, &again); // as tt_send()",
     "        int32_t marked = (int32_t)hal_sendto(again.fd, buf, len, 0, (const struct sockaddr*)&again.to, "
     "sizeof(again.to));"),
    ("send: tt_send_iov() unmarked", HAL,
     "        return sendmsg_marked(node, &again, hdr, hdr_len); // as tt_send()",
     "        return (int32_t)hal_sendmsg(again.fd, msg, 0);"),
    ("send: a batch's messages unmarked", HAL,
     "        tx_marks_put(node, &msg->msg_hdr, control->bytes, msg->msg_hdr.msg_controllen, first->head, first->head_len,",
     "        (void)node;\n        if (false) tx_marks_put(node, &msg->msg_hdr, control->bytes, "
     "msg->msg_hdr.msg_controllen, first->head, first->head_len,"),
    # ---- hal_linux.c: the send time.
    ("txtime: never asked", HAL, "    if (node->opts.socket.txtime != tt_SWITCH_ON) {", "    if (true) {"),
    ("txtime: a refusal still marks", HAL,
     "    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_TXTIME, &txtime, sizeof(txtime)) == 0) {",
     "    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_TXTIME, &txtime, sizeof(txtime)) == 0 || true) {"),
    ("txtime: the time not sent", HAL,
     "    if ((hal->tx_marks & TT_TX_MARK_TIME) != 0 && node->tx_time_ns != 0) {", "    if (false) {"),
    ("txtime: a time of 0 sent", HAL,
     "    if ((hal->tx_marks & TT_TX_MARK_TIME) != 0 && node->tx_time_ns != 0) {",
     "    if ((hal->tx_marks & TT_TX_MARK_TIME) != 0) {"),
    ("txtime: CLOCK_MONOTONIC", HAL, "    struct sock_txtime txtime = {.clockid = CLOCK_TAI, .flags = 0};",
     "    struct sock_txtime txtime = {.clockid = CLOCK_MONOTONIC, .flags = 0};"),
]


def build(work, test, tickle_c, hal_c):
    """Builds tests/<test>.c against the given sources into work/<test>; the build's Result."""
    argv = ["gcc", "-O0", "-g", "-Wall", "-Wextra", f"-I{ROOT}/include", f"-I{ROOT}/src", f"-I{ROOT}/tests",
            f"-DTICKLE_C=\"{tickle_c}\"", f"-DHAL_LINUX_C=\"{hal_c}\"", "-o", str(work / test),
            str(ROOT / "tests" / f"{test}.c"), str(ROOT / "src/encoding.c"), str(ROOT / "src/log.c"), "-lm"]
    return run_capped(argv, timeout=600, cwd=LINUX)


def build_and_run(work, tickle_c, hal_c, control=None):
    """(verdict, output): PASS when both tests pass, FAIL when either fails, NOT_COMPILED, BUILD_FAILED."""
    for test in TESTS:
        result = build(work, test, tickle_c, hal_c)
        if result.returncode != 0:
            return "BUILD_FAILED", result.output[-2000:]
    if control is not None and code_digest(*(work / t for t in TESTS)) == control:
        return NOT_COMPILED, ""
    output = []
    failed = False
    for test in TESTS:
        run = run_capped([str(work / test)], timeout=TIMEOUT_S, cwd=LINUX)
        lines = [line for line in run.output.splitlines() if not line.startswith("[")]
        output.append(f"-- {test}: exit {run.returncode}{' (hung)' if run.timed_out else ''}\n" + "\n".join(lines))
        failed = failed or run.returncode != 0 or run.timed_out
    return ("FAIL" if failed else "PASS"), "\n".join(output)


def main():
    be_nice()
    check_anchors(ROOT, MUTANTS)
    failures = 0
    with tempfile.TemporaryDirectory(prefix="mutants_socket_marks.") as tmp:
        work = pathlib.Path(tmp)
        verdict, output = build_and_run(work, ROOT / TICKLE, ROOT / HAL)
        print(f"[control]  both tests, unmutated sources                        -> {verdict}", flush=True)
        if verdict != "PASS":
            print(output[-3000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1
        control = code_digest(*(work / t for t in TESTS))
        only = os.environ.get("MUTANTS", "")
        for name, rel, old, new in MUTANTS:
            if only and only not in name:
                continue
            mutant = work / pathlib.Path(rel).name
            mutant.write_text((ROOT / rel).read_text().replace(old, new, 1))
            tickle_c = mutant if rel == TICKLE else ROOT / TICKLE
            hal_c = mutant if rel == HAL else ROOT / HAL
            verdict, output = build_and_run(work, tickle_c, hal_c, control)
            mutant.unlink()
            ok = verdict == "FAIL"
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:62s} -> {verdict}", flush=True)
            if ok:
                print(f"  killed by: {', '.join(killed_by(output, ROOT)) or '(no failing check printed)'}")
            else:
                print(output[-1500:])
    print(usage_line())
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

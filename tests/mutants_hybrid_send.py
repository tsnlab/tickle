#!/usr/bin/env python3
"""Mutant sweep for the hybrid transport's sending half (DESIGN.md section 8, "Hybrid transport", steps 3 to 5).

Each entry breaks exactly one condition the sender relies on - when a sample may go in runs, how n is bounded, where a
run ends, what a repair is, how loss and cost are counted - and names the test binary that must go red for it. A mutant
that still passes means either the test does not test what it says, or the condition decides nothing and should not be
in the code.

Run from the repository root. Restores every file it touches, whatever happens. MUTANTS=<substring> runs only the
mutants whose name contains it (the controls always run).
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
SEND = "test_hybrid_send"
SHM = "test_large_shm"

# (name, file, old, new, test binary that must fail)
MUTANTS = [
    (
        "runs: from two units, not three",
        "src/tickle.c",
        "        if (pub != NULL && count >= 3U) {",
        "        if (pub != NULL && count >= 2U) {",
        SEND,
    ),
    (
        "small runs: the last unit inside a run",
        "src/tickle.c",
        "    for (uint32_t start = 0; start + 1U < count; start += n) {\n"
        "        uint32_t units = count - 1U - start < n ? count - 1U - start : n;",
        "    for (uint32_t start = 0; start < count; start += n) {\n"
        "        uint32_t units = count - start < n ? count - start : n;",
        SEND,
    ),
    (
        "small runs: each run named by the sample's seq_no, not its first unit's",
        "src/tickle.c",
        "    run->data.seq_no = data_header->seq_no + start; // the first unit's own",
        "    run->data.seq_no = data_header->seq_no; // the first unit's own",
        SEND,
    ),
    (
        "small runs: the tail not sent",
        "src/tickle.c",
        "    return hybrid_send_batch(node, &last, 1, &dest->context_id, 1) >= 0;",
        "    return true;",
        SEND,
    ),
    (
        "large runs: the last unit inside a run",
        "src/tickle.c",
        "    for (; k < LARGE_BATCH && next < last; k++) {\n        uint32_t run = last - next < n ? last - next : n;",
        "    for (; k < LARGE_BATCH && next <= last; k++) {\n"
        "        uint32_t run = last + 1U - next < n ? last + 1U - next : n;",
        SEND,
    ),
    (
        "large runs: each run named by the sample's seq_no",
        "src/tickle.c",
        "        header_l->data.seq_no = data->seq_no + next; // the first unit's own",
        "        header_l->data.seq_no = data->seq_no; // the first unit's own",
        SEND,
    ),
    (
        "large runs: the cursor moves by datagrams, not units",
        "src/tickle.c",
        "            moved += units[k];",
        "            moved += 1U;",
        SEND,
    ),
    (
        "n_max: the peer's ceiling ignored",
        "src/tickle.c",
        "    uint32_t units = ceiling > framing ? (ceiling - framing) / unit : 0U;",
        "    uint32_t units = (uint32_t)tt_HYBRID_N_MAX;",
        SEND,
    ),
    (
        "n_max: the Publisher's ip_datagram_max ignored",
        "src/tickle.c",
        "    if (pub->ip_datagram_max != 0 && pub->ip_datagram_max < ceiling) {",
        "    if (false) {",
        SEND,
    ),
    (
        "n_max: a segment peer gets runs",
        "src/tickle.c",
        "    if (n_max > 1U && peer_segment(node, context_id, ip, port) != NULL) {",
        "    if (false) {",
        SHM,
    ),
    (
        "mode: MTU ignored",
        "src/tickle.c",
        "    if (n_max < 2U || mode == tt_LARGE_FRAGMENTATION_MTU) {",
        "    if (n_max < 2U) {",
        SEND,
    ),
    (
        "mode: a Publisher's override ignored",
        "src/tickle.c",
        "    return pub->large_fragmentation != tt_LARGE_FRAGMENTATION_INHERIT ? pub->large_fragmentation\n"
        "                                                                      : node->large_fragmentation;",
        "    UNUSED(pub);\n    return node->large_fragmentation;",
        SEND,
    ),
    (
        "mode: RELIABLE AUTO sent n_max as best effort is",
        "src/tickle.c",
        "    if (mode == tt_LARGE_FRAGMENTATION_IP || !pub->reliable) {",
        "    if (true) {",
        SEND,
    ),
    (
        "broadcast: the largest n over the peers, not the smallest",
        "src/tickle.c",
        "        smallest = smallest == 0 || n < smallest ? n : smallest;",
        "        smallest = smallest == 0 || n > smallest ? n : smallest;",
        SEND,
    ),
    (
        "unicast: every destination given the broadcast's minimum",
        "src/tickle.c",
        "    uint32_t n_max = 1U;\n    if (context_id != tt_CONTEXT_ID_INVALID) {",
        "    uint32_t n_max = 1U;\n    if (false) {",
        SEND,
    ),
    (
        "counters: a run's units not counted",
        "src/tickle.c",
        "        hybrid->run_units += count - 1U;",
        "",
        SEND,
    ),
    # --- RELIABLE: the cost model and the writer's count of each reader's loss ---
    (
        "cost model: no loss read as MTU",
        "src/tickle.c",
        "    if (p <= 0.0) {\n        return n_max;",
        "    if (p <= 0.0) {\n        return 1U;",
        SEND,
    ),
    (
        "cost model: the datagram cost not shared over the run",
        "src/tickle.c",
        "        double cost = (c_d / (double)n) + c_f + ((1.0 - clean) * repair);",
        "        double cost = c_d + c_f + ((1.0 - clean) * repair);",
        SEND,
    ),
    (
        "cost model: a lost run's repairs not counted",
        "src/tickle.c",
        "        double cost = (c_d / (double)n) + c_f + ((1.0 - clean) * repair);",
        "        double cost = (c_d / (double)n) + c_f;",
        SEND,
    ),
    (
        "cost model: the receiver's costs left out",
        "src/tickle.c",
        "    *c_d = (double)tt_HYBRID_TX_DATAGRAM_NS + (double)tt_HYBRID_RX_DATAGRAM_NS;",
        "    *c_d = (double)tt_HYBRID_TX_DATAGRAM_NS;",
        SEND,
    ),
    (
        "reliable n: the most optimistic reader's p",
        "src/tickle.c",
        "            p = peer_p > p ? peer_p : p;",
        "            p = p < 0.0 || peer_p < p ? peer_p : p;",
        SEND,
    ),
    (
        "reliable n: a node with no reader of this Publisher read as lossless",
        "src/tickle.c",
        "    if (p < 0.0) {\n        return 1U;\n    }",
        "    if (p < 0.0) {\n        p = 0.0;\n    }",
        SEND,
    ),
    (
        "exposure: the frames sent not counted",
        "src/tickle.c",
        "            loss_estimate_observe(&ack->loss, count, 0);\n            ack->run_n = (uint8_t)n;",
        "            ack->run_n = (uint8_t)n;",
        SEND,
    ),
    (
        "exposure: n not recorded",
        "src/tickle.c",
        "            loss_estimate_observe(&ack->loss, count, 0);\n            ack->run_n = (uint8_t)n;",
        "            loss_estimate_observe(&ack->loss, count, 0);",
        SEND,
    ),
    (
        "exposure: given to every reader, not the one sent to",
        "src/tickle.c",
        "            (context_id == tt_CONTEXT_ID_INVALID || ack->context_id == context_id)) {",
        "            (context_id == tt_CONTEXT_ID_INVALID || true)) {",
        SEND,
    ),
    (
        "exposure: counted for a reader that could not take runs",
        "src/tickle.c",
        "        if (capable) {\n            hybrid_note_sent(node, pub, destinations[i].context_id, n, record->count);",
        "        if (true) {\n            hybrid_note_sent(node, pub, destinations[i].context_id, n, record->count);",
        SEND,
    ),
    (
        "nacks: counted for a reader never exposed",
        "src/tickle.c",
        "    if (ack == NULL || ack->loss.frames == 0) {",
        "    if (ack == NULL) {",
        SEND,
    ),
    (
        "nacks: every first NACK its own event",
        "src/tickle.c",
        "                if (range != 0 && seq_no == previous + 1U) {",
        "                if (false) {",
        SEND,
    ),
    (
        "nacks: a range's runs rounded down",
        "src/tickle.c",
        "    events += (range + n - 1U) / n;\n    if (nacked == 0) {",
        "    events += range / n;\n    if (nacked == 0) {",
        SEND,
    ),
    (
        "nacks: a re-NACK counted with the repair still in flight",
        "src/tickle.c",
        "            } else if (renack_counts) {",
        "            } else {",
        SEND,
    ),
    (
        "nacks: a re-NACK never counted",
        "src/tickle.c",
        "            } else if (renack_counts) {",
        "            } else if (false) {",
        SEND,
    ),
    (
        "nacks: the highest NACK not kept",
        "src/tickle.c",
        "    ack->nack_high = high;",
        "",
        SEND,
    ),
    (
        "nacks: the time of the last NACK not kept",
        "src/tickle.c",
        "    ack->nack_ns = now;",
        "",
        SEND,
    ),
    (
        "nacks: the repairs not counted as exposure",
        "src/tickle.c",
        "    loss_estimate_observe(&ack->loss, nacked, events);",
        "    loss_estimate_observe(&ack->loss, 0, events);",
        SEND,
    ),
    (
        "nacks: an ACKNACK's NACKs not read",
        "src/tickle.c",
        "        hybrid_note_nacks(pub, header->source, sender_entity_id, seq_no, bitmap, bitmap_words);",
        "",
        SEND,
    ),
    # --- the reader's pooled loss and cap ---
    (
        "reader: a gap not an event",
        "src/tickle.c",
        "    loss_estimate_observe(&hybrid->rx_loss, (index + units) - writer->next, index > writer->next ? 1U : 0U);",
        "    loss_estimate_observe(&hybrid->rx_loss, (index + units) - writer->next, 0U);",
        SEND,
    ),
    (
        "reader: a sample left short not an event",
        "src/tickle.c",
        "            loss_estimate_observe(&hybrid->rx_loss, writer->count - writer->next, 1); // its tail never came",
        "            loss_estimate_observe(&hybrid->rx_loss, writer->count - writer->next, 0); // its tail never came",
        SEND,
    ),
    (
        "reader: a writer first met mid-sample read as a gap",
        "src/tickle.c",
        "        *writer = (struct tt_HybridRxWriter) {entity_id, sample, (uint16_t)count, (uint16_t)index, source};",
        "        *writer = (struct tt_HybridRxWriter) {entity_id, sample, (uint16_t)count, 0, source};",
        SEND,
    ),
    (
        "reader: an older sample's unit taken as a new sample",
        "src/tickle.c",
        "    if ((int32_t)(sample - writer->sample) > 0) {",
        "    if (sample != writer->sample) {",
        SEND,
    ),
    (
        "reader: U counts arrivals, not the units sent",
        "src/tickle.c",
        "    hybrid->tick_units += (index + units) - writer->next;",
        "    hybrid->tick_units += units;",
        SEND,
    ),
    (
        "reader: the window never halves",
        "src/tickle.c",
        "    if (estimate->frames >= (uint32_t)tt_HYBRID_WINDOW_FRAMES) {",
        "    if (false) {",
        SEND,
    ),
    (
        "bound: the point estimate, not the upper limit",
        "src/tickle.c",
        "    double p = loss_poisson_upper(estimate->events) / (double)estimate->frames;",
        "    double p = (double)estimate->events / (double)estimate->frames;",
        SEND,
    ),
    (
        "bound: Wilson-Hilferty without its skew term",
        "src/tickle.c",
        "    double cube = 1.0 - (1.0 / (9.0 * k1)) + (LOSS_BOUND_Z / (3.0 * root));",
        "    double cube = 1.0 + (LOSS_BOUND_Z / (3.0 * root));",
        SEND,
    ),
    (
        "bound: no exposure reads as no loss",
        "src/tickle.c",
        "    if (estimate->frames == 0) {\n        return 1.0;",
        "    if (estimate->frames == 0) {\n        return 0.0;",
        SEND,
    ),
    (
        "cap: the threshold's half taken twice",
        "src/tickle.c",
        "    double budget = (double)hybrid->frag_high_thresh / held_per_loss; // L(n) may reach this",
        "    double budget = (double)hybrid->frag_high_thresh / 2.0 / held_per_loss; // L(n) may reach this",
        SEND,
    ),
    (
        "cap: quantised up, not down",
        "src/tickle.c",
        "    while (quantised * 2U <= cap) {",
        "    while (quantised < cap) {",
        SEND,
    ),
    (
        "cap: n_max quantised like the rest",
        "src/tickle.c",
        "    if (cap >= n_max) {\n        return node->ip_reassembly_max;",
        "    if (cap > n_max) {\n        return node->ip_reassembly_max;",
        SEND,
    ),
    (
        "cap: the limits not read at create",
        "src/tickle.c",
        "    if (!tt_ip_reassembly_limits(&node->hybrid.frag_high_thresh, &node->hybrid.frag_time_s) ||",
        "    if (true ||",
        SEND,
    ),
    (
        "cap: kept on a context whose ceiling holds one unit",
        "src/tickle.c",
        "    node->hybrid.rx_capable = hybrid_units_within(node->ip_reassembly_max, false) >= 2U;",
        "    node->hybrid.rx_capable = true;",
        SEND,
    ),
    (
        "tick: more often than the announce interval",
        "src/tickle.c",
        "    if (now - hybrid->tick_ns < tt_CONTEXT_UPDATE_INTERVAL) {\n        return;\n    }\n    uint64_t failures",
        "    uint64_t failures",
        SEND,
    ),
    (
        "tick: a change not announced",
        "src/tickle.c",
        "    node->last_modified = now > node->last_modified ? now : node->last_modified + 1U;\n"
        "    build_and_send_update(node, NULL, 0);",
        "    node->last_modified = now > node->last_modified ? now : node->last_modified + 1U;",
        SEND,
    ),
    (
        "tick: a change not a new generation",
        "src/tickle.c",
        "    node->last_modified = now > node->last_modified ? now : node->last_modified + 1U;\n"
        "    build_and_send_update(node, NULL, 0);",
        "    build_and_send_update(node, NULL, 0);",
        SEND,
    ),
    (
        "announce: carries the configured ceiling, not the cap",
        "src/tickle.c",
        "    announce->ip_reassembly_max = node->hybrid.advertised; // the ceiling under the reader's cap",
        "    announce->ip_reassembly_max = node->ip_reassembly_max; // the ceiling under the reader's cap",
        SEND,
    ),
    (
        "withdrawal: without runs arriving",
        "src/tickle.c",
        "            if (hybrid->tick_runs && !hybrid->withdrawn) {",
        "            if (!hybrid->withdrawn) {",
        SEND,
    ),
    (
        "withdrawal: an unread baseline taken as a rise",
        "src/tickle.c",
        "        if (hybrid->reasm_failures_read && failures > hybrid->reasm_failures) {",
        "        if (failures > hybrid->reasm_failures) {",
        SEND,
    ),
    (
        "withdrawal: still for T, not 2T",
        "src/tickle.c",
        "    if (hybrid->withdrawn && now - hybrid->still_since_ns >= 2ULL * hybrid->frag_time_s * tt_SECOND) {",
        "    if (hybrid->withdrawn && now - hybrid->still_since_ns >= 1ULL * hybrid->frag_time_s * tt_SECOND) {",
        SEND,
    ),
    (
        "withdrawal: a later rise does not restart the wait",
        "src/tickle.c",
        "            hybrid->still_since_ns = now;\n        }\n        hybrid->reasm_failures = failures;",
        "            hybrid->still_since_ns = hybrid->still_since_ns == 0 ? now : hybrid->still_since_ns;\n"
        "        }\n        hybrid->reasm_failures = failures;",
        SEND,
    ),
    (
        "withdrawal: a large run's receipt not noted",
        "src/tickle.c",
        "    const uint8_t* payload = buffer + head;\n#if tt_HYBRID_TRANSPORT\n    if (node->hybrid.rx_capable) {\n"
        "        hybrid_rx_note(node, header->source, entity_id, seq_no - index, index, count, run_count);\n"
        "        node->hybrid.tick_runs = true;",
        "    const uint8_t* payload = buffer + head;\n#if tt_HYBRID_TRANSPORT\n    if (node->hybrid.rx_capable) {\n"
        "        hybrid_rx_note(node, header->source, entity_id, seq_no - index, index, count, run_count);",
        SEND,
    ),
]


def build_and_run(binary):
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), f"obj/debug/tests/{binary}"],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        run = subprocess.run([str(TESTS / binary)], capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode < 0:
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    files = sorted({m[1] for m in MUTANTS})
    backup = pathlib.Path(tempfile.mkdtemp(prefix="hybrid_send_mutants_"))
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
            print(f"[control]  {binary:<28} unmutated -> {verdict}")
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
            caught = verdict in ("FAIL", "CRASH", "HANG")
            print(f"[{'caught' if caught else 'MISSED'}]   {name:<72} {binary} -> {verdict}")
            failures += 0 if caught else 1
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"{len(MUTANTS)} mutants, {failures} not caught" if failures else f"all {len(MUTANTS)} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

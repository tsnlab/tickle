#!/usr/bin/env python3
"""Mutant sweep for the hybrid transport's sending half (DESIGN.md section 8, "Hybrid transport", steps 3 to 5).

Each entry breaks exactly one condition the sender relies on - when a sample may go in runs, how n is bounded, where a
run ends, what a repair is, how loss and cost are counted - and names the test binary that must go red for it. A mutant
that still passes means either the test does not test what it says, or the condition decides nothing and should not be
in the code.

Run from the repository root. MUTANTS=<substring> runs only the mutants whose name contains it, or any of several
separated by "|" (the controls always run). MUTANTS_SHARD=k/n runs only the mutants whose index is k modulo n: the
sweep is ~180 runs of a test binary that takes one to two minutes, so two shards in two checkouts halve it.
tests/mutant_guard.py refuses the sweep unless every anchor occurs exactly
once - checked for all of them before the controls, so a stale anchor is reported with every other one rather than one
at a time mid-sweep (35 went stale at once when tt_InitOptions bundle 7 moved the hybrid constants into
node->opts.hybrid_transport) - bounds every child (2 GB, a deadline, capped output) and restores the edited sources
whatever happens, SIGINT/SIGTERM/SIGHUP included.
"""
import os
import pathlib
import sys

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
LINUX = ROOT / "platform/linux"
TESTS = LINUX / "obj/debug/tests"
TIMEOUT_S = 300
SEND = "test_hybrid_send"
SHM = "test_large_shm"
CAP = "test_hybrid_capability"

# (name, file, old, new, test binary that must fail)
MUTANTS = [
    (
        "runs: from two units, not three",
        "src/tickle.c",
        "#define HYBRID_MIN_UNITS 3U",
        "#define HYBRID_MIN_UNITS 2U",
        SEND,
    ),
    (
        "small runs: the last unit inside a run",
        "src/tickle.c",
        "    return one_run && run_n < count - 1U ? run_n : count - 1U;",
        "    return one_run && run_n < count - 1U ? run_n : count;",
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
        "    return seam_send_batch(node, &last, 1, &dest->context_id) >= 0;",
        "    return true;",
        SEND,
    ),
    (
        "large runs: the last unit inside a run",
        "src/tickle.c",
        "    for (; k < LARGE_BATCH && call_units < max_units && next < last; k++) {\n        if (next >= run_until) {",
        "    for (; k < LARGE_BATCH && call_units < max_units && next <= last; k++) {\n        if (next >= run_until) {",
        SEND,
    ),
    (
        "large runs: a send call packs runs up to LARGE_BATCH datagrams, not units (the encode ahead of the wire)",
        "src/tickle.c",
        "    for (; k < LARGE_BATCH && call_units < max_units && next < last; k++) {",
        "    for (; k < LARGE_BATCH && next < last; k++) {",
        SEND,
    ),
    (
        "large runs: a run's units not counted toward its call",
        "src/tickle.c",
        "        units[k] = run;\n        call_units += run;",
        "        units[k] = run;",
        SEND,
    ),
    (
        "large runs: a re-probe's MTU units not counted toward its call",
        "src/tickle.c",
        "            units[k] = 1;\n            call_units++;",
        "            units[k] = 1;",
        SEND,
    ),
    (
        "large runs: a call stops after one run",
        "src/tickle.c",
        "        units[k] = run;\n        call_units += run;",
        "        units[k] = run;\n        call_units += LARGE_BATCH;",
        SEND,
    ),
    (
        "send on writable: the timer's first wait though the HAL reports room",
        "src/tickle.c",
        "    uint64_t wait = reported ? (uint64_t)tt_LARGE_SEND_WRITABLE_FALLBACK_NS : cursor->retry_ns;",
        "    uint64_t wait = cursor->retry_ns;",
        SEND,
    ),
    (
        "send on writable: the fallback timer though the poll never waits",
        "src/tickle.c",
        "    bool reported = cursor->on_writable && !__atomic_load_n(&node->large.polls_without_waiting, __ATOMIC_RELAXED);",
        "    bool reported = cursor->on_writable;",
        SEND,
    ),
    (
        "send on writable: the wait's report ignored by the poll",
        "src/tickle.c",
        "    if (node->large.tx_writable) {\n        large_tx_writable(node);",
        "    if (false && node->large.tx_writable) {\n        large_tx_writable(node);",
        SEND,
    ),
    (
        "send on writable: armed once, never again",
        "src/tickle.c",
        "    pub->large_cursor.scheduled = false;\n    pub->large_cursor.on_writable = false;\n    large_pump(node, pub);",
        "    pub->large_cursor.scheduled = false;\n    large_pump(node, pub);",
        SEND,
    ),
    (
        "send on writable: the fallback timer left behind",
        "src/tickle.c",
        "        (void)tt_Context_unschedule(node, large_send_resume, pub);\n        node->large.send_writable++;",
        "        node->large.send_writable++;",
        SEND,
    ),
    (
        "match point: the end-of-sample HEARTBEAT carries none",
        "src/tickle.c",
        # large_send_end_heartbeat() only: send_idle_heartbeat() has the same two lines, reached by no early return
        "    if (!publisher_oldest_seq_no(pub, &first)) {\n        return;\n    }\n"
        "    uint8_t count = heartbeat_unicast_count(node, pub);\n    uint32_t owed = 0;\n    if (match_point_owed(pub, &owed)) {",
        "    if (!publisher_oldest_seq_no(pub, &first)) {\n        return;\n    }\n"
        "    uint8_t count = heartbeat_unicast_count(node, pub);\n    uint32_t owed = 0;\n"
        "    if (false && match_point_owed(pub, &owed)) {",
        SEND,
    ),
    (
        "match point: a re-registration's initial HEARTBEAT carries none",
        "src/tickle.c",
        "    if (match_point_owed(pub, &owed)) {\n        encode_heartbeat_range(node, pub, first_seq_no, owed - 1U, false, "
        "NULL, 0, tt_HEARTBEAT_FLAG_FINAL);\n    }\n    encode_and_send_heartbeat(node, pub, first_seq_no, target, 1,",
        "    if (false && match_point_owed(pub, &owed)) {\n        encode_heartbeat_range(node, pub, first_seq_no, owed - 1U, "
        "false, NULL, 0, tt_HEARTBEAT_FLAG_FINAL);\n    }\n    encode_and_send_heartbeat(node, pub, first_seq_no, target, 1,",
        SEND,
    ),
    (
        "keep_all: a reader without a discovery table never has the writer's announce pulled again",
        "src/tickle.c",
        "                sub->node->update_reprocess[node_id] = true;",
        "                (void)0;",
        SEND,
    ),
    (
        "keep_all: the writer's announce pulled again whatever the policy",
        "src/tickle.c",
        "            if (proxy->keep_all == tt_WRITER_KEEP_ALL_UNKNOWN && sub->node != NULL) {",
        "            if (sub->node != NULL) {",
        SEND,
    ),
    (
        "match point: the periodic Heartbeat carries none",
        "src/tickle.c",
        "        if (match_point_owed(pub, &owed)) {\n            encode_heartbeat_range(node, pub, first_seq_no,",
        "        if (false && match_point_owed(pub, &owed)) {\n            encode_heartbeat_range(node, pub, first_seq_no,",
        SEND,
    ),
    (
        "match point: the periodic Heartbeat's carried one seq_no late",
        "src/tickle.c",
        "            encode_heartbeat_range(node, pub, first_seq_no, owed - 1U, false, NULL, 0, tt_HEARTBEAT_FLAG_FINAL);",
        "            encode_heartbeat_range(node, pub, first_seq_no, owed, false, NULL, 0, tt_HEARTBEAT_FLAG_FINAL);",
        SEND,
    ),
    (
        "match point: carried one seq_no late",
        "src/tickle.c",
        # large_send_end_heartbeat() only, by the line after: send_idle_heartbeat() has the same encode
        "        encode_heartbeat_range(node, pub, first, owed - 1U, false, NULL, 0, tt_HEARTBEAT_FLAG_FINAL);\n    }\n"
        "    encode_heartbeat_range(node, pub, first, publisher_announced_last_seq_no(pub), true, count != 0 ? pub->peers : NULL,\n"
        "                           count, 0);\n}",
        "        encode_heartbeat_range(node, pub, first, owed, false, NULL, 0, tt_HEARTBEAT_FLAG_FINAL);\n    }\n"
        "    encode_heartbeat_range(node, pub, first, publisher_announced_last_seq_no(pub), true, count != 0 ? pub->peers : NULL,\n"
        "                           count, 0);\n}",
        SEND,
    ),
    (
        "match point: still carried once the reader has it",
        "src/tickle.c",
        "        if (ack->context_id == tt_CONTEXT_ID_INVALID || ack->match_heartbeats_left == 0) {\n            continue;\n        }\n        if (!any || seq_delta(ack->first_owed_seq_no, *owed) < 0) {\n            *owed = ack->first_owed_seq_no;\n            any = true;\n        }\n    }\n    return any;",
        "        if (ack->context_id == tt_CONTEXT_ID_INVALID) {\n            continue;\n        }\n        if (!any || seq_delta(ack->first_owed_seq_no, *owed) < 0) {\n            *owed = ack->first_owed_seq_no;\n            any = true;\n        }\n    }\n    return any;",
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
        "    return pub->large_fragmentation != tt_LARGE_FRAGMENTATION_INHERIT ? pub->large_fragmentation\n                                                                      : node->large_fragmentation;",
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
        "        smallest = smallest == 0 || run_n < smallest ? run_n : smallest;",
        "        smallest = smallest == 0 || run_n > smallest ? run_n : smallest;",
        SEND,
    ),
    (
        "unicast: every destination given the broadcast's minimum",
        "src/tickle.c",
        "    *one_run = false;\n    if (context_id != tt_CONTEXT_ID_INVALID) {",
        "    *one_run = false;\n    if (false) {",
        SEND,
    ),
    (
        "counters: a run's units not counted",
        "src/tickle.c",
        "        hybrid->run_units += run_until;",
        "",
        SEND,
    ),
    (
        "objective: no loss read as MTU",
        "src/tickle.c",
        "    if (p <= 0.0) {\n        return n_max;",
        "    if (p <= 0.0) {\n        return 1U;",
        SEND,
    ),
    (
        "objective: the header not shared over the run",
        "src/tickle.c",
        "        double cost = unit + (header / (double)run_n) + ((1.0 - clean) * whole);",
        "        double cost = unit + header + ((1.0 - clean) * whole);",
        SEND,
    ),
    (
        "objective: a lost run's repairs not counted",
        "src/tickle.c",
        "        double cost = unit + (header / (double)run_n) + ((1.0 - clean) * whole);",
        "        double cost = unit + (header / (double)run_n);",
        SEND,
    ),
    (
        "objective: a repair's header left out",
        "src/tickle.c",
        "    double whole = unit + header;",
        "    double whole = unit;",
        SEND,
    ),
    (
        "reliable n: a node with no reader of this Publisher read as lossless",
        "src/tickle.c",
        "    if (!reader) {\n        return 1U;",
        "    if (false) {\n        return 1U;",
        SEND,
    ),
    (
        "exposure: given to every reader, not the one sent to",
        "src/tickle.c",
        "            (context_id != tt_CONTEXT_ID_INVALID && ack->context_id != context_id)) {",
        "            (context_id != tt_CONTEXT_ID_INVALID && false)) {",
        SEND,
    ),
    (
        "nacks: read with no sample watched",
        "src/tickle.c",
        "    if (watch->watch_count == 0) {\n        return; // no sample sent to it in runs is being watched\n    }",
        "",
        SEND,
    ),
    (
        "nacks: a re-NACK counted",
        "src/tickle.c",
        "            if (seq_delta(seq_no, watch->nack_high) <= 0) {\n                continue; // a re-NACK\n            }",
        "",
        SEND,
    ),
    (
        "nacks: the highest NACK not kept",
        "src/tickle.c",
        "    watch->nack_high = high;",
        "",
        SEND,
    ),
    (
        "nacks: an ACKNACK's NACKs not read",
        "src/tickle.c",
        "            hybrid_note_nacks(node, pub, header->source, sender_entity_id, seq_no, bitmap, bitmap_words);",
        "",
        SEND,
    ),
    (
        "nacks: a Publisher that sent runs never marked watching",
        "src/tickle.c",
        "        pub->watching = true; // process_acknack() reads NACKs for the watch from now on",
        "",
        SEND,
    ),
    (
        "path: judged failed with the tail lost too",
        "src/tickle.c",
        "    if (watch->watch_count == 0 || watch->watch_all_lost || watch->watch_tail_nacked ||",
        "    if (watch->watch_count == 0 || watch->watch_all_lost ||",
        SEND,
    ),
    (
        "path: judged failed with a run through",
        "src/tickle.c",
        "        watch->watch_nacked != watch->watch_run_units) {",
        "        watch->watch_nacked == 0) {",
        SEND,
    ),
    (
        "path: one multi-run sample not enough",
        "src/tickle.c",
        "    if (watch->watch_runs >= 2U || watch->all_lost_streak >= 2U || watch->watch_probe) {",
        "    if (watch->all_lost_streak >= 2U || watch->watch_probe) {",
        SEND,
    ),
    (
        "path: one single-run sample enough",
        "src/tickle.c",
        "    if (watch->watch_runs >= 2U || watch->all_lost_streak >= 2U || watch->watch_probe) {",
        "    if (watch->watch_runs >= 1U || watch->all_lost_streak >= 2U || watch->watch_probe) {",
        SEND,
    ),
    (
        "path: a lost re-probe not judged at once",
        "src/tickle.c",
        "    if (watch->watch_runs >= 2U || watch->all_lost_streak >= 2U || watch->watch_probe) {",
        "    if (watch->watch_runs >= 2U || watch->all_lost_streak >= 2U) {",
        SEND,
    ),
    (
        "path: no MTU period after the judgement",
        "src/tickle.c",
        "            if (now - watch->path_failed_ns < node->opts.hybrid_transport.path_clean_ns) {\n                return 1U;\n            }",
        "",
        SEND,
    ),
    (
        "path: re-probed with every run",
        "src/tickle.c",
        "            *one_run = true;\n        }",
        "        }",
        SEND,
    ),
    (
        "path: a re-probe that got through not cleared",
        "src/tickle.c",
        "        if (watch->watch_probe) {\n            watch->path_failed_ns = 0;\n        }\n        watch->watch_count = 0;",
        "        watch->watch_count = 0;",
        SEND,
    ),
    (
        "path: the tail not told from the runs",
        "src/tickle.c",
        "    } else if (seq_no - watch->watch_base == watch->watch_count - 1U) {",
        "    } else if (false) {",
        SEND,
    ),
    (
        "reader: a sample left short not closed",
        "src/tickle.c",
        "            hybrid_rx_gap(node, writer, writer->next, writer->count); // the writer's last sample ended short",
        "            (void)0;",
        SEND,
    ),
    (
        "reader: a writer first met mid-sample read as a gap",
        "src/tickle.c",
        "        writer->next = (uint16_t)index;\n        writer->source = source;",
        "        writer->next = 0;\n        writer->source = source;",
        SEND,
    ),
    (
        "reader: an older sample's unit taken as a new sample",
        "src/tickle.c",
        "    if (seq_delta(sample, writer->sample) > 0) {",
        "    if (sample != writer->sample) {",
        SEND,
    ),
    (
        "reader: R counts arrivals, not the units sent",
        "src/tickle.c",
        "    hybrid->tick_units += end - from;\n",
        "",
        SEND,
    ),
    (
        "reader: the window never halves",
        "src/tickle.c",
        "    if (estimate->frames >= estimate->window_frames) {",
        "    if (false) {",
        SEND,
    ),
    (
        "bound: the point estimate, not the upper limit",
        "src/tickle.c",
        "    double p = loss_poisson_upper(estimate->events, strict) / (double)estimate->frames;",
        "    double p = (double)estimate->events / (double)estimate->frames;",
        SEND,
    ),
    (
        "bound: Wilson-Hilferty without its skew term",
        "src/tickle.c",
        "    double cube = 1.0 - (1.0 / (LOSS_WH_SKEW * shifted)) + (quantile / (LOSS_WH_SPREAD * hybrid_sqrt(shifted)));",
        "    double cube = 1.0 + (quantile / (LOSS_WH_SPREAD * hybrid_sqrt(shifted)));",
        SEND,
    ),
    (
        "bound: no exposure reads as no loss",
        "src/tickle.c",
        "    if (estimate->frames == 0) {\n        return 1.0;\n    }\n    double p = loss_poisson_upper",
        "    if (estimate->frames == 0) {\n        return 0.0;\n    }\n    double p = loss_poisson_upper",
        SEND,
    ),
    (
        "cap: n_max quantised like the rest",
        "src/tickle.c",
        "    if (cap >= n_max && node->opts.hybrid_transport.n_max >= (uint8_t)tt_HYBRID_N_MAX) {",
        "    if (cap > n_max && node->opts.hybrid_transport.n_max >= (uint8_t)tt_HYBRID_N_MAX) {",
        SEND,
    ),
    (
        "cap: the benchmark switch ignored",
        "src/tickle.c",
        "    if (!hybrid->cap_off && hybrid->path_until_ns == 0) {\n        hybrid_control(",
        "    if (hybrid->path_until_ns == 0) {\n        hybrid_control(",
        SEND,
    ),
    (
        "cap: the benchmark switch not read at create",
        "src/tickle.c",
        "    hybrid->cap_off = node->opts.net.reassembly_cap_off != 0;",
        "    hybrid->cap_off = false;",
        SEND,
    ),
    (
        "tick: more often than the announce interval",
        "src/tickle.c",
        "    if (now - hybrid->tick_ns < discovery_update_interval_ns(node)) {\n        return;\n    }\n    uint64_t elapsed",
        "    uint64_t elapsed",
        SEND,
    ),
    (
        "tick: a change not announced",
        "src/tickle.c",
        "    node->last_modified = now > node->last_modified ? now : node->last_modified + 1U;\n    build_and_send_update(node, NULL, 0);",
        "    node->last_modified = now > node->last_modified ? now : node->last_modified + 1U;",
        SEND,
    ),
    (
        "tick: a change not a new generation",
        "src/tickle.c",
        "    node->last_modified = now > node->last_modified ? now : node->last_modified + 1U;\n    build_and_send_update(node, NULL, 0);",
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
        "reader: a small run's units not noted",
        "src/tickle.c",
        "    if (hybrid_rx_capable(node)) {\n        hybrid_rx_note(node, header->source, entity_id, seq_no - index, index, count, run_count, true);\n    }\n#endif\n    for (uint32_t i = 0; i < run_count; i++) {\n        uint32_t unit_index = index + i;\n        uint32_t unit_length",
        "    if (false) {\n    }\n#endif\n    for (uint32_t i = 0; i < run_count; i++) {\n        uint32_t unit_index = index + i;\n        uint32_t unit_length",
        SEND,
    ),
    (
        "reader: a large run's units not noted",
        "src/tickle.c",
        "    if (hybrid_rx_capable(node)) {\n        hybrid_rx_note(node, header->source, entity_id, seq_no - index, index, count, run_count, true);\n    }\n#endif\n    if (index != 0 &&\n        large_run_batched(",
        "    if (false) {\n    }\n#endif\n    if (index != 0 &&\n        large_run_batched(",
        SEND,
    ),
    (
        "objective: the posterior mean without its prior event",
        "src/tickle.c",
        "    double p = ((double)estimate->events + 1.0) / (double)estimate->frames;",
        "    double p = (double)estimate->events / (double)estimate->frames;",
        SEND,
    ),
    (
        "objective: the posterior mean with no exposure reads as no loss",
        "src/tickle.c",
        "    if (estimate->frames == 0) {\n        return 1.0;\n    }\n    double p = ((double)estimate->events",
        "    if (estimate->frames == 0) {\n        return 0.0;\n    }\n    double p = ((double)estimate->events",
        SEND,
    ),
    (
        "reliable n: the reader's reported p ignored",
        "src/tickle.c",
        "    return hybrid_n_star(hybrid_peer_p(node, context_id),",
        "    return hybrid_n_star(0.0,",
        SEND,
    ),
    (
        "announce: loss_p not carried",
        "src/tickle.c",
        "    announce->loss_p = node->hybrid.advertised_loss;",
        "    announce->loss_p = 0;",
        SEND,
    ),
    (
        "announce: loss_p not recorded",
        "src/tickle.c",
        "    node->hybrid.peer_loss_p[source] = rd16(header, announce->loss_p);",
        "",
        SEND,
    ),
    (
        "announce: loss_p kept across a presumed death",
        "src/tickle.c",
        "    node->hybrid.peer_loss_p[source] = 0; // nor its loss known",
        "",
        CAP,
    ),
    (
        "loss code: not rounded up",
        "src/tickle.c",
        "    rounded += (double)rounded < mantissa ? 1U : 0U;\n",
        "",
        SEND,
    ),
    (
        "loss code: every change re-announced",
        "src/tickle.c",
        "    if ((measured >> 8U) != (advertised >> 8U)) {\n        return true;\n    }",
        "    if (measured != advertised) {\n        return true;\n    }",
        SEND,
    ),
    (
        "tick: p not announced",
        "src/tickle.c",
        "    bool loss_moved = hybrid_loss_moved(loss, hybrid->advertised_loss);",
        "    bool loss_moved = false;",
        SEND,
    ),
    (
        "tick: p announced as the upper bound, not the posterior mean",
        "src/tickle.c",
        "    return hybrid_loss_code(loss_estimate_mean(estimate));",
        "    return hybrid_loss_code(loss_estimate_p(estimate));",
        SEND,
    ),
    (
        "reader: a lost body read as arrived runs",
        "src/tickle.c",
        "        hybrid_rx_run(node, writer, size, true);",
        "        hybrid_rx_run(node, writer, size, false);",
        SEND,
    ),
    (
        "reader: lost runs not in the pooled window",
        "src/tickle.c",
        "    run_window_observe(&hybrid->window, size, lost, &hybrid->mtu_loss.window_frames);",
        "    run_window_observe(&hybrid->window, size, false, &hybrid->mtu_loss.window_frames);",
        SEND,
    ),
    (
        "reader: n not learnt from a full run",
        "src/tickle.c",
        "            writer->layout_n = (uint8_t)units;\n",
        "",
        SEND,
    ),
    (
        "reader: an MTU unit in the body does not say MTU",
        "src/tickle.c",
        "    } else if (index < tail) {\n        writer->layout_n = 1;\n",
        "    } else if (index < tail) {\n",
        SEND,
    ),
    (
        "reader: a missing tail not an MTU loss",
        "src/tickle.c",
        "        hybrid_rx_mtu(node, 1, 1); // the tail, always an MTU unit",
        "        hybrid_rx_mtu(node, 1, 0);",
        SEND,
    ),
    (
        "reader: a repair counted",
        "src/tickle.c",
        "    if (sample != writer->sample || index + units <= writer->next) {\n        return; // an older sample's unit, or a repair",
        "    if (sample != writer->sample) {\n        return; // an older sample's unit, or a repair",
        SEND,
    ),
    (
        "reader: a gap of unknown layout counted as MTU loss",
        "src/tickle.c",
        "            loss_estimate_observe(&hybrid->rx_loss, body_end - from, 1);",
        "            hybrid_rx_mtu(node, body_end - from, body_end - from);",
        SEND,
    ),
    (
        "law: no collapse ever called",
        "src/tickle.c",
        "    if (hybrid_collapsed(&hybrid->window, &hybrid->mtu_loss, opts)) {",
        "    if (false) {",
        SEND,
    ),
    (
        "law: collapse without significance",
        "src/tickle.c",
        "    return score >= tt_HYBRID_COLLAPSE_Z && lost >= far * runs;",
        "    return lost >= far * runs;",
        SEND,
    ),
    (
        "law: collapse without the size of the excess",
        "src/tickle.c",
        "    return score >= tt_HYBRID_COLLAPSE_Z && lost >= far * runs;",
        "    return score >= tt_HYBRID_COLLAPSE_Z;",
        SEND,
    ),
    (
        "law: collapse on fewer than the minimum lost",
        "src/tickle.c",
        " || window->lost < opts->collapse_min_lost) {",
        ") {",
        SEND,
    ),
    (
        "law: collapse on fewer than the minimum runs",
        "src/tickle.c",
        "    if (window->sent < opts->collapse_min_runs || window->lost",
        "    if (window->lost",
        SEND,
    ),
    (
        "law: the baseline at p's mean, not its upper bound",
        "src/tickle.c",
        "    double baseline = hybrid_baseline_run_loss(window, loss_estimate_bound(mtu, true));",
        "    double baseline = hybrid_baseline_run_loss(window, loss_estimate_mean(mtu));",
        SEND,
    ),
    (
        "law: significance at the 95% bound, not the test's own 99.9%",
        "src/tickle.c",
        "    double baseline = hybrid_baseline_run_loss(window, loss_estimate_bound(mtu, true));",
        "    double baseline = hybrid_baseline_run_loss(window, loss_estimate_bound(mtu, false));",
        SEND,
    ),
    (
        "law: the size of the excess at the 99.9% bound, not the 95% one",
        "src/tickle.c",
        "    double allowed = hybrid_baseline_run_loss(window, loss_estimate_p(mtu));",
        "    double allowed = baseline;",
        SEND,
    ),
    (
        "law: the clean test at the 99.9% bound",
        "src/tickle.c",
        "hybrid_baseline_run_loss(window, loss_estimate_p(mtu)) * (double)window->sent;",
        "hybrid_baseline_run_loss(window, loss_estimate_bound(mtu, true)) * (double)window->sent;",
        SEND,
    ),
    (
        "law: far read on the loss side only (2 L0 past 1 never met)",
        "src/tickle.c",
        "    far = far < far_survival ? far : far_survival;\n",
        "    far = far_survival > 1.0 ? far_survival : far;\n",
        SEND,
    ),
    (
        "law: far read on the survival side only",
        "src/tickle.c",
        "    far = far < far_survival ? far : far_survival;\n",
        "    far = far_survival;\n",
        SEND,
    ),
    (
        "bound: the strict table is the 95% one",
        "src/tickle.c",
        "        return strict ? exact_strict[events] : exact[events];",
        "        return exact[events];",
        SEND,
    ),
    (
        "bound: the strict Wilson-Hilferty at the 95% quantile",
        "src/tickle.c",
        "    double quantile = strict ? LOSS_BOUND_Z_STRICT : LOSS_BOUND_Z;",
        "    double quantile = LOSS_BOUND_Z;",
        SEND,
    ),
    (
        "law: a clean window without the minimum runs",
        "src/tickle.c",
        "    return window->sent >= opts->collapse_min_runs &&\n           (double)window->lost <=",
        "    return (double)window->lost <=",
        SEND,
    ),
    (
        "law: the drop is a halving",
        "src/tickle.c",
        "    uint32_t dropped = (uint32_t)(((double)n_fail * drop) + HYBRID_HALF);",
        "    uint32_t dropped = (uint32_t)(((double)n_fail * 0.5) + HYBRID_HALF);",
        SEND,
    ),
    (
        "law: the ceiling is the overshoot, not the survival's",
        "src/tickle.c",
        "    uint32_t n_fail = hybrid_ceil((double)run_n * survived / clean);",
        "    uint32_t n_fail = run_n;",
        SEND,
    ),
    (
        "law: a failed probe forgets the ceiling",
        "src/tickle.c",
        "    if (hybrid->n_fail != 0 && hybrid->n_fail < n_fail) {",
        "    if (false) {",
        SEND,
    ),
    (
        "law: no hold after a collapse",
        "src/tickle.c",
        "    hybrid->hold_until_ns = now + hybrid->lifetime_ns;",
        "    hybrid->hold_until_ns = now;",
        SEND,
    ),
    (
        "law: the hold's windows kept",
        "src/tickle.c",
        "        hybrid->window = (struct tt_RunWindow) {0}; // the full memory drains: nothing in it is about the cap now\n",
        "",
        SEND,
    ),
    (
        "law: a probe before the wait",
        "src/tickle.c",
        "    if (now >= hybrid->probe_at_ns) {",
        "    if (true) {",
        SEND,
    ),
    (
        "law: the probe wait not doubled on a failure",
        "src/tickle.c",
        "            hybrid->backoff < opts->probe_backoff_max ? (uint8_t)(hybrid->backoff + 1U) : opts->probe_backoff_max;",
        "            hybrid->backoff < opts->probe_backoff_max ? (uint8_t)(hybrid->backoff + 0U) : opts->probe_backoff_max;",
        SEND,
    ),
    (
        "law: a probe judged before tau",
        "src/tickle.c",
        "        if (now - hybrid->probe_since_ns < hybrid->lifetime_ns) {",
        "        if (false) {",
        SEND,
    ),
    (
        "law: a probe that held does not move the ceiling",
        "src/tickle.c",
        "        hybrid->n_fail = step > hybrid->cap ? step : hybrid->cap + 1U;\n",
        "",
        SEND,
    ),
    (
        "law: slow start adds one, not doubles",
        "src/tickle.c",
        "        hybrid->cap = hybrid->cap * 2U < n_max ? hybrid->cap * 2U : n_max;",
        "        hybrid->cap = hybrid->cap + 1U < n_max ? hybrid->cap + 1U : n_max;",
        SEND,
    ),
    (
        "law: growth jumps to the ceiling",
        "src/tickle.c",
        "        hybrid->cap += (plateau - hybrid->cap + 1U) / 2U;",
        "        hybrid->cap = hybrid->n_fail;",
        SEND,
    ),
    (
        "law: a one-unit cap never moves on",
        "src/tickle.c",
        " || hybrid->cap < 2U) {",
        ") {",
        SEND,
    ),
    (
        "law: the slow start's two units back",
        "src/tickle.c",
        "    hybrid->cap = hybrid->cap_off ? n_max : hybrid_start_cap(n_max, node->opts.hybrid_transport.slow_start);",
        "    hybrid->cap = hybrid->cap_off ? n_max : 2U;",
        SEND,
    ),
    (
        "p: no 'none seen', the posterior mean at no events",
        "src/tickle.c",
        "    if (estimate->events == 0 &&\n        estimate->frames - estimate->frames_at_doubt >= opts->none_seen_frames) { // seen, since a doubt",
        "    if (false) {",
        SEND,
    ),
    (
        "p: 'none seen' after any frame",
        "src/tickle.c",
        "    if (estimate->events == 0 &&\n        estimate->frames - estimate->frames_at_doubt >= opts->none_seen_frames) { // seen, since a doubt",
        "    if (estimate->events == 0 && estimate->frames != 0) {",
        SEND,
    ),
    (
        "p: 'none seen' at the tail only before any announce",
        "src/tickle.c",
        "    if ((node->hybrid.advertised_loss != HYBRID_LOSS_NONE_SEEN && measured == HYBRID_LOSS_NONE_SEEN) ||",
        "    if ((node->hybrid.advertised_loss == 0 && measured == HYBRID_LOSS_NONE_SEEN) ||",
        SEND,
    ),
    (
        "p: the first 'none seen' waits for the interval",
        "src/tickle.c",
        "        hybrid_rx_first_estimate(node);",
        "        (void)0;",
        SEND,
    ),
    (
        "reader: a gap judged by the layout of the sample before",
        "src/tickle.c",
        "    hybrid_rx_layout(writer, index, units, tail, run);\n    if (index > writer->next) {\n"
        "        hybrid_rx_gap(node, writer, writer->next, index);\n    }\n",
        "    if (index > writer->next) {\n        hybrid_rx_gap(node, writer, writer->next, index);\n    }\n"
        "    hybrid_rx_layout(writer, index, units, tail, run);\n",
        SEND,
    ),
    (
        "reader: a short last run keeps an MTU layout",
        "src/tickle.c",
        "        } else if (writer->layout_n < 2U) {",
        "        } else if (false) {",
        SEND,
    ),
    (
        "p: stale events never forgotten",
        "src/tickle.c",
        "    uint32_t clean = estimate->frames - anchor;\n    if (",
        "    uint32_t clean = estimate->frames - anchor;\n    if (clean != 0U || clean == 0U) {\n        return false;\n    }\n    if (",
        SEND,
    ),
    (
        "p: stale events forgotten one clean frame early",
        "src/tickle.c",
        "        double stretch = 1.0 + ((double)clean / (double)anchor);",
        "        double stretch = 1.0 + ((double)(clean + 1U) / (double)anchor);",
        SEND,
    ),
    (
        "p: stale events weighed as one fewer",
        "src/tickle.c",
        "        for (uint32_t i = 0; i < estimate->events; i++) {",
        "        for (uint32_t i = 1; i < estimate->events; i++) {",
        SEND,
    ),
    (
        "p: stale events forgotten by the rule before 2026-10-15 (9.21 / p, p read at the event)",
        "src/tickle.c",
        "        if (chance > opts->stale_false_forget) {",
        "        if (((double)estimate->events + 1.0) * (double)clean < 9.21 * (double)anchor) {",
        SEND,
    ),
    (
        "p: stale events forgotten only at the floor, never by the ratio",
        "src/tickle.c",
        "        if (chance > opts->stale_false_forget) {",
        "        if (true) {",
        SEND,
    ),
    (
        "p: stale floor removed (the ratio alone, be097aec)",
        "src/tickle.c",
        "    if (clean < estimate->stale_floor_frames) { // the floor: 9,210 frames by default",
        "    if (true) { // the floor: 9,210 frames by default",
        SEND,
    ),
    (
        "p: stale floor not at p_min (9.21 / 1e-2, 921 frames)",
        "src/tickle.c",
        "    double floor = hybrid_stale_exposure(opts) / opts->stale_p_min;",
        "    double floor = hybrid_stale_exposure(opts) / 1e-2;",
        SEND,
    ),
    (
        "p: stale floor one clean frame early",
        "src/tickle.c",
        "    if (clean < estimate->stale_floor_frames) { // the floor: 9,210 frames by default",
        "    if (clean + 1U < estimate->stale_floor_frames) { // the floor: 9,210 frames by default",
        SEND,
    ),
    (
        "p: stale events forgotten however many",
        "src/tickle.c",
        "    if (estimate->events - 1U >= opts->stale_events || estimate->frames_at_event == 0) {",
        "    if (estimate->events == 0 || estimate->frames_at_event == 0) {",
        SEND,
    ),
    (
        "p: stale events never forgotten (not at a tail, the only place since 2026-10-26)",
        "src/tickle.c",
        "    if (hybrid_rx_forget_stale(node)) {\n        hybrid_announce(node, tt_get_ns());\n        return;\n    }\n",
        "",
        SEND,
    ),
    (
        "p: a stale forget keeps the clean stretch as the estimate (\"none seen\" at once, on trains)",
        "src/tickle.c",
        "    loss_estimate_restart(estimate); // no estimate: the next sample's frames are measured as the first p's were",
        "    estimate->events = 0;\n    estimate->frames = clean;\n    estimate->frames_at_event = 0;\n"
        "    estimate->frames_at_doubt = 0;",
        SEND,
    ),
    (
        "p: no estimate after a stale forget not announced at once",
        "src/tickle.c",
        "    if (hybrid_rx_forget_stale(node)) {\n        hybrid_announce(node, tt_get_ns());\n        return;\n    }\n",
        "    if (hybrid_rx_forget_stale(node)) {\n        return;\n    }\n",
        SEND,
    ),
    (
        "scale: on a single tick",
        "src/tickle.c",
        "    if (hybrid->scale_streak < ticks && hybrid->scale_streak > -ticks) {",
        "    if (false) {",
        SEND,
    ),
    (
        "scale: a normal tick does not break the streak",
        "src/tickle.c",
        "    } else {\n        hybrid->scale_streak = 0;\n    }\n    int32_t ticks",
        "    }\n    int32_t ticks",
        SEND,
    ),
    (
        "scale: R ignored",
        "src/tickle.c",
        "    double factor = hybrid->anchor_r / units_per_s;",
        "    double factor = 1.0;",
        SEND,
    ),
    (
        "scale: no earlier probe",
        "src/tickle.c",
        "        hybrid->probe_at_ns = now;\n        return;\n    }\n    hybrid->n_fail = scaled;",
        "        return;\n    }\n    hybrid->n_fail = scaled;",
        SEND,
    ),
    (
        "scale: down, the cap left above the plateau",
        "src/tickle.c",
        "    hybrid->cap = hybrid->cap < plateau ? hybrid->cap : plateau;\n}",
        "    (void)plateau;\n}",
        SEND,
    ),
    (
        "tau: the writers' run rate ignored",
        "src/tickle.c",
        "        double wrap_ns = HYBRID_IP_ID_SPACE * (double)elapsed_ns / (double)runs;",
        "        double wrap_ns = (double)tt_HYBRID_QUEUE_LIFETIME_NS;",
        SEND,
    ),
    (
        "tau: counted per writer, not per source",
        "src/tickle.c",
        "            if (hybrid->rx_writers[j].source != tt_CONTEXT_ID_INVALID &&\n"
        "                hybrid->rx_writers[j].source == writer->source) {",
        "            if (j == i) {",
        SEND,
    ),
    (
        "tau: the shortest over the sources",
        "src/tickle.c",
        "        longest = lifetime > longest ? lifetime : longest;",
        "        longest = longest == 0 || lifetime < longest ? lifetime : longest;",
        SEND,
    ),
    (
        "tau: a tick without runs resets it",
        "src/tickle.c",
        "    if (longest != 0) {\n        hybrid->lifetime_ns = longest;",
        "    if (true) {\n        hybrid->lifetime_ns = longest;",
        SEND,
    ),
    (
        "reader: the window's n not kept",
        "src/tickle.c",
        "    hybrid->window_n = size > hybrid->window_n ? size : hybrid->window_n;\n",
        "",
        SEND,
    ),
    (
        "path rx: never judged",
        "src/tickle.c",
        "    if (!run && index == tail) {\n        hybrid_rx_judge_path(node, writer);",
        "    if (false) {\n        hybrid_rx_judge_path(node, writer);",
        SEND,
    ),
    (
        "path rx: frame loss no excuse",
        "src/tickle.c",
        "    if (writer->path_odds >= node->opts.hybrid_transport.path_alpha) {\n        return;\n    }\n    writer->path_odds = 1.0;",
        "    writer->path_odds = 1.0;",
        SEND,
    ),
    (
        "path rx: runs that got through within tau ignored",
        "src/tickle.c",
        " || hybrid->path_until_ns != 0 || runs_got_through) {",
        " || hybrid->path_until_ns != 0) {",
        SEND,
    ),
    (
        "path rx: no re-probe",
        "src/tickle.c",
        "        hybrid->cap = hybrid->path_cap; // rule (e)'s re-probe: the cap it had, judged by the next sample to its tail\n",
        "",
        SEND,
    ),
    (
        "path rx: the wait not doubled on a failed re-probe",
        "src/tickle.c",
        "                                   ? (uint8_t)(hybrid->path_backoff + 1U)",
        "                                   ? (uint8_t)(hybrid->path_backoff + 0U)",
        SEND,
    ),
    (
        "path rx: a body through does not clear the re-probe",
        "src/tickle.c",
        "            hybrid->path_probe = false;\n            hybrid->path_backoff = 0;",
        "            hybrid->path_probe = false;",
        SEND,
    ),
    (
        "path rx: not announced at once",
        "src/tickle.c",
        "                   (unsigned)writer->source, (unsigned)((hybrid->path_until_ns - now) / tt_SECOND));\n    hybrid_announce(node, now);",
        "                   (unsigned)writer->source, (unsigned)((hybrid->path_until_ns - now) / tt_SECOND));",
        SEND,
    ),
    (
        "path rx: the law runs while the path is failed",
        "src/tickle.c",
        "    if (!hybrid->cap_off && hybrid->path_until_ns == 0) {",
        "    if (!hybrid->cap_off) {",
        SEND,
    ),
    (
        "path rx: layouts kept across a cap change",
        "src/tickle.c",
        "            hybrid->rx_writers[i].layout_n = 0; // every writer lays its samples out anew under the new cap\n",
        "",
        SEND,
    ),
    (
        "path rx: an unknown layout read as one run of the body",
        "src/tickle.c",
        "    uint32_t layout = hybrid->cap < body ? hybrid->cap : body;",
        "    uint32_t layout = body;",
        SEND,
    ),
    (
        "cap: kept on a context whose ceiling holds one unit",
        "src/tickle.c",
        "    hybrid->rx_capable = n_max >= 2U;",
        "    hybrid->rx_capable = true;",
        CAP,
    ),
    (
        "reader: receiver drops counted as path loss",
        "src/tickle.c",
        "        loss_estimate_doubt(&hybrid->rx_loss);\n    }\n    return taken;",
        "        loss_estimate_doubt(&hybrid->rx_loss);\n    }\n    return 0;",
        SEND,
    ),
    (
        "p: a gap put down to a receiver drop read as clean (no doubt)",
        "src/tickle.c",
        "    } else if (taken != 0) {\n        loss_estimate_doubt(&hybrid->rx_loss);\n    }\n",
        "    }\n",
        SEND,
    ),
    (
        "p: an excused gap before the first p only a doubt, the estimate kept",
        "src/tickle.c",
        "    if (taken != 0 && hybrid->advertised_loss == 0) {",
        "    if (false) {",
        SEND,
    ),
    (
        "p: an excused gap restarts the estimate after the first p too",
        "src/tickle.c",
        "    if (taken != 0 && hybrid->advertised_loss == 0) {",
        "    if (taken != 0) {",
        SEND,
    ),
    (
        "p: the first p not held after an excused gap",
        "src/tickle.c",
        "    if (hybrid->advertised_loss == 0 && hybrid->receiver_lost != 0 &&",
        "    if (false &&",
        SEND,
    ),
    (
        "p: the first p held whether or not a gap was excused",
        "src/tickle.c",
        "    if (hybrid->advertised_loss == 0 && hybrid->receiver_lost != 0 &&",
        "    if (hybrid->advertised_loss == 0 &&",
        SEND,
    ),
    (
        "p: the first p held one frame short",
        "src/tickle.c",
        "hybrid->rx_loss.frames < opts->none_seen_frames) {\n        return 0;",
        "hybrid->rx_loss.frames + 1U < opts->none_seen_frames) {\n        return 0;",
        SEND,
    ),
    (
        "p: the interval announces a first p the excused gaps left",
        "src/tickle.c",
        "    uint16_t loss = hybrid_rx_announce_code(hybrid, &node->opts.hybrid_transport);",
        "    uint16_t loss = hybrid_rx_loss_code(&hybrid->rx_loss, &node->opts.hybrid_transport);",
        SEND,
    ),
    (
        "p: 'none seen' counts the frames before a doubt",
        "src/tickle.c",
        "        estimate->frames - estimate->frames_at_doubt >= opts->none_seen_frames) { // seen, since a doubt",
        "        estimate->frames >= opts->none_seen_frames) { // seen, since a doubt",
        SEND,
    ),
    (
        "p: the stale stretch from the last event, through a doubt",
        "src/tickle.c",
        "        estimate->frames_at_doubt > estimate->frames_at_event ? estimate->frames_at_doubt : estimate->frames_at_event;",
        "        estimate->frames_at_event;",
        SEND,
    ),
    (
        "p: the stale stretch from the doubt, F still the last event's",
        "src/tickle.c",
        "        double stretch = 1.0 + ((double)clean / (double)anchor);",
        "        double stretch = 1.0 + ((double)clean / (double)estimate->frames_at_event);",
        SEND,
    ),
    (
        "p: the doubt not halved with the window",
        "src/tickle.c",
        "        estimate->frames_at_doubt /= 2U;\n",
        "",
        SEND,
    ),
    (
        "p: a forget leaves the doubt",
        "src/tickle.c",
        "    loss_estimate_restart(estimate); // no estimate: the next sample's frames are measured as the first p's were",
        "    uint32_t doubt = estimate->frames_at_doubt;\n    loss_estimate_restart(estimate);\n"
        "    estimate->frames_at_doubt = doubt;",
        SEND,
    ),
    (
        "reader: a run dropped by the receiver still a lost run",
        "src/tickle.c",
        "        if (hybrid_rx_receiver_lost(node, 1) != 0) {\n            continue;\n        }",
        "        (void)hybrid_rx_receiver_lost(node, 1);",
        SEND,
    ),
    (
        "reader: MTU units dropped by the receiver still lost",
        "src/tickle.c",
        "            uint32_t lost = (body_end - from) - hybrid_rx_receiver_lost(node, body_end - from);",
        "            uint32_t lost = body_end - from;",
        SEND,
    ),
    (
        "reader: a tail dropped by the receiver still lost",
        "src/tickle.c",
        "    if (end > tail && from <= tail && hybrid_rx_receiver_lost(node, 1) == 0) {",
        "    if (end > tail && from <= tail) {",
        SEND,
    ),
    (
        "reader: the drop count never read again",
        "src/tickle.c",
        "    if (hybrid->rx_drop_credit[0] + hybrid->rx_drop_credit[1] < datagrams) {\n        uint32_t drops = tt_rx_receiver_drops(node);",
        "    if (false) {\n        uint32_t drops = tt_rx_receiver_drops(node);",
        SEND,
    ),
    (
        "reader: receiver drops never let go",
        "src/tickle.c",
        "    hybrid->rx_drop_credit[1] = hybrid->rx_drop_credit[0] + fresh;",
        "    hybrid->rx_drop_credit[1] += hybrid->rx_drop_credit[0] + fresh;",
        SEND,
    ),
    (
        "drain: sized for a fragment, not a run",
        "src/tickle.c",
        "    (void)tt_rx_drain_start(node, longest);",
        "    (void)longest;\n    (void)tt_rx_drain_start(node, tt_CONTROL_MAX_LENGTH);",
        SEND,
    ),
    (
        "drain: sized for a run where no run can come",
        "src/tickle.c",
        "    uint32_t longest = node->ip_reassembly_max > (uint32_t)tt_CONTROL_MAX_LENGTH ? (uint32_t)node->ip_reassembly_max",
        "    uint32_t longest = true ? (uint32_t)tt_MAX_BUFFER_LENGTH",
        SEND,
    ),
    (
        "p_mtu: a sample lost whole never counted",
        "src/tickle.c",
        "        hybrid_rx_missed(node, writer, sample, count);\n",
        "",
        SEND,
    ),
    (
        "p_mtu: a jump counted across a size that changed",
        "src/tickle.c",
        "    if (count != writer->count || jump % count != 0) {",
        "    if (jump % count != 0) {",
        SEND,
    ),
    (
        "p_mtu: a jump of not whole samples counted",
        "src/tickle.c",
        "    if (count != writer->count || jump % count != 0) {",
        "    if (count != writer->count) {",
        SEND,
    ),
    (
        "p_mtu: a jump counted however long",
        "src/tickle.c",
        "    if (unseen > HYBRID_MISSED_MAX) {",
        "    if (false) {",
        SEND,
    ),
    (
        "p_mtu: the bound one sample short",
        "src/tickle.c",
        "    if (unseen > HYBRID_MISSED_MAX) {",
        "    if (unseen >= HYBRID_MISSED_MAX) {",
        SEND,
    ),
    (
        "p_mtu: a jump counted as one sample, however many",
        "src/tickle.c",
        "    for (uint32_t missed = unseen; missed != 0; missed--) {",
        "    for (uint32_t missed = unseen != 0 ? 1U : 0U; missed != 0; missed--) {",
        SEND,
    ),
    (
        "p_mtu: a sample lost whole without its tail",
        "src/tickle.c",
        "        hybrid_rx_gap(node, writer, 0, count); // all of it: its runs, and its tail",
        "        hybrid_rx_gap(node, writer, 0, count - 1U); // all of it: its runs, and its tail",
        SEND,
    ),
    (
        "p_mtu: a sample lost whole without its runs",
        "src/tickle.c",
        "        hybrid_rx_gap(node, writer, 0, count); // all of it: its runs, and its tail",
        "        hybrid_rx_gap(node, writer, count - 1U, count); // all of it: its runs, and its tail",
        SEND,
    ),
    (
        "p_mtu: the jump not across 2^32",
        "src/tickle.c",
        "    uint32_t jump = sample - (writer->sample + writer->count); // modulo 2^32: right across the wrap",
        "    uint32_t jump = sample > writer->sample ? sample - (writer->sample + writer->count) : UINT32_MAX;",
        SEND,
    ),
    (
        "p_mtu: a repair counted as a first transmission",
        "src/tickle.c",
        "    if (node->rx_targeted) {\n        return;\n    }\n    struct tt_HybridState* hybrid = &node->hybrid;\n"
        "    struct tt_HybridRxWriter* writer = NULL;",
        "    struct tt_HybridState* hybrid = &node->hybrid;\n    struct tt_HybridRxWriter* writer = NULL;",
        SEND,
    ),
]




def build_and_run(binary, control=None):
    build = run_capped(["make", "-s", f"obj/debug/tests/{binary}"], timeout=900, cwd=LINUX)
    if build.returncode != 0:
        return "BUILD_FAILED", build.output[-2000:]
    if unchanged(control, TESTS / binary):
        return NOT_COMPILED, ""
    run = run_capped([str(TESTS / binary)], timeout=TIMEOUT_S, cwd=LINUX)
    if run.timed_out:
        return "HANG", f"(stopped after {TIMEOUT_S} s)\n" + run.output[-1000:]
    if run.returncode is None or run.returncode < 0:
        return "CRASH", run.output
    return ("PASS" if run.returncode == 0 else "FAIL"), run.output


def main():
    be_nice()
    check_anchors(ROOT, [(name, rel, old, new) for name, rel, old, new, _binary in MUTANTS])
    only = os.environ.get("MUTANTS", "")
    shard = None
    if os.environ.get("MUTANTS_SHARD"):
        k, n = (int(part) for part in os.environ["MUTANTS_SHARD"].split("/"))
        if not 0 <= k < n:
            print(f"REFUSED: MUTANTS_SHARD={k}/{n} names no shard")
            return 2
        shard = (k, n)
    failures = 0
    ran = 0
    digests = {}
    with SourceGuard(ROOT, {m[1] for m in MUTANTS}) as guard:
        for binary in sorted({m[4] for m in MUTANTS}):
            verdict, output = build_and_run(binary)
            print(f"[control]  {binary:<28} unmutated -> {verdict}", flush=True)
            if verdict != "PASS":
                print(output[-2000:])
                print("control did not pass; the sweep cannot decide anything")
                return 1
            digests[binary] = code_digest(TESTS / binary)
        for index, (name, rel, old, new, binary) in enumerate(MUTANTS):
            if only and not any(part in name for part in only.split("|")):
                continue
            if shard is not None and index % shard[1] != shard[0]:
                continue
            ran += 1
            (ROOT / rel).write_text(guard.original(rel).replace(old, new, 1))
            try:
                verdict, output = build_and_run(binary, digests[binary])
            finally:
                guard.restore()
            caught = verdict in ("FAIL", "CRASH", "HANG")
            print(f"[{label(verdict, caught, 'caught', 'MISSED')}]   {name:<72} {binary} -> {verdict}", flush=True)
            if caught:
                print(f"  killed by: {', '.join(killed_by(output, ROOT)) or '(no failing check printed)'}", flush=True)
            else:
                print(output[-1500:], flush=True)
            failures += 0 if caught else 1
    for binary in sorted({m[4] for m in MUTANTS}):
        run_capped(["make", "-s", f"obj/debug/tests/{binary}"], timeout=900, cwd=LINUX)  # leave the parent's build
    print(usage_line())
    print(f"{ran} mutants, {failures} not caught" if failures else f"all {ran} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

#!/bin/bash
# Mutants for tests/test_segment_skip.c: each removes one decision of the segment drain's skip-to-newest, and the
# test binary must fail on every one. A control run on the unmodified source must pass. Restores the source on exit.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/tickle.c"
BIN="$ROOT/platform/linux/obj/debug/tests/test_segment_skip"
BACKUP=$(mktemp)
cp -p "$SRC" "$BACKUP"
trap 'cp "$BACKUP" "$SRC"; touch "$SRC"; rm -f "$BACKUP"' EXIT

run() {
    touch "$SRC" # a restored copy keeps its mtime; make must not reuse the mutant's binary
    make -s -C "$ROOT/platform/linux" "obj/debug/tests/test_segment_skip" >/dev/null 2>&1 || { echo "BUILD_FAIL"; return; }
    if "$BIN" >/dev/null 2>&1; then echo "PASS"; else echo "FAIL"; fi
}

mutate() { # name, python replacement (old, new)
    cp "$BACKUP" "$SRC"
    python3 - "$SRC" "$2" "$3" <<'EOF'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
assert s.count(old) == 1, old
open(p, "w").write(s.replace(old, new, 1))
EOF
    printf '%-28s %s\n' "$1" "$(run)"
}

cp "$BACKUP" "$SRC"
printf '%-28s %s\n' "control" "$(run)"
mutate "no-skip" "            node->segment_plan_skip[i / BITS_IN_1BYTE] |= (uint8_t)(1U << (i % BITS_IN_1BYTE));" "            (void)0;"
mutate "no-depth-check" "writer->depth != 0 && writer->newer_complete >= writer->depth" "writer->depth != 0"
mutate "partial-counts-complete" "        rec->kind == SEGMENT_RECORD_WHOLE ||" "        true ||"
mutate "reliable-no-ack" "    (void)update_reliable_ack(node, sub, rec->seq_no, rec->source, rec->entity_id, rec->sender_ip, rec->sender_port);" "    (void)0;"
# KEEP_ALL is refused twice, by the plan (keep_last_depth_of) and again at the moment of reading (visit_skip_allowed),
# because a Subscriber can be created between the two. A single-threaded test cannot show either one alone deciding,
# so this mutant removes both (the second edit applied on top of the first).
mutate "keep-all-plan-only(PASS ok)" "    return (ctx.any && !ctx.keeps_all) ? ctx.depth : 0;" "    return ctx.any ? (ctx.depth ? ctx.depth : 1) : 0;"
python3 - "$SRC" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = "    if (sub->keep_last_depth == 0) {\n        ctx->allowed = false;"
assert s.count(old) == 1
open(p, "w").write(s.replace(old, "    if (false) {\n        ctx->allowed = false;", 1))
EOF
printf '%-28s %s\n' "keep-all-not-honoured(both)" "$(run)"
mutate "no-continuation-skip" "    if (rec.kind == SEGMENT_RECORD_CONT && skipping && skip_continuation(node, &rec, index)) {" "    if (false) {"
mutate "no-hand-back" "        if (node->rx_keep_last_delivered != node->segment_plan_mark) {" "        if (false) {"
# The plan's leading run (segment_skip_run()). Each mutant breaks one thing the run must keep from the record-by-record
# path; the last removes the run altogether, which only makes the drain slower and must still pass.
mutate "run-ignores-writer" "    return run->count != 0 && run->source == rec->source && run->entity_id == rec->entity_id &&" "    return run->count != 0 || run->source == rec->source && run->entity_id == rec->entity_id &&"
# A RELIABLE Subscriber is refused the run twice: by allowed.reliable, and by visit_skip_allowed()'s seq_no check,
# which the run's record (seq_no 0, it stands for many) can only pass once ack_seq_no wraps to 0. The test cannot
# reach that, so removing the first alone passes. The first is kept so the run never rests on the wrap.
mutate "run-takes-reliable(PASS ok)" "    if (!allowed.allowed || !allowed.any || allowed.reliable) {" "    if (!allowed.allowed || !allowed.any) {"
mutate "run-counts-one" "    struct segment_superseded_ctx ctx = {&rec, count};" "    struct segment_superseded_ctx ctx = {&rec, 1U};"
mutate "run-releases-one" "    segment_release_run(node->own_segment, index, count);" "    segment_release(node->own_segment, index + count - 1U);"
mutate "no-run(PASS ok)" "    if (offset == 0 && node->segment_plan_run.count != 0 && !skipping) {" "    if (false) {"

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
mutate "no-skip" "            if (segment_skip_head(node)) {" "            if (false && segment_skip_head(node)) {"
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

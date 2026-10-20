#!/bin/bash
# Mutants for held slots passed over (DESIGN.md section 10, "Held slots"): each removes one decision of
# segment_pass_held() / segment_pass_head() / segment_hold() / segment_unhold(), and test_segment_held or
# test_sample_lending must fail on every one. A control run on the unmodified source must pass. Restores the source
# on exit.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/tickle.c"
TESTS="test_segment_held test_sample_lending"
BACKUP=$(mktemp)
cp -p "$SRC" "$BACKUP"
trap 'cp "$BACKUP" "$SRC"; touch "$SRC"; rm -f "$BACKUP"' EXIT

run() {
    touch "$SRC" # a restored copy keeps its mtime; make must not reuse the mutant's binary
    local verdict="PASS"
    for t in $TESTS; do
        make -s -C "$ROOT/platform/linux" "obj/debug/tests/$t" >/dev/null 2>&1 || { echo "BUILD_FAIL"; return; }
        if ! timeout 120 "$ROOT/platform/linux/obj/debug/tests/$t" >/dev/null 2>&1; then
            verdict="FAIL($t)"
            break
        fi
    done
    echo "$verdict"
}

mutate() { # name, old, new
    cp "$BACKUP" "$SRC"
    python3 - "$SRC" "$2" "$3" <<'EOF'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
assert s.count(old) == 1, old
open(p, "w").write(s.replace(old, new, 1))
EOF
    printf '%-30s %s\n' "$1" "$(run)"
}

cp "$BACKUP" "$SRC"
printf '%-30s %s\n' "control" "$(run)"
# The skip itself: a writer never passes a held slot (the ring is full at it, as before).
mutate "no-writer-pass" "    if (sequence == claimed - slots + 2U) {" "    if (false) {"
# The reader never moves past a passed index: it waits there for a record nobody writes.
mutate "no-reader-pass" \
    "    return header->slots >= SEGMENT_HELD_MIN_SLOTS && (sequence == index + 2U || sequence == index + header->slots);" \
    "    return false;"
# Passed and released before the reader came: index + slots not recognised.
mutate "no-released-pass" \
    "    return header->slots >= SEGMENT_HELD_MIN_SLOTS && (sequence == index + 2U || sequence == index + header->slots);" \
    "    return header->slots >= SEGMENT_HELD_MIN_SLOTS && sequence == index + 2U;"
# The reader moves read_index past write_index when the marking writer has not moved it yet.
mutate "reader-no-write-index" \
    "    uint32_t expected = read_index;
    (void)__atomic_compare_exchange_n(&header->write_index, &expected, read_index + 1U, false, __ATOMIC_ACQ_REL,
                                      __ATOMIC_RELAXED);
    __atomic_store_n(&header->read_index, read_index + 1U, __ATOMIC_RELEASE);" \
    "    __atomic_store_n(&header->read_index, read_index + 1U, __ATOMIC_RELEASE);"
# A writer that finds an index already passed refuses instead of finishing the pass.
mutate "writer-no-help" "    } else if (!segment_seq_passed(header, sequence, claimed)) {" "    } else {"
# The hold leaves the slot published: writers refuse at it a lap later (the old rule).
mutate "hold-no-mark" "    if (header->slots >= SEGMENT_HELD_MIN_SLOTS) {
        __atomic_store_n(&((struct tt_SegmentSlot*)segment_slot(header, index))->sequence, index + 2U," \
    "    if (false) {
        __atomic_store_n(&((struct tt_SegmentSlot*)segment_slot(header, index))->sequence, index + 2U,"
# The release frees the slot for the index it was held at plus one lap, ignoring the passes since.
mutate "unhold-ignores-passes" "        const uint32_t free_at = marked ? sequence - 2U + slots : index + slots;" \
    "        const uint32_t free_at = index + slots;"
# The writer's small-ring guard. Redundant with segment_hold()'s (no mark is ever made in such a ring, and a published
# or free slot of a 1- or 2-slot ring never reads as "held since the last lap" past the index check), so it is
# expected to pass; kept so the writer does not rest on what the reader does.
mutate "no-min-slots(PASS ok)" "    if (slots < SEGMENT_HELD_MIN_SLOTS) {
        return false;
    }
    if (sequence == claimed - slots + 2U) {" "    if (sequence == claimed - slots + 2U) {"

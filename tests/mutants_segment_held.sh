#!/bin/bash
# Mutants for held slots passed over (DESIGN.md section 10, "Held slots"): each removes one decision of
# segment_pass_held() / segment_pass_head() / segment_hold() / segment_unhold(), and test_segment_held or
# test_sample_lending must fail on every one. A control run on the unmodified source must pass. Restores the source
# on exit.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/tickle.c"
TESTS="test_segment_held test_sample_lending test_large_shm"
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
    # An anchor that no longer matches mutates nothing, and the unmutated source would read as a surviving mutant
    # (or, worse, as "PASS ok"): say so instead of running it.
    if ! python3 - "$SRC" "$2" "$3" <<'EOF'; then
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
if s.count(old) != 1:
    sys.exit(1)
open(p, "w").write(s.replace(old, new, 1))
EOF
        printf '%-30s %s\n' "$1" "ANCHOR_MISSING"
        return
    fi
    printf '%-30s %s\n' "$1" "$(run)"
}

cp "$BACKUP" "$SRC"
printf '%-30s %s\n' "control" "$(run)"
# The skip itself: a writer never passes a held slot (the ring is full at it, as before).
mutate "no-writer-pass" "    if (sequence - (claimed - slots) - 2U <= 1U) {" "    if (false) {"
# A writer passes a writer's or a claim's mark (p + 2) but not the reader's (p + 3): full at it a lap later.
mutate "writer-skips-reader-mark" "    if (sequence - (claimed - slots) - 2U <= 1U) {" \
    "    if (sequence == claimed - slots + 2U) {"
# The reader never moves past a passed index: it waits there for a record nobody writes.
mutate "no-reader-pass" \
    "    return header->slots >= SEGMENT_HELD_MIN_SLOTS &&
           (sequence - index - 2U <= 1U || sequence == index + header->slots);" \
    "    return false;"
# Passed and released before the reader came: index + slots not recognised.
mutate "no-released-pass" \
    "    return header->slots >= SEGMENT_HELD_MIN_SLOTS &&
           (sequence - index - 2U <= 1U || sequence == index + header->slots);" \
    "    return header->slots >= SEGMENT_HELD_MIN_SLOTS && sequence - index - 2U <= 1U;"
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
# The reader's mark (p + 3) released as if unmarked: freed for the held index's own next lap, ignoring the passes.
mutate "unhold-ignores-reader-mark" "        const bool marked = slots >= SEGMENT_HELD_MIN_SLOTS && mark - 2U <= 1U;" \
    "        const bool marked = slots >= SEGMENT_HELD_MIN_SLOTS && mark == 2U;"
# The reader passes a held head without marking it (p + 2 stays): a claim it passed could then be published in place
# behind it and never read (test_sample_lending's claim kept by reference; test_publish_claim decides it too).
mutate "reader-no-mark" "    while (sequence == read_index + 2U) {" "    while (false) {"
mutate "unhold-ignores-passes" "        const uint32_t free_at = marked ? sequence - mark + slots : index + slots;" \
    "        const uint32_t free_at = index + slots;"
# The writer's small-ring guard. Redundant with segment_hold()'s (no mark is ever made in such a ring, and a published
# or free slot of a 1- or 2-slot ring never reads as "held since the last lap" past the index check), so it is
# expected to pass; kept so the writer does not rest on what the reader does.
mutate "no-min-slots(PASS ok)" "    if (slots < SEGMENT_HELD_MIN_SLOTS) {
        return false;
    }
    if (sequence - (claimed - slots) - 2U <= 1U) {" "    if (sequence - (claimed - slots) - 2U <= 1U) {"
# References (tt_SEGMENT_SPAN_REF), read by segment_ref_at_head().
mutate "ref-not-left-at-head" "        if (span == tt_SEGMENT_SPAN_FENCE || span == tt_SEGMENT_SPAN_REF) {
            return false; // left at the head: note_head_stall() takes it
        }
        span = 1;
    }
    *record" "        if (span == tt_SEGMENT_SPAN_FENCE) {
            return false; // left at the head: note_head_stall() takes it
        }
        span = 1;
    }
    *record"
mutate "ref-no-held-check" "                 segment_seq_held_since(header, sequence, desc.index) && sequence - desc.index < SEGMENT_INDEX_HALF &&" \
    "                 sequence - desc.index < SEGMENT_INDEX_HALF &&"
mutate "ref-no-length-check" "desc.length != 0 && desc.length <= header->slot_bytes;" "desc.length <= header->slot_bytes;"
mutate "ref-takes-our-held-slot" \
    "    valid = valid && !(node->lend.held_slots != 0 && lend_holds_slot(node, header, desc.index));" "    (void)0;"
mutate "ref-slot-not-given-back" "    if (!(node->lend.held_slots != 0 && lend_holds_slot(node, header, desc.index))) {
        segment_unhold(header, desc.index);
    }" "    (void)0;"
mutate "ref-kept-slot-given-back" "    if (!(node->lend.held_slots != 0 && lend_holds_slot(node, header, desc.index))) {
        segment_unhold(header, desc.index);
    }" "    segment_unhold(header, desc.index);"

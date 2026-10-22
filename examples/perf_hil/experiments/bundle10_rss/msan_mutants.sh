#!/usr/bin/env bash
# Bundle 10 item 2 (2026-10-22): the control for platform/linux/garbage_ctx_check.c under MemorySanitizer. That check
# passing says no byte the reset leaves unwritten is read - but only if the check can see such a read at all. Each
# mutant below puts back one read of a table the reset no longer writes; MSan must report it (a report whose stack is
# not the HAL's pre-existing sin_zero one, hal_sendmsg). A mutant MSan does not catch means the check is blind there.
#
#   frag_slots    frag_slot_for() walks every slot, not only the claimed ones
#   frag_pool     frag_pool_has() does
#   update_parts  update_parts_any() reads a row whatever its source's count
#   endpoint_idx  a rebuild does not NULL the slots it puts in use
#   endpoint_mask find_endpoint() homes an id with the array's full mask, not the index's (a slot past those in use)
#
# Runs in rmw_tickle's build (garbage_ctx_check_large_msan), fill "none", in the namespace pair given.
# Usage: msan_mutants.sh <ns_a> <ns_b> <broadcast>     Needs clang with MemorySanitizer and `sudo -n ip`.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
NS_A=$1
NS_B=$2
BCAST=$3
TMP=$(mktemp -d "${TMPDIR:-/tmp}/msan_mutants.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
failed=0

# mutant NAME OLD NEW: this tree's tickle.c with OLD (exactly once) replaced by NEW, built and run as the check.
mutant() {
    local name=$1 dir="$TMP/$1"
    mkdir -p "$dir/src" "$dir/platform/linux"
    cp -r "$ROOT/include" "$dir/"
    cp -r "$ROOT/examples" "$dir/"
    cp "$ROOT/src/"*.c "$ROOT/src/"*.h "$dir/src/"
    cp "$ROOT/platform/linux/garbage_ctx_check.c" "$ROOT/platform/linux/garbage_ctx_check.sh" "$dir/platform/linux/"
    # A mutant whose anchor is gone is a check that did not run: it fails the script, not just this function.
    if ! python3 - "$dir/src/tickle.c" "$2" "$3" <<'EOF'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(f"mutant anchor matched {text.count(old)} times, not 1")
open(path, "w").write(text.replace(old, new))
EOF
    then
        echo "FAIL  $name: the mutant was not applied"
        failed=1
        return
    fi
    (cd "$dir/platform/linux" && clang -fsanitize=memory -fsanitize-recover=memory -fsanitize-memory-track-origins \
        -fno-omit-frame-pointer -O1 -g -Dtt_HAL_RX_HINT=1 -DTT_RX_DROP_PERCENT=10 -Dtt_MAX_BUFFER_LENGTH=65507 \
        -Dtt_LARGE_SAMPLES=1 -Dtt_LOCAL_DELIVERY=1 -Dtt_DISCOVERY_OPTIONS=1 \
        -Dtt_ENDPOINT_INDEX_SIZE=4096 -Dtt_MAX_ENDPOINT_COUNT=2048 -Dtt_MAX_SCHEDULER_LENGTH=512 -Dtt_UPDATE_MAX_PARTS=255 \
        -Dtt_MAX_NODES=256 -I../../include -I../../src -I../../examples/set_bool -o gc_mutant garbage_ctx_check.c \
        ../../examples/set_bool/SetBool.c ../../src/encoding.c ../../src/log.c ../../src/tickle.c ../../src/hal_linux.c \
        -lm -lpthread 2>"$dir/build.log") || {
        echo "FAIL  $name: does not build"
        tail -5 "$dir/build.log"
        failed=1
        return
    }
    MSAN_OPTIONS=halt_on_error=0 GC_ROUND_S=10 "$dir/platform/linux/garbage_ctx_check.sh" "$NS_A" "$NS_B" "$BCAST" \
        gc_mutant none "$dir/run" >"$dir/run.out" 2>&1
    local reports
    reports=$(cat "$dir/run-a.log" "$dir/run-b.log" | grep -A2 'WARNING: MemorySanitizer' | grep '#1 ' |
        grep -vc 'hal_sendmsg .*hal_linux.c')
    local where
    where=$(cat "$dir/run-a.log" "$dir/run-b.log" | grep -A1 'WARNING: MemorySanitizer' | grep '#0 ' |
        grep -v sendmsg | sed 's/.* in //; s/ .*//' | sort | uniq -c | sort -rn | head -3 | tr '\n' ' ')
    if [ "$reports" -gt 0 ]; then
        printf 'PASS  %-13s caught: %s reports outside hal_sendmsg (%s)\n' "$name" "$reports" "$where"
    else
        printf 'FAIL  %-13s not caught: no report outside hal_sendmsg\n' "$name"
        failed=1
    fi
}

mutant frag_slots "    for (uint32_t i = 0; i < node->frag_slots_used; i++) {
        struct tt_FragSlot* slot = &node->frag_slots[i];" "    for (uint32_t i = 0; i < tt_FRAG_REASSEMBLY_SLOTS; i++) {
        struct tt_FragSlot* slot = &node->frag_slots[i];"
mutant frag_pool "    for (uint32_t i = 0; i < node->frag_slots_used; i++) {
        const struct tt_FragSlot* slot = &node->frag_slots[i];" "    for (uint32_t i = 0; i < tt_FRAG_REASSEMBLY_SLOTS; i++) {
        const struct tt_FragSlot* slot = &node->frag_slots[i];"
mutant update_parts "    if (node->update_part_count[source] == 0) {
        return false;
    }
    for (uint32_t word = 0;" "    for (uint32_t word = 0;"
mutant endpoint_idx "    for (uint32_t i = 0; i <= mask; i++) {
        node->endpoint_index[i] = NULL;
    }" "    (void)0;"
mutant endpoint_mask "    uint32_t slot = endpoint_id & mask;
    for (;;) { // ends at an empty slot: the slots in use always outnumber the endpoints
        struct tt_Endpoint* endpoint = node->endpoint_index[slot];
        if (endpoint == NULL) {
            return NULL;
        }" "    uint32_t slot = endpoint_id & (tt_ENDPOINT_INDEX_SIZE - 1);
    for (;;) {
        struct tt_Endpoint* endpoint = node->endpoint_index[slot];
        if (endpoint == NULL) {
            return NULL;
        }"
echo "msan_mutants: $([ "$failed" = 0 ] && echo PASS || echo FAIL)"
exit "$failed"

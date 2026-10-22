#!/usr/bin/env bash
# tt_InitOptions bundle 8 (ROADMAP "Context options"), the segment group: every check tt_Context_create_with() makes on
# tt_SegmentOptions, and every field's reading, are decided by tests/test_init_options_segment.c. Each mutant replaces
# one piece of src/tickle.c (a refusal becomes a log line that lets creation go on; a field's reader reads the config.h
# default instead), rebuilds the test against the mutant, and expects it to FAIL. A mutant that survives is a check or
# a field no test can tell from its absence.
#
# The control first: the test built from the unmutated tickle.c must pass, or a failure below says nothing.
#
# Usage: tests/mutants_init_options_segment.sh     Prints one line per mutant and exits non-zero if any survives.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TEST=test_init_options_segment

# run TICKLE_C: builds tests/$TEST.c against the given tickle.c and runs it; its exit status.
run() {
    local tickle_c=$1 bin="$TMP/$TEST"
    "$CC" -O0 -Wall -Wextra -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/tests" -DTICKLE_C="\"$tickle_c\"" \
        -o "$bin" "$ROOT/tests/$TEST.c" "$ROOT/src/encoding.c" "$ROOT/src/log.c" -lm -lpthread 2> "$TMP/build.log" || {
        cat "$TMP/build.log" >&2
        return 2
    }
    (cd "$ROOT/platform/linux" && timeout 300 "$bin" > "$TMP/run.log" 2>&1)
}

failed=0
if run "$ROOT/src/tickle.c"; then
    printf 'PASS  control   %s passes on the unmutated source\n' "$TEST"
else
    printf 'FAIL  control   %s fails on the unmutated source\n' "$TEST"
    exit 1
fi

# mutant LABEL OLD NEW: OLD (which must occur exactly once in src/tickle.c) becomes NEW.
mutant() {
    local label=$1
    python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" "$2" "$3" << 'PY'
import sys
text = open(sys.argv[1]).read()
old, new = sys.argv[3], sys.argv[4]
if text.count(old) != 1:
    sys.exit(f"anchor {old!r} matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, new))
PY
    local status=0
    run "$TMP/mutant_tickle.c" || status=$?
    if [ "$status" = 1 ]; then
        printf 'PASS  killed    %s\n' "$label"
    else
        printf 'FAIL  survived  %s (exit %s)\n' "$label" "$status"
        failed=1
    fi
}

# The checks.
refusal() {
    mutant "refusal $1" "return reject_option($1" "(void)reject_option($1"
}
refusal '"segment.slot_bytes"'
refusal '"segment.bytes", "holds fewer'
refusal '"segment.bytes", "holds more'
refusal '"segment.dead_reader_ns"'
refusal '"segment.fence_hold_ns"'
refusal '"segment.probe_every_max"'

# Every field's reading: the reader takes the config.h default.
mutant "reading slot_bytes" \
    "return node->opts.segment.slot_bytes != 0 ? node->opts.segment.slot_bytes : (uint32_t)tt_SEGMENT_SLOT_BYTES;" \
    "return (uint32_t)tt_SEGMENT_SLOT_BYTES;"
mutant "reading bytes" "    if (node->opts.segment.bytes == 0) {" "    if (true) {"
mutant "reading fence_hold_ns" "now + node->opts.segment.fence_hold_ns;" "now + tt_SEGMENT_FENCE_HOLD_NS;"
mutant "reading dead_reader_ns" "entry->last_progress_ns >= node->opts.segment.dead_reader_ns &&" \
    "entry->last_progress_ns >= tt_SEGMENT_DEAD_READER_NS &&"
mutant "deriving dead_reader_ns" "    return discovery_silence_of(interval, miss) * 2U / ((miss * 2U) + 1U);" \
    "    return (uint64_t)tt_SEGMENT_DEAD_READER_NS;"
mutant "the context keeping its derived dead_reader_ns" \
    "    node->opts.segment.dead_reader_ns = segment_dead_reader_of(&node->opts.segment, &node->opts.discovery);" ""
mutant "reading drain_per_poll" "const uint32_t drain_per_poll = node->opts.segment.drain_per_poll;" \
    "const uint32_t drain_per_poll = tt_SEGMENT_DRAIN_PER_POLL;"
mutant "a chunk's bound ignoring the poll's" "const uint32_t bound = chunk < left ? chunk : left;" \
    "const uint32_t bound = chunk;"
mutant "reading probe_every_max" "node->segment_probe_every < node->opts.segment.probe_every_max" \
    "node->segment_probe_every < tt_SEGMENT_PROBE_EVERY_MAX"
mutant "reading stall_passes" "node->segment_head_stall_passes == node->opts.segment.stall_passes" \
    "node->segment_head_stall_passes == tt_SEGMENT_STALL_PASSES"
mutant "reading attach_retry_ns" ": node->opts.segment.attach_retry_ns;" ": tt_SEGMENT_ATTACH_RETRY_NS;"
mutant "reading revalidate_ns" "? node->opts.segment.revalidate_ns :" "? tt_SEGMENT_REVALIDATE_NS :"

exit "$failed"

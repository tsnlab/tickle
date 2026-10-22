#!/usr/bin/env bash
# tt_InitOptions bundle 2 (ROADMAP "Context options"), the discovery group: every check tt_Context_create_with() makes on
# tt_DiscoveryOptions, the interop INFO line, and every field's reading are decided by tests/test_init_options_discovery.c.
# Each mutant replaces one piece of src/tickle.c (a refusal becomes a log line that lets creation go on, the INFO line's
# condition becomes false, a field's reader reads 0 and so the default), rebuilds the test against the mutant, and
# expects it to FAIL. A mutant that survives is a check or a field no test can tell from its absence.
#
# The control first: the test built from the unmutated tickle.c must pass, or a failure below says nothing.
#
# Usage: tests/mutants_init_options_discovery.sh     Prints one line per mutant and exits non-zero if any survives.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TEST=test_init_options_discovery

# run TICKLE_C: builds tests/$TEST.c against the given tickle.c and runs it; its exit status.
run() {
    local tickle_c=$1 bin="$TMP/$TEST"
    "$CC" -O0 -Wall -Wextra -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/tests" -DTICKLE_C="\"$tickle_c\"" \
        -o "$bin" "$ROOT/tests/$TEST.c" "$ROOT/src/encoding.c" "$ROOT/src/log.c" -lm 2> "$TMP/build.log" || {
        cat "$TMP/build.log" >&2
        return 2
    }
    (cd "$ROOT/platform/linux" && "$bin" > "$TMP/run.log" 2>&1)
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
refusal 'durations[i].field'
refusal '"discovery.tx_interval_ns"'
mutant "the interop INFO line" \
    "    if (discovery->update_interval_ns != tt_CONTEXT_UPDATE_INTERVAL ||
        discovery->miss_threshold != tt_LIVELINESS_MISS_THRESHOLD) {" \
    "    if (false) {"

# Every field's reading: the reader takes 0, which is the default.
reader() {
    mutant "reading $2" "    $1 value = node->opts.discovery.$2;" "    $1 value = 0;"
}
reader uint64_t cycle_ns
reader uint64_t update_interval_ns
reader uint64_t tx_interval_ns
reader uint64_t max_lease_ns
reader uint64_t request_retry_ns
reader uint64_t liveliness_defer_ns
reader uint64_t sched_full_warn_interval_ns
reader uint8_t lease_divisor
reader uint8_t request_attempts
reader uint8_t liveliness_max_deferrals
reader uint8_t miss_threshold

exit "$failed"

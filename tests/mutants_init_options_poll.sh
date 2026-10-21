#!/usr/bin/env bash
# tt_InitOptions bundle 6, the poll group (struct tt_PollOptions): every check tt_Context_create_with() makes on it is
# decided by tests/test_init_options_poll.c, and so is every read of a field - no field written and never read.
#
#   check mutants   one refusal turned into a log line that lets creation go on (`return reject_option(...)` becomes
#                   `(void)reject_option(...)`);
#   reader mutants  one place that reads a field made to read the compiled default instead (or the resolver to
#                   resolve a zero to something else).
# Each rebuilds the test against the mutated tickle.c and expects it to FAIL (exit 1). A mutant that survives is a
# check, or a read, that no test can tell from its absence.
#
# The control first: the test built from the unmutated tickle.c must pass, or a failure below says nothing.
#
# Not covered here, and said so: the segment drain's read of rx_lock_chunk (drain_own_segment()) - the same local the
# socket drain's chunk is, driven only by a shared-memory ring, which the mock test does not build.
#
# Usage: tests/mutants_init_options_poll.sh     Prints one line per mutant and exits non-zero if any survives.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TEST=test_init_options_poll

# run TICKLE_C: builds the test against the given tickle.c and runs it; its exit status (2: it did not build).
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

# replace NAME OLD NEW: tickle.c with OLD (which must occur exactly once) replaced by NEW; the test must fail.
replace() {
    local name=$1
    python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" "$2" "$3" <<'EOF'
import sys
text = open(sys.argv[1]).read()
if text.count(sys.argv[3]) != 1:
    sys.exit(f"anchor {sys.argv[3]!r} matched {text.count(sys.argv[3])} times, not 1")
open(sys.argv[2], "w").write(text.replace(sys.argv[3], sys.argv[4]))
EOF
    local status=0
    run "$TMP/mutant_tickle.c" || status=$?
    if [ "$status" = 1 ]; then
        printf 'PASS  killed    %s\n' "$name"
    else
        printf 'FAIL  survived  %s (exit %s)\n' "$name" "$status"
        failed=1
    fi
}

# check FIELD: the refusal naming FIELD no longer returns.
check() {
    replace "check $1" "return reject_option($1" "(void)reject_option($1"
}

check '"poll.rx_clock_refresh"'
check '"poll.rx_lock_chunk"'
check '"poll.receive_timeout"'
check '"poll.scheduler_io_interleave"'

replace "read: drain_rx() ignores rx_clock_refresh" \
    "const uint32_t refresh = poll_rx_clock_refresh(node);" "const uint32_t refresh = tt_RX_CLOCK_REFRESH;"
replace "read: drain_rx() ignores rx_lock_chunk" \
    "const uint32_t chunk = poll_rx_lock_chunk(node);
    while (true) {" "const uint32_t chunk = tt_RX_LOCK_CHUNK;
    while (true) {"
replace "read: node_poll() ignores scheduler_io_interleave" \
    "const uint32_t interleave = poll_scheduler_io_interleave(node);" \
    "const uint32_t interleave = tt_SCHEDULER_IO_INTERLEAVE;"
replace "read: node_poll() ignores receive_timeout" \
    "const uint64_t busy_slice = poll_receive_timeout(node);" "const uint64_t busy_slice = tt_RECEIVE_TIMEOUT;"
replace "read: a context built by hand reads 0, not the default" \
    "return node->opts.poll.rx_clock_refresh != 0 ? node->opts.poll.rx_clock_refresh : (uint32_t)tt_RX_CLOCK_REFRESH;" \
    "return node->opts.poll.rx_clock_refresh;"
replace "resolve: a zero rx_lock_chunk is not the default" \
    "poll->rx_lock_chunk = tt_RX_LOCK_CHUNK;" "poll->rx_lock_chunk = 1;"

exit "$failed"

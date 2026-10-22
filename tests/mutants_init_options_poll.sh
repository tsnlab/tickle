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
    # A mutant may loop (a zero lock chunk drains nothing, forever): 60 s is far past the test's own second.
    (cd "$ROOT/platform/linux" && timeout 60 "$bin" > "$TMP/run.log" 2>&1)
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
    elif [ "$status" = 124 ]; then
        printf 'PASS  killed    %s (the test hung: timeout)\n' "$name"
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
check '"poll.rx_check_ns"'

replace "read: drain_rx() ignores rx_clock_refresh" \
    "const uint32_t refresh = node->opts.poll.rx_clock_refresh;" "const uint32_t refresh = tt_RX_CLOCK_REFRESH;"
replace "read: drain_rx() ignores rx_lock_chunk" \
    "const uint32_t chunk = node->opts.poll.rx_lock_chunk;
    if (len >= 0) {" "const uint32_t chunk = tt_RX_LOCK_CHUNK;
    if (len >= 0) {"
replace "read: node_poll() ignores rx_check_ns" \
    "time - node->rx_looked_ns < node->opts.poll.rx_check_ns" "time - node->rx_looked_ns < tt_RECEIVE_TIMEOUT"
# The receive check by time (2026-10-22, ROADMAP 5a): each condition of it removed.
replace "check: a busy poll never checking its socket" \
    "time - node->rx_looked_ns < node->opts.poll.rx_check_ns" "true"
replace "check: a busy poll checking on every step" \
    "time - node->rx_looked_ns < node->opts.poll.rx_check_ns" "false"
replace "check: an empty check not counted as a look" \
    "            node->rx_looked_ns = time;
            ran_since_look = false;" "            ran_since_look = false;"
replace "check: a check not ending the entries' run" \
    "            node->rx_looked_ns = time;
            ran_since_look = false;" "            node->rx_looked_ns = time;"
replace "check: a wait that timed out not counted as a look" \
    "        node->rx_looked_ns = woke; // the wait looked at the socket until it ended
" ""
replace "check: a wait that timed out counted from its start" \
    "        node->rx_looked_ns = woke; // the wait looked at the socket until it ended" \
    "        node->rx_looked_ns = time;"
replace "check: a wait that brought a datagram not counted as a look" \
    "        node->rx_looked_ns = node->rx_clock_ns; // and it looked at the socket until then (node_poll(), rx_check_ns)
" ""
replace "check: the look forgotten at each call" \
    "    bool ran_since_look = false; // ... since the last receive check, in this call
" "    bool ran_since_look = false; // ... since the last receive check, in this call
    node->rx_looked_ns = time;
"
replace "check: two checks with no entry between" \
    "        if (!ran_since_look || time - node->rx_looked_ns < node->opts.poll.rx_check_ns) {" \
    "        if (time - node->rx_looked_ns < node->opts.poll.rx_check_ns) {"
replace "resolve: a zero rx_check_ns is not the slice" \
    "poll->rx_check_ns = poll->receive_timeout;" "poll->rx_check_ns = tt_POLL_RECEIVE_TIMEOUT_MAX;"
replace "read: node_poll() ignores receive_timeout" \
    "time - poll_start >= node->opts.poll.receive_timeout" "time - poll_start >= tt_RECEIVE_TIMEOUT"
# The line alone: the groups node_init_locks() resolves after it changed with bundles 7 and 8, and an anchor that took
# the next line with it matched nothing from then on.
replace "init: a context built by hand keeps a zero poll group" \
    "    resolve_poll_options(&node->opts.poll);
" ""
replace "resolve: a zero rx_lock_chunk is not the default" \
    "poll->rx_lock_chunk = tt_RX_LOCK_CHUNK;" "poll->rx_lock_chunk = 1;"

exit "$failed"

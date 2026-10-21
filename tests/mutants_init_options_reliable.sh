#!/usr/bin/env bash
# tt_InitOptions bundle 3 (ROADMAP "Context options"), the reliable group: every check tt_Context_create_with() makes on
# struct tt_ReliableOptions is decided by a test, and every field is read where it should be.
#   - check mutants turn one refusal into a log line that lets creation go on (its `return reject_option(...)` becomes
#     `(void)reject_option(...)`);
#   - reader mutants put the build's constant back where a reader takes the context's field, so the field is written and
#     never read there.
# Each rebuilds tests/test_init_options_reliable.c against the mutant and expects it to FAIL. A mutant that survives is
# a check, or a reader, no test can tell from its absence.
#
# The control first: the test built from the unmutated tickle.c must pass, or a failure below says nothing.
#
# Usage: tests/mutants_init_options_reliable.sh     Prints one line per mutant and exits non-zero if any survives.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TEST=test_init_options_reliable

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

# mutant NAME OLD NEW: OLD (which must occur in tickle.c exactly once) becomes NEW.
mutant() {
    local name=$1 old=$2 new=$3
    python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" "$old" "$new" <<'EOF'
import sys
text = open(sys.argv[1]).read()
old, new = sys.argv[3], sys.argv[4]
if text.count(old) != 1:
    sys.exit(f"anchor {old!r} matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, new))
EOF
    local status=0
    run "$TMP/mutant_tickle.c" || status=$?
    if [ "$status" = 1 ]; then
        printf 'PASS  killed    %s\n' "$name"
    else
        printf 'FAIL  survived  %s (%s exit %s)\n' "$name" "$TEST" "$status"
        failed=1
    fi
}

# check FIELD: the refusal naming tt_InitOptions.FIELD no longer returns.
check() {
    mutant "check $1" "return reject_option(\"$1\"" "(void)reject_option(\"$1\""
}

check reliable.retry
check reliable.backoff_shift
check reliable.deadline
check reliable.retry_interval
check reliable.retry_initial
check reliable.retry_granularity
check reliable.timer_lateness_initial

mutant "reader deadline" \
    "return reliable->deadline != 0 ? reliable->deadline : reliable->retry_interval;" \
    "return reliable->retry_interval;"
mutant "reader retry_interval" \
    "return reliable->deadline != 0 ? reliable->deadline : reliable->retry_interval;" \
    "return reliable->deadline != 0 ? reliable->deadline : (uint64_t)tt_RELIABLE_RETRY_INTERVAL;"
mutant "reader retry_initial (dynamic interval)" \
    "    if (srtt_ns == 0) {
        return reliable->retry_initial;" \
    "    if (srtt_ns == 0) {
        return (uint64_t)tt_RELIABLE_RETRY_INITIAL;"
mutant "reader retry_initial (repair window)" \
    "        return node->opts.reliable.retry_initial;" \
    "        return (uint64_t)tt_RELIABLE_RETRY_INITIAL;"
mutant "reader retry_initial (publisher)" \
    "return configured != 0 ? configured : reliable_opts(node)->retry_initial;" \
    "return configured != 0 ? configured : (uint64_t)tt_RELIABLE_RETRY_INITIAL;"
mutant "reader retry_granularity" \
    "return granularity_for(reliable_opts(node)->retry_granularity, node);" \
    "return granularity_for((uint64_t)tt_RELIABLE_RETRY_GRANULARITY, node);"
mutant "reader timer_lateness_initial" \
    "return measured != 0 ? (uint64_t)measured : reliable_opts(node)->timer_lateness_initial;" \
    "return measured != 0 ? (uint64_t)measured : (uint64_t)tt_TIMER_LATENESS_INITIAL;"
mutant "reader max_srtt_multiple" \
    "uint64_t ceiling = unit * (uint64_t)reliable->max_srtt_multiple;" \
    "uint64_t ceiling = unit * (uint64_t)tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE;"
mutant "reader retry (reader back-off)" \
    "if (frontier != proxy->retry_frontier || proxy->retry <= reliable->retry) {" \
    "if (frontier != proxy->retry_frontier || proxy->retry <= tt_RELIABLE_RETRY) {"
mutant "reader retry (writer suppression)" \
    "if (pub_keep_all || entry->retry < node->opts.reliable.retry) {" \
    "if (pub_keep_all || entry->retry < tt_RELIABLE_RETRY) {"
mutant "reader backoff_shift" \
    "    } else if (proxy->backoff < shift) {" \
    "    } else if (proxy->backoff < (uint8_t)tt_RELIABLE_RETRY_BACKOFF_SHIFT) {"
mutant "reader backoff_shift (no back-off)" \
    "uint8_t shift = reliable->backoff_shift == tt_RELIABLE_NO_BACKOFF ? 0U : reliable->backoff_shift;
    if (frontier" \
    "uint8_t shift = reliable->backoff_shift;
    if (frontier"
mutant "reader stuck_warn_interval" \
    "now - proxy->stuck_warned_ns >= node->opts.reliable.stuck_warn_interval) {" \
    "now - proxy->stuck_warned_ns >= tt_RELIABLE_STUCK_WARN_INTERVAL) {"
mutant "reader idle_heartbeat_period" \
    "                                 : node->opts.reliable.idle_heartbeat_period;" \
    "                                 : tt_IDLE_HEARTBEAT_PERIOD;"
mutant "reader idle_heartbeat_retry_multiple" \
    "uint64_t floor = (uint64_t)reliable_opts(pub->node)->idle_heartbeat_retry_multiple *" \
    "uint64_t floor = (uint64_t)tt_IDLE_HEARTBEAT_RETRY_MULTIPLE *"

exit "$failed"

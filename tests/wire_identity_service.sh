#!/usr/bin/env bash
# tt_InitOptions bundle 4 (ROADMAP "Context options"): at the default options a context's services behave exactly as
# they did before the service options existed - every datagram, when it went out, every answer and every callback.
#
# Builds tests/wire_identity_service.c (the scenario is described there) against PARENT and against this tree, and
# compares the traces byte for byte:
#
#   control  the parent against itself, two separate builds - identical, or the trace is not deterministic and the
#            comparisons below could not decide anything;
#   change   this tree against the parent - identical;
#   mutant1  this tree with the default seed the options resolve to changed by 1 ms - must DIFFER, or the scenario
#            does not read the client's resolved options;
#   mutant2  this tree with the default deferred-response timeout the options resolve to cut to 4 s - must DIFFER, or
#            the scenario does not read the server's.
#
# Usage: tests/wire_identity_service.sh [PARENT]     PARENT defaults to 25a658b0, the parent of the service options.
# Prints one line per check and exits non-zero if any check fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-25a658b0}
CC=${CC:-gcc}

TMP=$(mktemp -d)
cleanup() {
    git -C "$ROOT" worktree remove --force "$TMP/parent" >/dev/null 2>&1 || true
    rm -rf "$TMP"
}
trap cleanup EXIT

git -C "$ROOT" worktree add --detach "$TMP/parent" "$PARENT" >/dev/null 2>&1

# capture TREE TICKLE_C NAME: the scenario, copied out so that its "test_mock.h" is TREE's, built against TREE's
# headers and the given tickle.c, run; its trace is $TMP/NAME.bin.
capture() {
    local tree=$1 tickle_c=$2 name=$3
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/wire_identity_service.c" "$TMP/src_$name/"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DWIRE_CAPTURE_OUT="\"$TMP/$name.bin\"" -o "$TMP/bin_$name" "$TMP/src_$name/wire_identity_service.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" -lm 2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    "$TMP/bin_$name" > "$TMP/run_$name.log" 2>&1 || {
        tail -5 "$TMP/run_$name.log" >&2
        return 1
    }
}

failed=0
verdict() { # name expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-8s %s\n' "$1" "$(wc -c < "$3") vs $(wc -c < "$4") bytes, $same as expected"
    else
        printf 'FAIL  %-8s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}

# mutant TAG OLD NEW: this tree's tickle.c with OLD (which must occur exactly once) replaced by NEW.
mutant() {
    python3 - "$ROOT/src/tickle.c" "$TMP/$1_tickle.c" "$2" "$3" <<'EOF'
import sys
text = open(sys.argv[1]).read()
if text.count(sys.argv[3]) != 1:
    sys.exit(f"mutant anchor matched {text.count(sys.argv[3])} times, not 1")
open(sys.argv[2], "w").write(text.replace(sys.argv[3], sys.argv[4]))
EOF
    capture "$ROOT" "$TMP/$1_tickle.c" "$1"
}

capture "$TMP/parent" "$TMP/parent/src/tickle.c" parent_a
capture "$TMP/parent" "$TMP/parent/src/tickle.c" parent_b
verdict control same "$TMP/parent_a.bin" "$TMP/parent_b.bin"
# Large enough to have exercised the scenario: a trace this small could not hold its retries and timeouts.
if [ "$(wc -c < "$TMP/parent_a.bin")" -lt 10000 ]; then
    printf 'FAIL  %-8s %s\n' size "the parent's trace is only $(wc -c < "$TMP/parent_a.bin") bytes"
    failed=1
fi

capture "$ROOT" "$ROOT/src/tickle.c" change
verdict change same "$TMP/parent_a.bin" "$TMP/change.bin"

mutant mutant1 "service->call_retry_interval = (uint32_t)tt_CALL_RETRY_INTERVAL;" \
    "service->call_retry_interval = (uint32_t)tt_CALL_RETRY_INTERVAL + 1000000U;"
verdict mutant1 differ "$TMP/parent_a.bin" "$TMP/mutant1.bin"
mutant mutant2 "service->server_deferred_response_timeout = (uint64_t)tt_SERVER_DEFERRED_RESPONSE_TIMEOUT;" \
    "service->server_deferred_response_timeout = 4 * tt_SECOND;"
verdict mutant2 differ "$TMP/parent_a.bin" "$TMP/mutant2.bin"

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"

#!/usr/bin/env bash
# tt_InitOptions bundle 5 (ROADMAP "Context options"): every check on the QoS defaults, and the rule that creation
# uses them, is decided by tests/test_init_options_qos.c. Each mutant changes one line of tickle.c, rebuilds the test
# against it, and expects the test to FAIL. A mutant that survives is a check, or a use, no test can tell from its
# absence.
#
#   qos.depth, qos.keep_all_depth   the refusal becomes a log line that lets creation go on;
#   storage refusal                 a Publisher's short storage is no longer refused - built with AddressSanitizer as
#                                   well, where the write past the storage itself must be caught;
#   compiled defaults               a Publisher's creation resolves with config.h's defaults, not the context's.
#
# The control first: the test built from the unmutated tickle.c, plain and with AddressSanitizer, must pass, or a
# failure below says nothing.
#
# Usage: tests/mutants_init_options_qos.sh     Prints one line per mutant and exits non-zero if any survives.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TEST=test_init_options_qos
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# run TICKLE_C [CFLAGS...]: builds the test against the given tickle.c and runs it; its exit status.
run() {
    local tickle_c=$1 bin="$TMP/$TEST"
    shift
    "$CC" -O0 -g -Wall -Wextra "$@" -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/tests" -DTICKLE_C="\"$tickle_c\"" \
        -o "$bin" "$ROOT/tests/$TEST.c" "$ROOT/src/encoding.c" "$ROOT/src/log.c" -lm 2> "$TMP/build.log" || {
        cat "$TMP/build.log" >&2
        return 2
    }
    (cd "$ROOT/platform/linux" && ASAN_OPTIONS=exitcode=1 "$bin" > "$TMP/run.log" 2>&1)
}

ASAN=(-fsanitize=address -fno-omit-frame-pointer)
failed=0
for flags in plain asan; do
    extra=()
    [ "$flags" = asan ] && extra=("${ASAN[@]}")
    if run "$ROOT/src/tickle.c" "${extra[@]}"; then
        printf 'PASS  control   %s (%s) passes on the unmutated source\n' "$TEST" "$flags"
    else
        printf 'FAIL  control   %s (%s) fails on the unmutated source\n' "$TEST" "$flags"
        failed=1
    fi
done
[ "$failed" = 0 ] || exit 1

# mutant NAME OLD NEW [CFLAGS...]: OLD (which must occur exactly once in tickle.c) becomes NEW.
mutant() {
    local name=$1 old=$2 new=$3
    shift 3
    python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" "$old" "$new" <<'EOF'
import sys
text = open(sys.argv[1]).read()
old, new = sys.argv[3], sys.argv[4]
if text.count(old) != 1:
    sys.exit(f"anchor {old!r} matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, new))
EOF
    local status=0
    run "$TMP/mutant_tickle.c" "$@" || status=$?
    # 1: an expectation failed; above 128: the mutant crashed the test (a write past short storage). 2 is a build
    # failure, which says nothing.
    if [ "$status" = 1 ] || [ "$status" -gt 128 ]; then
        printf 'PASS  killed    %s\n' "$name"
    else
        printf 'FAIL  survived  %s (exit %s)\n' "$name" "$status"
        failed=1
    fi
}

mutant qos.depth 'return reject_option("qos.depth"' '(void)reject_option("qos.depth"'
mutant qos.keep_all_depth 'return reject_option("qos.keep_all_depth"' '(void)reject_option("qos.keep_all_depth"'
STORAGE_OLD='qos_log_storage_refusal(topic_name, qos, &actual, storage, storage_size, layout.total);
        return tt_RET_INVALID_ARGUMENT;'
STORAGE_NEW='qos_log_storage_refusal(topic_name, qos, &actual, storage, storage_size, layout.total);'
mutant 'storage refusal' "$STORAGE_OLD" "$STORAGE_NEW"
mutant 'storage refusal (asan)' "$STORAGE_OLD" "$STORAGE_NEW" "${ASAN[@]}"
if [ "$failed" = 0 ] && ! grep -q 'AddressSanitizer: heap-buffer-overflow' "$TMP/run.log"; then
    printf 'FAIL  asan      the unrefused short storage was not caught as a heap-buffer-overflow\n'
    failed=1
fi
mutant 'compiled defaults' 'qos_resolve_for(node, qos, tt_KIND_TOPIC_PUBLISHER, &matched, &actual)' \
    'tt_QosProfile_resolve(qos, tt_KIND_TOPIC_PUBLISHER, &matched, &actual)'

exit "$failed"

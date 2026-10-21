#!/usr/bin/env bash
# tt_InitOptions bundle 7 (ROADMAP "Context options"): every check tt_Context_create_with() makes on the options
# is decided by a test. Each mutant turns one refusal into a log line that lets creation go on (its `return
# reject_option(...)` becomes `(void)reject_option(...)`), rebuilds the test that covers it against the mutant, and
# expects that test to FAIL. A mutant that survives is a check no test can tell from its absence.
#
# The control first: the test built from the unmutated tickle.c must pass, or a failure below says nothing.
#
# Usage: tests/mutants_init_options_hybrid.sh     Prints one line per mutant and exits non-zero if any survives.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# run TEST TICKLE_C: builds tests/TEST.c against the given tickle.c and runs it; its exit status.
run() {
    local test=$1 tickle_c=$2 bin="$TMP/$1"
    "$CC" -O0 -Wall -Wextra -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/tests" -DTICKLE_C="\"$tickle_c\"" \
        -o "$bin" "$ROOT/tests/$test.c" "$ROOT/src/encoding.c" "$ROOT/src/log.c" -lm 2> "$TMP/build.log" || {
        cat "$TMP/build.log" >&2
        return 2
    }
    (cd "$ROOT/platform/linux" && "$bin" > "$TMP/run.log" 2>&1)
}

failed=0
for test in test_init_options_hybrid test_init_options_noflag; do
    if run "$test" "$ROOT/src/tickle.c"; then
        printf 'PASS  control   %s passes on the unmutated source\n' "$test"
    else
        printf 'FAIL  control   %s fails on the unmutated source\n' "$test"
        failed=1
    fi
done
[ "$failed" = 0 ] || exit 1

# mutant TEST ANCHOR: ANCHOR (a reject_option call's text, which must occur exactly once) no longer returns.
mutant() {
    local test=$1 anchor=$2
    python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" "$anchor" <<'EOF'
import sys
text = open(sys.argv[1]).read()
old = "return reject_option(" + sys.argv[3]
if text.count(old) != 1:
    sys.exit(f"anchor {sys.argv[3]!r} matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, "(void)reject_option(" + sys.argv[3]))
EOF
    local status=0
    run "$test" "$TMP/mutant_tickle.c" || status=$?
    if [ "$status" = 1 ]; then
        printf 'PASS  killed    %s\n' "$anchor"
    else
        printf 'FAIL  survived  %s (%s exit %s)\n' "$anchor" "$test" "$status"
        failed=1
    fi
}

mutant test_init_options_hybrid '"hybrid_transport.n_max"'
mutant test_init_options_hybrid '"hybrid_transport.window_frames"'
mutant test_init_options_hybrid '"hybrid_transport.collapse_ratio"'
mutant test_init_options_hybrid '"hybrid_transport.collapse_min_lost"'
mutant test_init_options_hybrid '"hybrid_transport.window_max_runs"'
mutant test_init_options_hybrid '"hybrid_transport.drop"'
mutant test_init_options_hybrid '"hybrid_transport.stale_false_forget"'
mutant test_init_options_hybrid '"hybrid_transport.stale_p_min"'
mutant test_init_options_hybrid '"hybrid_transport.path_alpha"'
mutant test_init_options_hybrid '"hybrid_transport.probe_step"'
mutant test_init_options_hybrid '"hybrid_transport.scale_at"'
mutant test_init_options_hybrid '"hybrid_transport.scale_max"'
mutant test_init_options_hybrid '"hybrid_transport.queue_lifetime_ns"'
mutant test_init_options_hybrid '"hybrid_transport.probe_backoff_max"'
mutant test_init_options_noflag '"hybrid_transport", "is set'

exit "$failed"

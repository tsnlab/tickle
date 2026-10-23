#!/usr/bin/env bash
# The large-sample resend order (DESIGN.md section 4): the wire is the parent's wherever no repair is asked for twice.
#
# Builds tests/wire_identity_resend_order.c - test_large_sample.c's two-context sim, every datagram of both contexts
# captured with its time and destination, as rmw_tickle builds core (65507 B, tt_LARGE_SAMPLES) - against the
# parent's tickle.c and headers and against this tree's, and compares the captures of three scenarios:
#
#   control    the parent against itself, two separate builds, every scenario - identical, or the capture is not
#              deterministic and nothing below decides anything;
#   lossless   this tree against the parent, 5 x 1 MiB and a small sample, nothing lost - identical, bytes and times;
#   tails      each sample's last fragment lost once over a 0 ms path - one request and one resend each - identical;
#   delayed    the same over a 40 ms unicast path, where the reader asks again while the repair is on its way - must
#              DIFFER: the treatment, a request the rule leaves unanswered;
#   mutant     this tree with each large sample's first send starting at its second unit - lossless must DIFFER, or the
#              capture cannot see a change in the large send it exists to watch.
#
# Usage: tests/wire_identity_resend_order.sh [PARENT]     PARENT defaults to 896d4a29, the change's parent on main.
# Prints one line per check; exits non-zero if any fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-896d4a29}
CC=${CC:-gcc}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/parent"
git -C "$ROOT" archive "$PARENT" include src | tar -x -C "$TMP/parent"

# capture TREE TICKLE_C NAME: builds the capture against TREE's headers and the given tickle.c, and runs its scenarios
# into $TMP/NAME.{lossless,tails,delayed}.bin.
capture() {
    local tree=$1 tickle_c=$2 name=$3
    "$CC" -O1 -g -w -I"$tree/include" -I"$tree/src" -I"$ROOT/tests" -DTICKLE_C="\"$tickle_c\"" \
        -o "$TMP/bin_$name" "$ROOT/tests/wire_identity_resend_order.c" "$tree/src/encoding.c" "$tree/src/log.c" \
        -lpthread -lm 2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    (
        ulimit -v 2000000 # the PC is shared: a runaway sim is cut off, not the machine
        "$TMP/bin_$name" "$TMP/$name.lossless.bin" "$TMP/$name.tails.bin" "$TMP/$name.delayed.bin" \
            > "$TMP/run_$name.log" 2>&1
    ) || {
        tail -5 "$TMP/run_$name.log" >&2
        return 1
    }
    grep '^wire_identity_resend_order:' "$TMP/run_$name.log" | sed "s|$TMP/||"
}

failed=0
verdict() { # check expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-18s %s\n' "$1" "$(wc -c < "$3") vs $(wc -c < "$4") bytes, $same as expected"
    else
        printf 'FAIL  %-18s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}

capture "$TMP/parent" "$TMP/parent/src/tickle.c" parent_a
capture "$TMP/parent" "$TMP/parent/src/tickle.c" parent_b
for scenario in lossless tails delayed; do
    verdict "control $scenario" same "$TMP/parent_a.$scenario.bin" "$TMP/parent_b.$scenario.bin"
done

capture "$ROOT" "$ROOT/src/tickle.c" change
verdict "lossless" same "$TMP/parent_a.lossless.bin" "$TMP/change.lossless.bin"
verdict "tails" same "$TMP/parent_a.tails.bin" "$TMP/change.tails.bin"
verdict "delayed" differ "$TMP/parent_a.delayed.bin" "$TMP/change.delayed.bin"

python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" <<'EOF'
import sys
text = open(sys.argv[1]).read()
old = "    large_cursor_destinations(node, pub, next);\n    return true;\n"
new = "    large_cursor_destinations(node, pub, next);\n    pub->large_cursor.next_index = 1;\n    return true;\n"
if text.count(old) != 1:
    sys.exit(f"mutant anchor matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, new))
EOF
if capture "$ROOT" "$TMP/mutant_tickle.c" mutant; then
    verdict "mutant lossless" differ "$TMP/parent_a.lossless.bin" "$TMP/mutant.lossless.bin"
else
    echo "FAIL  mutant             did not build or run"
    failed=1
fi

exit "$failed"

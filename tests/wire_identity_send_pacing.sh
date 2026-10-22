#!/usr/bin/env bash
# Send pacing (docs/DESIGN.md "Send pacing"): a context with no paced link - ETHERNET, or no links configured - sends
# what a context sent before pacing existed: every datagram, its bytes, its destination and the moment it left.
# Captures tests/wire_identity_send_pacing.c's scenario (broadcast and unicast samples, a batch, a long sample, RELIABLE
# samples and retransmissions; in the large build a 256 KB sample through a socket buffer that fills, and retransmissions
# of its units; announces, summaries and HEARTBEATs) in three builds - the core default, -Dtt_MAX_SAMPLE_LENGTH=16000
# (fragments), and rmw_tickle's -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 - with the parent's tickle.c and with
# this tree's, and checks, for each build:
#   control   the parent against itself, two separate builds - identical, or the capture is not deterministic and the
#             comparison below could not decide anything;
#   identity  this tree against the parent - identical, bytes and times;
#   mutant    this tree with ETHERNET paced by default at 10 kbit/s - must DIFFER from the parent, or the scenario does
#             not reach a paced send (the retransmissions held, in every build; the large sample's units, in the large
#             one) and its identity is not shown.
#
# Usage: tests/wire_identity_send_pacing.sh [PARENT]     PARENT defaults to 13511f47, the parent of send pacing: a
# commit, checked out in a temporary worktree, or a directory already holding that tree. Prints one line per check and
# exits non-zero if any check fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-13511f47}
CC=${CC:-gcc}

TMP=$(mktemp -d)
PARENT_TREE="$TMP/parent"
added_worktree=0
cleanup() {
    if [ "$added_worktree" = 1 ]; then
        git -C "$ROOT" worktree remove --force "$PARENT_TREE" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ -d "$PARENT" ]; then
    PARENT_TREE=$(cd "$PARENT" && pwd)
else
    git -C "$ROOT" worktree add --detach "$PARENT_TREE" "$PARENT" >/dev/null 2>&1
    added_worktree=1
fi

# capture TREE TICKLE_C NAME [CFLAGS...]: this tree's capture source, copied out so that its "test_mock.h" is TREE's,
# built against TREE's headers and the given tickle.c, run to write $TMP/NAME.bin.
capture() {
    local tree=$1 tickle_c=$2 name=$3
    shift 3
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/wire_identity_send_pacing.c" "$TMP/src_$name/"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DWIRE_CAPTURE_OUT="\"$TMP/$name.bin\"" "$@" -o "$TMP/bin_$name" "$TMP/src_$name/wire_identity_send_pacing.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" -lm 2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    (
        mkdir -p "$TMP/cwd"
        cd "$TMP/cwd"
        ulimit -v 2097152
        ulimit -f 102400
        timeout 300 "$TMP/bin_$name" > "$TMP/run_$name.log" 2>&1
    ) || {
        echo "FAIL  capture $name did not run:"
        tail -5 "$TMP/run_$name.log"
        return 1
    }
    [ -s "$TMP/$name.bin" ] || {
        echo "FAIL  capture $name wrote nothing"
        return 1
    }
}

failed=0
verdict() { # name expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-16s %s\n' "$1" "$(wc -c < "$3") vs $(wc -c < "$4") bytes, $same as expected"
    else
        printf 'FAIL  %-16s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}

# mutate TAG OLD NEW: this tree's tickle.c with OLD (which must occur exactly once) replaced by NEW, as $TMP/TAG.c.
mutate() {
    python3 - "$ROOT/src/tickle.c" "$TMP/$1.c" "$2" "$3" <<'EOF'
import sys
text = open(sys.argv[1]).read()
if text.count(sys.argv[3]) != 1:
    sys.exit(f"mutant anchor matched {text.count(sys.argv[3])} times, not 1")
open(sys.argv[2], "w").write(text.replace(sys.argv[3], sys.argv[4]))
EOF
}
mutate mutant "link->pacing_rate = t1s ? link_t1s_pacing_rate(link->link_rate) : tt_PACING_OFF;" \
    "link->pacing_rate = t1s ? link_t1s_pacing_rate(link->link_rate) : 10000U;"

for build in core frag large; do
    flags=()
    [ "$build" = frag ] && flags=(-Dtt_MAX_SAMPLE_LENGTH=16000)
    [ "$build" = large ] && flags=(-Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1)
    capture "$PARENT_TREE" "$PARENT_TREE/src/tickle.c" "${build}_parent_a" "${flags[@]}"
    capture "$PARENT_TREE" "$PARENT_TREE/src/tickle.c" "${build}_parent_b" "${flags[@]}"
    capture "$ROOT" "$ROOT/src/tickle.c" "${build}_this" "${flags[@]}"
    capture "$ROOT" "$TMP/mutant.c" "${build}_mutant" "${flags[@]}"
    printf '      %-16s %s\n' "$build" "$(grep -o 'wire capture: [0-9]* datagrams' "$TMP/run_${build}_this.log")"
    verdict "$build control" same "$TMP/${build}_parent_a.bin" "$TMP/${build}_parent_b.bin"
    verdict "$build identity" same "$TMP/${build}_this.bin" "$TMP/${build}_parent_a.bin"
    verdict "$build mutant" differ "$TMP/${build}_mutant.bin" "$TMP/${build}_parent_a.bin"
done

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"

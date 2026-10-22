#!/usr/bin/env bash
# Link profiles (docs/DESIGN.md "Link profiles"): a link left at the zero profile, ETHERNET, sends what a context sent
# before profiles existed - every datagram, its bytes and its destination.
# Captures tests/wire_identity_link_profiles.c's scenario (an explicitly configured link on an interface reporting MTU
# 1000; broadcast and unicast samples, a batch longer than a datagram, a long sample, 70 RELIABLE samples, 3 s of
# announces, summaries and heartbeats) in two builds - the core default, which does not fragment, and
# -Dtt_MAX_SAMPLE_LENGTH=16000, which does - with the parent's tickle.c and with this tree's, and checks, for each build:
#   control   the parent against itself, two separate builds - identical, or the capture is not deterministic and the
#             comparison below could not decide anything;
#   identity  this tree against the parent - identical;
#   mutant    this tree with a default changed where the scenario must pass through it - must DIFFER from the parent,
#             or the scenario does not reach that code and its identity is not shown:
#               core   ETHERNET's MTU 1500 -> 1400 (the batch and the announce are cut shorter);
#               frag   the default fragment cut 4 bytes shorter (the long samples' fragments);
#             and, in the core build, WIFI's summary copies and announce repeats (2026-10-23) given to every link - must
#             DIFFER, or the scenario does not reach the announce repeats and ETHERNET's identity says nothing about
#             them (the summary's miss-threshold byte is tests/wire_identity_discovery_timing.sh's).
#
# Usage: tests/wire_identity_link_profiles.sh [PARENT]     PARENT defaults to 6c355fa2, the parent of link profiles: a
# commit, checked out in a temporary worktree, or a directory already holding that tree. Prints one line per check and
# exits non-zero if any check fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-6c355fa2}
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
    cp "$ROOT/tests/wire_identity_link_profiles.c" "$TMP/src_$name/"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DWIRE_CAPTURE_OUT="\"$TMP/$name.bin\"" "$@" -o "$TMP/bin_$name" "$TMP/src_$name/wire_identity_link_profiles.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" -lm 2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    (
        cd "$tree/platform/linux"
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
mutate mutant_core "link->mtu = (uint16_t)LINK_ETHERNET_MTU;" "link->mtu = (uint16_t)1400U;"
mutate mutant_frag "node->frag_cont = control - LINK_FRAG_CONT_OVERHEAD;" \
    "node->frag_cont = control - LINK_FRAG_CONT_OVERHEAD - 4U;"
mutate mutant_wifi "bool wifi = link->profile == tt_LINK_PROFILE_WIFI;" "bool wifi = true;"

for build in core frag; do
    flags=()
    [ "$build" = frag ] && flags=(-Dtt_MAX_SAMPLE_LENGTH=16000)
    capture "$PARENT_TREE" "$PARENT_TREE/src/tickle.c" "${build}_parent_a" "${flags[@]}"
    capture "$PARENT_TREE" "$PARENT_TREE/src/tickle.c" "${build}_parent_b" "${flags[@]}"
    capture "$ROOT" "$ROOT/src/tickle.c" "${build}_this" "${flags[@]}"
    capture "$ROOT" "$TMP/mutant_$build.c" "${build}_mutant" "${flags[@]}"
    printf '      %-16s %s\n' "$build" "$(grep -o 'wire capture: [0-9]* datagrams' "$TMP/run_${build}_this.log")"
    verdict "$build control" same "$TMP/${build}_parent_a.bin" "$TMP/${build}_parent_b.bin"
    verdict "$build identity" same "$TMP/${build}_this.bin" "$TMP/${build}_parent_a.bin"
    verdict "$build mutant" differ "$TMP/${build}_mutant.bin" "$TMP/${build}_parent_a.bin"
done
capture "$ROOT" "$TMP/mutant_wifi.c" core_wifi_mutant
verdict "core wifi mutant" differ "$TMP/core_wifi_mutant.bin" "$TMP/core_parent_a.bin"

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"

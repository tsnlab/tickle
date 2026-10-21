#!/usr/bin/env bash
# tt_InitOptions bundle 9 (ROADMAP "Context options"): with the default capacity group every table is filled up to its
# compiled size, as before the group, so a context sends what it sent - every datagram, its bytes and its instant.
# Captures two scenarios with the parent's tickle.c and with this tree's, each test source built from this tree:
#
#   tests/test_wire_identity_reliable.c     a lossy RELIABLE exchange: the reader's writer proxy taken, its ACKNACK
#                                           requests remembered and left out of its answers (request_history), the
#                                           writer's ack entry claimed (ack_entries);
#   tests/test_wire_identity_initoptions.c  a context created through tt_Context_create(), its Publishers' peers past
#                                           the unicast threshold, 3 s of announces, summaries and heartbeats.
#
# and checks, for each:
#   control   the parent against itself, two separate builds - identical, or the capture is not deterministic and the
#             comparison below could not decide anything;
#   identity  this tree against the parent - identical.
# and that the reliable scenario passes through the group's readers at all:
#   mutants   this tree with the writer-proxy reader's limit 0 (no proxy: the reader asks for nothing) and with the
#             ack-entry reader's limit 0 (no entry: the writer waits for nobody) - each must DIFFER from the parent, or
#             the scenario does not reach that reader and its identity is not shown.
# Neither scenario fills a table - one writer, one reader, a few peers - so none of the limits binds in it, and lowering
# one changes nothing there (request_history 1 included: no answer in this exchange depends on an older request). The
# limits binding are shown by tests/test_init_options_capacity.c, test_publish_claim.c and test_large_sample.c.
#
# Usage: tests/wire_identity_initoptions_capacity.sh [PARENT]     PARENT defaults to 05118219, the parent of the group:
# a commit, checked out in a temporary worktree, or a directory already holding that tree (`git archive` of it, for a
# caller that may not add worktrees). Prints one line per check and exits non-zero if any check fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-05118219}
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

# capture SRC TREE TICKLE_C NAME [MAY_FAIL]: SRC (this tree's test source, copied out so that its "test_mock.h" is
# TREE's) built against TREE's headers and the given tickle.c, run to write its default arm's capture to $TMP/NAME.bin.
# With MAY_FAIL (a mutant, whose own checks fail) the run's status is not asked, only that it wrote the capture.
capture() {
    local src=$1 tree=$2 tickle_c=$3 name=$4 may_fail=${5:-}
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/$src" "$TMP/src_$name/"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DWIRE_CAPTURE_OUT="\"$TMP/$name.bin\"" -o "$TMP/bin_$name" "$TMP/src_$name/$src" "$tree/src/encoding.c" \
        "$tree/src/log.c" -lm 2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    (
        cd "$tree/platform/linux"
        ulimit -v 2097152
        ulimit -f 102400
        timeout 300 "$TMP/bin_$name" > "$TMP/run_$name.log" 2>&1
    ) || [ -n "$may_fail" ] || {
        echo "FAIL  capture $name did not run:"
        tail -5 "$TMP/run_$name.log"
        return 1
    }
    [ -s "$TMP/$name.bin" ] || {
        echo "FAIL  capture $name wrote nothing:"
        tail -5 "$TMP/run_$name.log"
        return 1
    }
}

failed=0
verdict() { # name expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-32s %s\n' "$1" "$(wc -c < "$3") vs $(wc -c < "$4") bytes, $same as expected"
    else
        printf 'FAIL  %-32s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}

for scenario in reliable initoptions; do
    src=test_wire_identity_$scenario.c
    capture "$src" "$PARENT_TREE" "$PARENT_TREE/src/tickle.c" "${scenario}_parent_a"
    capture "$src" "$PARENT_TREE" "$PARENT_TREE/src/tickle.c" "${scenario}_parent_b"
    capture "$src" "$ROOT" "$ROOT/src/tickle.c" "${scenario}_this"
    verdict "$scenario control" same "$TMP/${scenario}_parent_a.bin" "$TMP/${scenario}_parent_b.bin"
    verdict "$scenario identity" same "$TMP/${scenario}_this.bin" "$TMP/${scenario}_parent_a.bin"
done

# mutant SCENARIO TAG OLD NEW: this tree's tickle.c with OLD (which must occur exactly once) replaced by NEW, captured.
mutant() {
    local scenario=$1 tag=$2
    python3 - "$ROOT/src/tickle.c" "$TMP/$tag.c" "$3" "$4" <<'EOF'
import sys
text = open(sys.argv[1]).read()
if text.count(sys.argv[3]) != 1:
    sys.exit(f"mutant anchor matched {text.count(sys.argv[3])} times, not 1")
open(sys.argv[2], "w").write(text.replace(sys.argv[3], sys.argv[4]))
EOF
    if capture "test_wire_identity_$scenario.c" "$ROOT" "$TMP/$tag.c" "$tag" may_fail; then
        verdict "$scenario mutant $tag" differ "$TMP/$tag.bin" "$TMP/${scenario}_parent_a.bin"
    else
        failed=1
    fi
}
mutant reliable writer_proxies \
    "int limit = sub->node != NULL ? (int)sub->node->opts.capacity.peers : tt_MAX_PEER_COUNT;" "int limit = 0;"
mutant reliable ack_entries "int limit = (int)pub->node->opts.capacity.ack_entries;" "int limit = 0;"

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"

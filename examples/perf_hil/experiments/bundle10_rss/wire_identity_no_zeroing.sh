#!/usr/bin/env bash
# Bundle 10 item 2 (2026-10-22, the user's decision): a context no longer writes its large tables in full at creation
# and reset. Nothing it sends may change: this checks the default wire, byte for byte and destinations included, of
# every wire-identity scenario in tests/, built against BASE's tickle.c and this tree's.
#
#   control   BASE against itself, two separate builds - identical, or the capture is not deterministic and the
#             comparison below could not decide anything;
#   change    this tree against BASE - identical;
#   mutant    this tree with the ip_reassembly_max a tt_Context_create() context advertises changed - must DIFFER, or
#             the capture does not see what a context announces (run on the initoptions scenario, whose capture is
#             that context's).
#
# Scenarios: test_wire_identity_initoptions.c (creation, best effort and RELIABLE pub/sub), test_wire_identity_reliable.c
# (a lossy RELIABLE exchange reaching every reader), test_wire_identity_largemsg.c (rmw_tickle's build: large samples
# and fragments), wire_identity_service.c (calls, answers, retries). Each is the capture tool its own wire_identity_*.sh
# uses; the mock HAL writes what core sends to WIRE_CAPTURE_OUT.
#
# Usage: wire_identity_no_zeroing.sh [BASE]    BASE defaults to where this branch left origin/main. One line per check; non-zero on any FAIL.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
BASE=${1:-$(git -C "$ROOT" merge-base HEAD origin/main)}
CC=${CC:-gcc}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/wire_identity_no_zeroing.XXXXXX")
cleanup() {
    git -C "$ROOT" worktree remove --force "$TMP/base" >/dev/null 2>&1 || true
    rm -rf "$TMP"
}
trap cleanup EXIT
git -C "$ROOT" worktree add --detach "$TMP/base" "$BASE" >/dev/null 2>&1
echo "wire_identity_no_zeroing: base $(git -C "$TMP/base" rev-parse --short HEAD), this tree $(git -C "$ROOT" rev-parse --short HEAD) + working tree"

# capture SOURCE TREE TICKLE_C NAME: SOURCE (this tree's tests/) built against TREE's headers and TICKLE_C, then run.
capture() {
    local src=$1 tree=$2 tickle_c=$3 name=$4
    "$CC" -O0 -g -w -I"$tree/include" -I"$tree/src" -I"$ROOT/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DWIRE_CAPTURE_OUT="\"$TMP/$name.bin\"" -o "$TMP/bin_$name" "$ROOT/tests/$src" "$tree/src/encoding.c" \
        "$tree/src/log.c" -lm 2>"$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    "$TMP/bin_$name" >"$TMP/run_$name.log" 2>&1 || true # the capture is the artefact; a test verdict is not asked here
    [ -s "$TMP/$name.bin" ] || {
        echo "FAIL  $name: no capture written" >&2
        return 1
    }
}

failed=0
verdict() { # label expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-34s %s\n' "$1" "$(wc -c <"$3") vs $(wc -c <"$4") bytes, $same as expected"
    else
        printf 'FAIL  %-34s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}

for src in test_wire_identity_initoptions.c test_wire_identity_reliable.c test_wire_identity_largemsg.c \
    wire_identity_service.c; do
    tag=${src%.c}
    capture "$src" "$TMP/base" "$TMP/base/src/tickle.c" "${tag}_base_a"
    capture "$src" "$TMP/base" "$TMP/base/src/tickle.c" "${tag}_base_b"
    capture "$src" "$ROOT" "$ROOT/src/tickle.c" "${tag}_new"
    verdict "$tag control" same "$TMP/${tag}_base_a.bin" "$TMP/${tag}_base_b.bin"
    verdict "$tag change" same "$TMP/${tag}_base_a.bin" "$TMP/${tag}_new.bin"
done

python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" <<'EOF'
import sys
text = open(sys.argv[1]).read()
old = "_tt_CONFIG.ip_reassembly_max != 0 ? _tt_CONFIG.ip_reassembly_max : tt_IP_REASSEMBLY_NONE;"
if text.count(old) != 1:
    sys.exit(f"mutant anchor matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, "_tt_CONFIG.ip_reassembly_max != 0 ? (uint16_t)1000U : tt_IP_REASSEMBLY_NONE;"))
EOF
capture test_wire_identity_initoptions.c "$ROOT" "$TMP/mutant_tickle.c" mutant
verdict "test_wire_identity_initoptions mutant" differ "$TMP/test_wire_identity_initoptions_base_a.bin" "$TMP/mutant.bin"
echo "wire_identity_no_zeroing: $([ "$failed" = 0 ] && echo PASS || echo FAIL)"
exit "$failed"

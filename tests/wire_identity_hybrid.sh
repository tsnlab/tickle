#!/usr/bin/env bash
# The hybrid transport's runs on the wire are byte-identical to a parent's (DESIGN.md section 8, "Hybrid transport"):
# for a change to how runs are sent - how many go in one send call, when a full send buffer is retried - that must
# leave every datagram as it was.
#
# Builds this tree's tests/test_hybrid_send.c with -DHYBRID_SEND_WIRE_PRINT against three cores and compares the wire
# digests of its byte-identity arms (4 MB in runs of 45, a send buffer of three datagrams, RELIABLE forced IP at 1%
# frame loss, two unicast readers in both orders; each arm with and without the ranged encode). A digest is an FNV-1a
# hash over every datagram's bytes and destination, in order, plus the count and the byte total. Time fields come from
# the mock clock, so they match only if the two builds behave alike.
#
#   control  the parent against itself, two builds - identical, or the capture is not deterministic and the comparison
#            below cannot decide anything;
#   change   this tree against the parent - identical;
#   mutant   this tree with every run's `zero` field set to 1 - must DIFFER, or the digests cannot see a run's bytes.
#
# The test's own assertions (call counts, range ends) may fail against the parent; only the digests are read. Each run
# under `ulimit -v` and a timeout.
#
# Usage: tests/wire_identity_hybrid.sh [PARENT]    PARENT defaults to c7262c89 (origin/main before ab/hybrid-v2).
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-c7262c89}
CC=${CC:-gcc}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/wire_identity_hybrid.XXXXXX")
cleanup() { # the one EXIT trap
    rm -rf "$TMP"
}
trap cleanup EXIT

mkdir -p "$TMP/parent" "$TMP/change" "$TMP/mutant"
git -C "$ROOT" archive "$PARENT" src include | tar -x -C "$TMP/parent" || exit 1
cp -r "$ROOT/src" "$ROOT/include" "$TMP/change/"
cp -r "$ROOT/src" "$ROOT/include" "$TMP/mutant/"
if [ "$(grep -c 'header_l->zero = 0;' "$TMP/mutant/src/tickle.c")" != 1 ]; then
    echo "FAIL  mutant   the substitution site is not there exactly once"
    exit 1
fi
sed -i 's/header_l->zero = 0;/header_l->zero = 1;/' "$TMP/mutant/src/tickle.c"

# digest NAME CORE: the test against CORE's sources, its WIRE lines in $TMP/NAME.wire.
digest() {
    local name=$1 core=$2
    mkdir -p "$TMP/$name.build/tests"
    cp "$ROOT"/tests/test_hybrid_send.c "$ROOT"/tests/test_common.h "$ROOT"/tests/test_mock.h "$TMP/$name.build/tests/"
    ln -sfn "$core/src" "$TMP/$name.build/src"
    "$CC" -O1 -g -w -DHYBRID_SEND_WIRE_PRINT -I"$core/include" -I"$core/src" -o "$TMP/$name.bin" \
        "$TMP/$name.build/tests/test_hybrid_send.c" "$core/src/encoding.c" "$core/src/log.c" -lm \
        >"$TMP/$name.build.log" 2>&1 || {
        cat "$TMP/$name.build.log" >&2
        return 1
    }
    (
        ulimit -v 2097152
        timeout 600 "$TMP/$name.bin" >"$TMP/$name.log" 2>&1
    )
    grep '^WIRE ' "$TMP/$name.log" >"$TMP/$name.wire"
    [ -s "$TMP/$name.wire" ] || {
        echo "FAIL  $name   no WIRE line: the capture did not run ($(tail -1 "$TMP/$name.log"))"
        return 1
    }
}

failed=0
digest parent "$TMP/parent" || exit 1
digest parent2 "$TMP/parent" || exit 1
digest change "$TMP/change" || exit 1
digest mutant "$TMP/mutant" || exit 1

verdict() { # name expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-8s %s arms, %s as expected\n' "$1" "$(wc -l <"$3")" "$same"
    else
        printf 'FAIL  %-8s %s, expected %s\n' "$1" "$same" "$2"
        diff "$3" "$4" | head -10
        failed=1
    fi
}
verdict control same "$TMP/parent.wire" "$TMP/parent2.wire"
verdict change same "$TMP/parent.wire" "$TMP/change.wire"
verdict mutant differ "$TMP/parent.wire" "$TMP/mutant.wire"
exit $failed

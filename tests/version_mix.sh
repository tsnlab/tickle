#!/usr/bin/env bash
# A tt_VERSION 13 core and a tt_VERSION 14 core refuse each other's discovery announce (QoS plan D9): mixed versions do
# not match, as every earlier version change, rather than one misreading the other's entity records.
#
# Builds tests/version_mix_driver.c against two cores - the parent's (v13) and this tree's (v14), each with its own
# headers and mock HAL - and hands each its own announce (its fuzz corpus seed, from its own encoder) and the other's:
#
#   parent   the v13 core applies its own v13 announce (the control) and refuses this tree's v14 one;
#   change   this tree's v14 core applies its own v14 announce (the control) and refuses the parent's v13 one;
#   mutant   this tree's core handed its own announce as the "other" - must FAIL, or the driver cannot tell an
#            applied announce from a refused one.
#
# Usage: tests/version_mix.sh [PARENT]     PARENT defaults to 461d6558 (ab/qos-5-compat, tt_VERSION 13). Prints one line
# per check and exits non-zero if any check fails.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-461d6558}
CC=${CC:-gcc}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/version_mix.XXXXXX")
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() { # the one EXIT trap
    rm -rf "$TMP"
}
trap cleanup EXIT

mkdir -p "$TMP/parent"
git -C "$ROOT" archive "$PARENT" src include tests | tar -x -C "$TMP/parent" || {
    echo "FAIL  cannot extract $PARENT"
    exit 1
}

# build TREE OUT: the driver against TREE's core, headers and mock HAL.
build() {
    local tree=$1 out=$2
    "$CC" -O0 -g -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tree/src/tickle.c\"" \
        -o "$out" "$ROOT/tests/version_mix_driver.c" "$tree/src/encoding.c" "$tree/src/log.c" -lm \
        2> "$out.build.log" || {
        cat "$out.build.log" >&2
        return 1
    }
}

failed=0
build "$TMP/parent" "$TMP/driver_parent" || failed=1
build "$ROOT" "$TMP/driver_change" || failed=1
if [ "$failed" -ne 0 ]; then
    echo "FAIL  build"
    exit 1
fi

PARENT_SEED="$TMP/parent/tests/fuzz_corpus/announce_single.bin"
CHANGE_SEED="$ROOT/tests/fuzz_corpus/announce_single.bin"

check() { # name expected(pass|fail) driver own other
    local name=$1 expected=$2 line rc
    line=$("$3" "$4" "$5")
    rc=$?
    local got=fail
    [ "$rc" -eq 0 ] && got=pass
    if [ "$got" = "$expected" ]; then
        printf 'PASS  %-7s %s\n' "$name" "$line"
    else
        printf 'FAIL  %-7s expected the driver to %s, it did %s: %s\n' "$name" "$expected" "$got" "$line"
        failed=1
    fi
}

check parent pass "$TMP/driver_parent" "$PARENT_SEED" "$CHANGE_SEED"
check change pass "$TMP/driver_change" "$CHANGE_SEED" "$PARENT_SEED"
check mutant fail "$TMP/driver_change" "$CHANGE_SEED" "$CHANGE_SEED"
exit "$failed"

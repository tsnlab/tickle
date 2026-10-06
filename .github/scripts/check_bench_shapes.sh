#!/usr/bin/env bash
# Checks that the HIL benchmark's four payload shapes really are the same shape on all three
# frameworks (examples/perf_hil/OPTIMIZATION_PLAN.md sections 3 and 4).
#
# Why this exists. The DDS pair cannot drift apart from each other: idlc and fastddsgen compile
# one shared idl/pN/Bench.idl. TickLE used to be able to, because its side was a hand-written
# codec; since 2026-09-25 it is generated from tickle/common/pN/Bench.msg, which removes the codec
# risk but leaves two source files per shape that a person could edit one of. That is what this
# closes, and it is the same class of guard as check-doc-shas: cheap, and it fails loudly.
#
# Three things are checked per shape, and each one has failed somewhere in this tree before:
#   1. the payload array is the same length in the .msg and the .idl
#   2. `send_ns` comes before `seq` in both - the CDR-4 vs CDR-8 padding difference that would
#      otherwise put 76 bytes on TickLE's wire and 80 on both vendors' for the same source
#   3. the shape's total is the size the campaign says it is, and the generated header agrees
#   4. each shape still lands on the side of the MTU its design depends on, with real margin -
#      for TickLE computed from its own wire structs and datagram limit, so a framing change that
#      would move P3 off its premise fails here (ROADMAP "P3 bench shape has 3.4 B of headroom")
#   5. every CycloneDDS/FastDDS binding generated on THIS host declares the IDL's payload size
#
# 5 was added on 2026-09-25, after the P2 resize ran for a day without reaching either DDS harness
# on the rig (see examples/perf_hil/bench_shape.sh). Those bindings are gitignored and generated
# per host, so this can only check the ones this host has - which is why it is worth running on the
# rig before a sweep, and why it names every binding it did not find rather than passing quietly.
# build.sh now refuses to compile a mismatched binding as well; this is the check that says so
# before a sweep starts rather than at its first build.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IDL_DIR="$HERE/examples/perf_hil/idl"
MSG_DIR="$HERE/examples/perf_hil/tickle/common"
# shellcheck disable=SC1091 # repo helper, sourced by path relative to the repo root
. "$HERE/examples/perf_hil/bench_shape.sh"

# Shape -> the total CDR sample size the campaign specifies. 8 (send_ns) + 4 (seq) + the array.
declare -A EXPECTED=([p1]=76 [p2]=1292 [p3]=1424 [p4]=2800)

# Per-sample framing, in tenths of a byte, measured on the rig in the 2026-09-25 campaign at the
# sizes where every framework sent exactly one datagram. It covers Ethernet + IP + UDP (42 bytes)
# as well as each protocol's own headers and its amortised ACK/heartbeat traffic.
#
# These are measurements, not constants, and that is the point of gating with them: a vendor
# upgrade can move them, and when it does this gate fails and says the premise needs re-measuring
# rather than letting a cell quietly change what it tests. **Re-measure after any CycloneDDS or
# FastDDS upgrade.**
#
# TickLE is not in this table any more (2026-10-06). Its figure was measured the same way, 70.6 =
# 42 + 28 B of DATA framing at the time, and it went stale without anything noticing: wire v10
# shrank tt_DataHeader from 20 to 16 bytes, so the literal described a protocol that no longer
# existed. A literal can only be wrong in silence, and TickLE's framing is the one we can read from
# source - so it is computed below, from the structs and the datagram limit this checkout compiles.
declare -A BASE_TENTHS=([cyclonedds]=1041 [fastdds]=2096)
# 1500 MTU + 14 bytes of Ethernet header, because /proc/net/dev counts at L2.
FRAME_TENTHS=15140

# TickLE's own single-datagram boundary, from include/tickle. A DATA is sent whole when
#     sizeof(tt_Header) + sizeof(tt_SubmessageHeader) + sizeof(tt_DataHeader) + cdr_len <= tt_CONTROL_MAX_LENGTH
# and fragmented otherwise - the comparison in tt_Publisher_publish() against FRAG_WHOLE_DATA_LIMIT (src/tickle.c),
# made on the classic framing before the datagram is compacted to the 4-byte single header, so this is the
# boundary TickLE itself applies, not the frame size on the wire (which is 4 bytes smaller and so always inside
# it). Compiled rather than parsed so that any edit to the structs or to the limit's #if chain moves it.
TICKLE_FRAMING=""
TICKLE_LIMIT=""
tickle_boundary() {
    local tmp rc=0
    tmp="$(mktemp -d)"
    printf '%s\n' '#include <stdio.h>' '#include "tickle/tickle.h"' \
        'int main(void) {' \
        '    printf("%zu %d\n", sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader),' \
        '           (int)tt_CONTROL_MAX_LENGTH);' \
        '    return 0;' \
        '}' >"$tmp/boundary.c"
    if "${CC:-cc}" -std=gnu11 -I"$HERE/include" -o "$tmp/boundary" "$tmp/boundary.c" >"$tmp/cc.log" 2>&1; then
        read -r TICKLE_FRAMING TICKLE_LIMIT < <("$tmp/boundary") || rc=1
    else
        cat "$tmp/cc.log" >&2
        rc=1
    fi
    rm -rf "$tmp"
    return "$rc"
}
if ! tickle_boundary || [ -z "$TICKLE_FRAMING" ] || [ -z "$TICKLE_LIMIT" ]; then
    # "Could not compute" must not read as "fits": the whole premise check depends on these two numbers.
    printf '%-4s %s\n' FAIL "could not compute TickLE's DATA framing and datagram limit from $HERE/include"
    exit 1
fi
echo "TickLE boundary from include/tickle: DATA framing $TICKLE_FRAMING B, whole-datagram limit $TICKLE_LIMIT B"

# How close to an edge a shape may sit. 1440 was a working P3 with 3.4 bytes to spare, which is
# not a margin - it is a cell that would flip on any change to TickLE's framing and read as a
# regression on packet count rather than as a test that lost its premise.
MIN_MARGIN_TENTHS=100

# Which frameworks each shape must fit in one frame. Everything not listed must split, and both
# directions are checked: a P3 that stopped splitting CycloneDDS would be just as broken as one
# that stopped fitting TickLE, and far quieter.
declare -A FITS=([p1]="tickle cyclonedds fastdds" [p2]="tickle cyclonedds fastdds" [p3]="tickle" [p4]="")

fail=0
gen_checked=0
gen_absent=""
note() { printf '%-4s %s\n' "$1" "$2"; }

for shape in p1 p2 p3 p4; do
    msg="$MSG_DIR/$shape/Bench.msg"
    idl="$IDL_DIR/$shape/Bench.idl"
    gen="$MSG_DIR/$shape/Bench.h"
    for f in "$msg" "$idl" "$gen"; do
        if [ ! -f "$f" ]; then
            note FAIL "$shape: missing $f"
            fail=1
            continue 2
        fi
    done

    msg_n="$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$msg")"
    idl_n="$(bench_idl_payload "$idl")"
    if [ -z "$msg_n" ] || [ -z "$idl_n" ]; then
        note FAIL "$shape: could not read the payload array size (msg='$msg_n' idl='$idl_n')"
        fail=1
        continue
    fi
    if [ "$msg_n" != "$idl_n" ]; then
        note FAIL "$shape: payload array is $msg_n in Bench.msg but $idl_n in Bench.idl"
        fail=1
        continue
    fi

    # Field order, checked as "which of the two appears first" rather than by matching a whole
    # line, so a comment change cannot break it and a reorder cannot slip past it.
    msg_order="$(grep -oE '^(uint64 send_ns|uint32 seq)' "$msg" | head -2 | tr '\n' ' ')"
    idl_order="$(grep -oE '(unsigned long long send_ns|unsigned long seq)' "$idl" | head -2 | tr '\n' ' ')"
    case "$msg_order" in
    "uint64 send_ns uint32 seq ") ;;
    *)
        note FAIL "$shape: Bench.msg field order is '$msg_order', want send_ns before seq"
        fail=1
        continue
        ;;
    esac
    case "$idl_order" in
    "unsigned long long send_ns unsigned long seq ") ;;
    *)
        note FAIL "$shape: Bench.idl field order is '$idl_order', want send_ns before seq"
        fail=1
        continue
        ;;
    esac

    total=$((12 + msg_n))
    if [ "$total" != "${EXPECTED[$shape]}" ]; then
        note FAIL "$shape: sample is $total bytes, campaign specifies ${EXPECTED[$shape]}"
        fail=1
        continue
    fi

    # The generator's own assertion, so a .msg edited without `make regen` is caught here too.
    gen_n="$(sed -n 's/.*_Static_assert(sizeof(struct BenchData) == \([0-9]*\).*/\1/p' "$gen")"
    if [ "$gen_n" != "$total" ]; then
        note FAIL "$shape: generated Bench.h asserts $gen_n bytes, sources say $total - run 'make regen'"
        fail=1
        continue
    fi

    # The MTU premise, per framework, in both directions.
    shape_ok=1
    margins=""
    for fw in tickle cyclonedds fastdds; do
        # Both sides in tenths of a byte. TickLE: its own datagram (framing + sample) against its own limit;
        # the vendors: the measured per-sample frame against the Ethernet frame.
        if [ "$fw" = "tickle" ]; then
            frame=$(((total + TICKLE_FRAMING) * 10))
            limit=$((TICKLE_LIMIT * 10))
        else
            frame=$((total * 10 + BASE_TENTHS[$fw]))
            limit=$FRAME_TENTHS
        fi
        want_fit=0
        case " ${FITS[$shape]} " in *" $fw "*) want_fit=1 ;; esac
        if [ "$want_fit" = "1" ]; then
            margin=$((limit - frame))
            verb="fits"
        else
            margin=$((frame - limit))
            verb="splits"
        fi
        # Formatted by hand because a negative margin is the interesting case and integer
        # division truncates toward zero on both halves, which prints -3.-4 for -34 tenths.
        sign=""
        abs=$margin
        if [ "$margin" -lt 0 ]; then
            sign="-"
            abs=$((-margin))
        fi
        pretty="$sign$((abs / 10)).$((abs % 10))"
        if [ "$margin" -lt "$MIN_MARGIN_TENTHS" ]; then
            note FAIL "$shape: $fw frame is $((frame / 10)).$((frame % 10)) B against $((limit / 10)).$((limit % 10)), must ${verb%s}, margin $pretty B"
            shape_ok=0
        fi
        margins="$margins $fw:$verb+$pretty"
    done
    if [ "$shape_ok" != "1" ]; then
        fail=1
        continue
    fi

    # The DDS bindings generated on this host, if any. Checked after the premise rather than
    # before it so a shape that fails both reports both.
    for fw in cyclonedds fastdds; do
        dds_gen="$HERE/examples/perf_hil/$fw/generated/$shape/Bench.h"
        if [ ! -f "$dds_gen" ]; then
            gen_absent="$gen_absent $fw/$shape"
            continue
        fi
        dds_n="$(bench_${fw}_gen_payload "$dds_gen")"
        gen_checked=$((gen_checked + 1))
        if [ "$dds_n" != "$msg_n" ]; then
            note FAIL "$shape: $fw/generated/$shape/Bench.h declares payload[$dds_n], sources say $msg_n - stale binding, rebuild it"
            shape_ok=0
        fi
    done
    if [ "$shape_ok" != "1" ]; then
        fail=1
        continue
    fi

    note OK "$shape: $total bytes, payload[$msg_n], send_ns first, .msg and .idl agree;$margins"
done

# Said every time, so "no DDS binding was checked" can never be mistaken for "every DDS binding
# agreed".
echo "DDS bindings generated on this host: $gen_checked checked; absent:${gen_absent:- none}"

if [ "$fail" != "0" ]; then
    echo "Bench payload shapes are inconsistent - see above" >&2
    exit 1
fi
echo "All four Bench payload shapes agree across .msg, .idl and the generated header"

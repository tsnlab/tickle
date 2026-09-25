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
#   4. each shape still lands on the side of the MTU its design depends on, with real margin
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IDL_DIR="$HERE/examples/perf_hil/idl"
MSG_DIR="$HERE/examples/perf_hil/tickle/common"

# Shape -> the total CDR sample size the campaign specifies. 8 (send_ns) + 4 (seq) + the array.
declare -A EXPECTED=([p1]=76 [p2]=1292 [p3]=1424 [p4]=2800)

# Per-sample framing, in tenths of a byte, measured on the rig in the 2026-09-25 campaign at the
# sizes where every framework sent exactly one datagram. It covers Ethernet + IP + UDP (42 bytes)
# as well as each protocol's own headers and its amortised ACK/heartbeat traffic.
#
# These are measurements, not constants, and that is the point of gating with them: a vendor
# upgrade can move them, and when it does this gate fails and says the premise needs re-measuring
# rather than letting a cell quietly change what it tests. **Re-measure after any CycloneDDS or
# FastDDS upgrade.** TickLE's own figure is the check on the decomposition rather than another
# datapoint: 42 + 28 = 70, and 28 is the DATA framing DESIGN.md specifies (tt_Header 4 +
# submessage header 4 + tt_DataHeader 20).
declare -A BASE_TENTHS=([tickle]=706 [cyclonedds]=1041 [fastdds]=2096)
# 1500 MTU + 14 bytes of Ethernet header, because /proc/net/dev counts at L2.
FRAME_TENTHS=15140
# How close to an edge a shape may sit. 1440 was a working P3 with 3.4 bytes to spare, which is
# not a margin - it is a cell that would flip on any change to TickLE's framing and read as a
# regression on packet count rather than as a test that lost its premise.
MIN_MARGIN_TENTHS=100

# Which frameworks each shape must fit in one frame. Everything not listed must split, and both
# directions are checked: a P3 that stopped splitting CycloneDDS would be just as broken as one
# that stopped fitting TickLE, and far quieter.
declare -A FITS=([p1]="tickle cyclonedds fastdds" [p2]="tickle cyclonedds fastdds" [p3]="tickle" [p4]="")

fail=0
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
    idl_n="$(sed -n 's/.*octet *payload\[\([0-9]*\)\].*/\1/p' "$idl")"
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
        frame=$((total * 10 + BASE_TENTHS[$fw]))
        want_fit=0
        case " ${FITS[$shape]} " in *" $fw "*) want_fit=1 ;; esac
        if [ "$want_fit" = "1" ]; then
            margin=$((FRAME_TENTHS - frame))
            verb="fits"
        else
            margin=$((frame - FRAME_TENTHS))
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
            note FAIL "$shape: $fw frame is $((frame / 10)).$((frame % 10)) B, must $verb, margin $pretty B"
            shape_ok=0
        fi
        margins="$margins $fw:$verb+$pretty"
    done
    if [ "$shape_ok" != "1" ]; then
        fail=1
        continue
    fi

    note OK "$shape: $total bytes, payload[$msg_n], send_ns first, .msg and .idl agree;$margins"
done

if [ "$fail" != "0" ]; then
    echo "Bench payload shapes are inconsistent - see above" >&2
    exit 1
fi
echo "All four Bench payload shapes agree across .msg, .idl and the generated header"

#!/usr/bin/env bash
# layout_flags_cost.sh - what a fixed code alignment costs and what it fixes, for the decision in TESTING.md 6.1
# (gate v2, A1). Static only: compiles and disassembles on this PC, runs no TickLE process, needs no rig and no netns.
#
# WHY (2026-10-11). The rig's A/B gate is more sensitive than the build is reproducible: a5747a85 vs 2e9e861c read
# client CPU +2% three times on the Pi 5 with equal executed instructions, and -falign-functions=64 on both arms made
# the gap vanish. Before a fixed alignment becomes the rig's (or the product's) build, this asks three things:
#   Q1 does it change the instructions a function executes? Each function's instruction sequence (mnemonics, registers
#      and immediates; addresses, comments and the padding after its last instruction removed) is compared between the
#      default build F0 and each flagged one, per object, for the core and the bench, and a difference is classed as
#      "in-body nops only" (loop padding, executed when a loop is entered by falling through) or otherwise (codegen).
#   Q2 does it make a function's code independent of what precedes it? tickle.c with K = 1, 5, 9 nops at the start of
#      its .text (a top-level asm statement; a C function put first is reordered by gcc and perturbs nothing) is
#      compared with the unperturbed unit, per function, under the same flags.
#   Q3 what does it cost in size? .text of the core's objects and of the linked reliable_throughput client and server
#      (p2), per target.
# Flag sets: F0 the default, F64 -falign-functions=64, F64L F64 plus -falign-loops=64, F128 -falign-functions=128 (the
# gate's placement arm A'). Targets: aarch64 (aarch64-linux-gnu-gcc, the Pi 5's ISA) and x86-64 (gcc, this PC), both
# as platform/linux's release build: -O2 -DNDEBUG, the default tt_ config.
#
# HOW TO READ IT (written before the first counted run; the lines below are computed, not typed):
#   Q1 under F64 no function "otherwise" -> the flag adds padding and changes nothing but loop-entry nops; any
#      "otherwise" function -> it changes codegen (named) and is not free in instructions.
#   Q2 under F64 every function identical for every K -> function alignment alone fixes every in-function offset
#      modulo 64, so loop alignment adds nothing for placement; under F0 functions that differ by in-body nops ->
#      the default build's executed instructions themselves move with layout.
#   Q3 "costs nothing measurable" means 0 bytes; anything else is a cost, weighed in TESTING.md section 6.1.
# FIRST RUN (2026-10-11, harness 768c5ada + this file, gcc 15.2; ~/rig_results_safe/layout_flags_cost/run1): aarch64
#   Q1 F64: 197 functions identical, 37 by in-body nops only, 0 otherwise. Q2: F0 2 functions move by in-body nops at
#   K = 1 and 9; F64, F64L and F128 0 at every K. Q3 F64: core .text +6,220 B (+4.6%), client +10,816 B (+6.7%),
#   server +2,528 B (+1.6%); F64L adds 1,109 in-body nops (133 -> 1,242) for nothing in Q2. x86 Q1 F64 has 29
#   functions changed otherwise (gcc's x86 jump and label alignment interacts), Q2 F64 0.
# Usage: layout_flags_cost.sh [OUT=dir]   (default ~/rig_results_safe/layout_flags_cost/<stamp>)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUTD=${OUT:-$HOME/rig_results_safe/layout_flags_cost/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD" || exit 1
WORK=$(mktemp -d /tmp/layout_flags_cost.XXXXXX) || exit 1
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() { [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf "$WORK"; return 0; }
trap cleanup EXIT
SUM=$OUTD/summary.txt
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }

# label|flags
FLAGSETS=("F0|" "F64|-falign-functions=64" "F64L|-falign-functions=64 -falign-loops=64" "F128|-falign-functions=128")
say "=== layout_flags_cost $(date -Is) harness=$(git -C "$REPO" rev-parse --short HEAD) out=$OUTD ==="
git -C "$REPO" archive HEAD | tar -x -C "$WORK" -f - --one-top-level=src_plain || exit 1
# The perturbations: K nops at the very start of tickle.c's .text, as a top-level asm statement (gcc emits those ahead
# of every function; a C function put first in the file is not - gcc reorders functions, and the first version of
# this script perturbed nothing that way). Every function of the unit then starts K x 4 bytes later before alignment.
# K = 1, 5 and 9: 4, 20 and 36 bytes on aarch64, none a multiple of 32.
PADS="1 5 9"
for k in $PADS; do
    cp -r "$WORK/src_plain" "$WORK/src_pert$k"
    {
        printf '__asm__(".text\\n\\t%s");\n' "$(for _ in $(seq 1 "$k"); do printf 'nop\\n\\t'; done)"
        cat "$WORK/src_plain/src/tickle.c"
    } >"$WORK/src_pert$k/src/tickle.c"
done

build() { # $1 target(aarch64|x86) $2 tree(plain|pert) $3 label $4 flags
    local cc o t=$WORK/src_$2 sd arr role f
    case $1 in aarch64) cc=aarch64-linux-gnu-gcc ;; x86) cc=gcc ;; esac
    o=$WORK/obj/$1/$2/$3
    mkdir -p "$o"
    for f in tickle encoding log hal_linux; do
        # shellcheck disable=SC2086 # the flag set is deliberately word-split
        $cc -O2 -DNDEBUG $4 -I"$t/include" -I"$t/src" -c "$t/src/$f.c" -o "$o/$f.o" 2>>"$OUTD/build.log" || return 1
    done
    [ "$2" = plain ] || return 0
    sd=$t/examples/perf_hil/tickle/common/p2
    arr=$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$sd/Bench.msg")
    for role in client server; do
        # shellcheck disable=SC2086
        $cc -O2 $4 -DBENCH_CORE_BUILD=release -DBENCH_SAMPLE_PATH=datagram -DBENCH_DATAGRAM_BYTES=1472 \
            -DBENCH_SAMPLE_BYTES=$((12 + arr)) -I"$sd" -I"$t/examples/perf_hil/tickle/common" -I"$t/include" \
            -c "$t/examples/perf_hil/tickle/reliable_throughput/$role.c" -o "$o/bench_$role.o" 2>>"$OUTD/build.log" || return 1
        # shellcheck disable=SC2086
        $cc -O2 $4 -I"$sd" -I"$t/include" -c "$sd/Bench.c" -o "$o/Bench.o" 2>>"$OUTD/build.log" || return 1
        $cc -o "$o/$role" "$o/bench_$role.o" "$o/Bench.o" "$o/tickle.o" "$o/encoding.o" "$o/log.o" "$o/hal_linux.o" \
            -lm -lpthread 2>>"$OUTD/build.log" || return 1
    done
}
# fnseq <objdump> <obj> <out>: one line per function, "name<TAB>count<TAB>;instruction;instruction;...", with addresses,
# raw bytes, comments and the trailing alignment nops removed, and every address operand reduced to "<>": in an
# unlinked object those are relocation placeholders or objdump's guess of a nearby symbol (an adrp to .rodata prints as
# <first function>, which moves with the padding), not what executes. Two builds compare equal exactly when every
# function executes the same instructions on the same registers and immediates.
fnseq() {
    "$1" -d --no-show-raw-insn "$2" | awk '
        function flush() { if (name != "") { while (n > 0 && ins[n] == "nop") n--; s = ""; for (i = 1; i <= n; i++) s = s ";" ins[i]; print name "\t" n "\t" s } }
        /^[0-9a-f]+ <[^>]+>:$/ { flush(); name = $2; gsub(/[<>:]/, "", name); n = 0; next }
        /^ *[0-9a-f]+:\t/ { sub(/^ *[0-9a-f]+:\t/, ""); sub(/[ \t]*(\/\/|#).*$/, ""); gsub(/(0x)?[0-9a-f]* ?<[^>]*>/, "<>")
            gsub(/[ \t]+/, " "); sub(/ +$/, "")
            if ($0 ~ /^((data16|cs|ds) )*(nop|xchg %ax,%ax)/) $0 = "nop"   # every x86 padding form counts as one nop
            n++; ins[n] = $0; next }
        END { flush() }' >"$3"
}
# sans_nops <seq>: the sequence without its nops - equal for two functions that differ only by alignment nops inside
# the body.
sans_nops() { printf '%s' "${1//;nop/}"; }
nops_in() { # total nops inside function bodies (trailing padding excluded) of a fnseq file
    awk -F'\t' '{ c += gsub(/;nop/, "", $3) } END { print c + 0 }' "$1"
}
text_of() { # $1 size tool, files... -> sum of text
    local sz=$1
    shift
    "$sz" -t "$@" | awk 'END { print $1 }'
}
for tgt in aarch64 x86; do
    case $tgt in aarch64) od=aarch64-linux-gnu-objdump sz=aarch64-linux-gnu-size ;; x86) od=objdump sz=size ;; esac
    say ""
    say "--- target $tgt ($($([ $tgt = aarch64 ] && echo aarch64-linux-gnu-gcc || echo gcc) --version | head -1)) ---"
    for fs in "${FLAGSETS[@]}"; do
        l=${fs%%|*} f=${fs#*|}
        for tree in plain $(for k in $PADS; do echo "pert$k"; done); do
            build $tgt "$tree" "$l" "$f" || { say "FATAL: build $tgt $tree $l failed (build.log)"; exit 1; }
            for u in tickle encoding log hal_linux; do
                fnseq "$od" "$WORK/obj/$tgt/$tree/$l/$u.o" "$WORK/obj/$tgt/$tree/$l/$u.fn"
            done
        done
        o=$WORK/obj/$tgt/plain/$l
        for r in client server; do fnseq "$od" "$o/bench_$r.o" "$o/bench_$r.fn"; done
    done
    b=$WORK/obj/$tgt/plain/F0
    core0=$(text_of "$sz" "$b"/{tickle,encoding,log,hal_linux}.o)
    cl0=$(text_of "$sz" "$b/client")
    sv0=$(text_of "$sz" "$b/server")
    nfn=$(cat "$b"/{tickle,encoding,log,hal_linux}.fn | wc -l)
    say "functions in the core: $nfn; F0 text: core $core0 B, client $cl0 B, server $sv0 B"
    for fs in "${FLAGSETS[@]}"; do
        l=${fs%%|*} f=${fs#*|}
        o=$WORK/obj/$tgt/plain/$l
        # Q1: per function, F vs F0, core and bench
        same=0 diff=0 nopdiff=0 names=""
        for u in tickle encoding log hal_linux bench_client bench_server; do
            while IFS=$'\t' read -r name _ seq; do
                other=$(awk -F'\t' -v n="$name" '$1 == n { print $3 }' "$b/$u.fn")
                if [ "$seq" = "$other" ]; then
                    same=$((same + 1))
                elif [ "$(sans_nops "$seq")" = "$(sans_nops "$other")" ]; then
                    nopdiff=$((nopdiff + 1))
                else
                    diff=$((diff + 1)) names="$names $u:$name"
                fi
            done <"$o/$u.fn"
        done
        core=$(text_of "$sz" "$o"/{tickle,encoding,log,hal_linux}.o)
        cl=$(text_of "$sz" "$o/client")
        sv=$(text_of "$sz" "$o/server")
        say "$(printf 'Q1 %-4s %-42s functions identical to F0: %d, by in-body nops only: %d, otherwise: %d%s' "$l" "'$f'" \
            "$same" "$nopdiff" "$diff" \
            "$([ $diff -gt 0 ] && echo " (${names:0:300})")")"
        say "$(printf 'Q3 %-4s text core %6d B (%+d, %+.1f%%)  client %6d B (%+d, %+.1f%%)  server %6d B (%+d, %+.1f%%)  in-body nops %d' \
            "$l" "$core" $((core - core0)) "$(awk -v a="$core" -v b="$core0" 'BEGIN{print 100*(a-b)/b}')" \
            "$cl" $((cl - cl0)) "$(awk -v a="$cl" -v b="$cl0" 'BEGIN{print 100*(a-b)/b}')" \
            "$sv" $((sv - sv0)) "$(awk -v a="$sv" -v b="$sv0" 'BEGIN{print 100*(a-b)/b}')" \
            "$(cat "$o"/{tickle,encoding,log,hal_linux}.fn | nops_in /dev/stdin)")"
        # Q2: tickle.o, perturbed vs plain, same flags
        for k in $PADS; do
            p=$WORK/obj/$tgt/pert$k/$l
            same=0 diff=0 nopdiff=0
            while IFS=$'\t' read -r name _ seq; do
                [ "$name" = tt_layout_pad ] && continue
                other=$(awk -F'\t' -v n="$name" '$1 == n { print $3 }' "$o/tickle.fn")
                if [ "$seq" = "$other" ]; then
                    same=$((same + 1))
                else
                    diff=$((diff + 1))
                    [ "$(sans_nops "$seq")" = "$(sans_nops "$other")" ] && nopdiff=$((nopdiff + 1))
                fi
            done <"$p/tickle.fn"
            say "$(printf 'Q2 %-4s tickle.o, a %d-nop function ahead vs none: identical %d, differing %d (by in-body nops only: %d)' \
                "$l" "$k" "$same" "$diff" "$nopdiff")"
        done
    done
done
say ""
say "=== layout_flags_cost done $(date -Is) ==="

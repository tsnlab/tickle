#!/usr/bin/env bash
# layout_identity.sh - is a build's every function unchanged by a layout-only change? Static: compiles, links and
# disassembles on this PC; runs no TickLE process, needs no rig and no netns.
#
# WHY (2026-10-15, backlog "gate witness layout noise"): gate v2/v3 decides on executed instructions per sample and
# reads every delta beyond the floor as a verdict (the user, 2026-10-12: keep reading alignment deltas as WORSE, make
# the instrument layout-invariant). Its placement arm P (-falign-functions=128) moves every function by a multiple of
# 64 bytes; a real change also moves static data, moves only the code after it, and grows the unit it is in.
# layout_perturb.sh makes each of those without changing what executes (T1 T2 T3: text ahead of tickle.c, hal_linux.c,
# the bench; D: data; R: an uncalled function in mid tickle.c). This builds the rig's bench (reliable_latency p1 and
# reliable_throughput p2, client and server, as tickle/build.sh does: -O2 -DNDEBUG core, the flag set on core and
# bench, static libtickle) for the base and each perturbation, and compares every linked function
# (layout_identity.py). FLAGSETS: L = gate_v2_lib.sh TICKLE_LAYOUT_CFLAGS as composed for the target (what the rig
# builds; on aarch64 it includes A1'' -mno-fix-cortex-a53-843419), V3 = gate v3's flags (L without A1''), D = none (the
# product), PI13 = the Pis' gcc 13 loop alignment stated explicitly (-falign-loops=8; the cross compiler here is gcc 15).
# Then THE PLACEMENT SWEEP (below): each binary relinked SWEEP times under L and V3, each placement compared.
#
# HOW TO READ IT (written before the first run; layout_identity.py's exit status carries it):
#   - a flag set is LAYOUT-STABLE for a target when every perturbation leaves "nops 0 codegen 0" in every binary:
#     every function executes the same instructions whatever moved around it ("operands" - a data displacement or an
#     aarch64 page offset - is allowed: it changes no count);
#   - L must be LAYOUT-STABLE on aarch64 (the rig) and on x86 (this PC, srv_gap_pc.sh), or the gate's instruction
#     count moves with layout and gate v3's claim is wrong;
#   - D or PI13 showing "nops" for some perturbation is the positive control: the instrument can see padding move.
#     If no perturbation moves anything under D and PI13 either, the run cannot tell stable from blind: VOID.
# FIRST RUNS (2026-10-15, commit ab722f4e, aarch64-linux-gnu-gcc 15.2 and gcc 15.2; ~/rig_results_safe/layout_identity/
#   run3 and run4_x86sweep): perturbations - L, V3 LAYOUT-STABLE on both targets; PI13 MOVES on both, D MOVES on x86
#   (controls fire). Sweep - L moved 0 of 63 placements in every binary, aarch64 and x86; V3 on aarch64 moved 1 of 63
#   in three of the four binaries (latency client, throughput client and server), each an erratum-843419 stub in
#   tt_log_internal: the reason for A1''.
# Usage: layout_identity.sh  env: TARGETS ("aarch64 x86"), FLAGSETS ("L D PI13"), PERTS ("T1 T2 T3 D R"), KEEP=1,
#        SWEEP (63; 0 skips it), SWEEP_FLAGSETS ("L V3"), COMMIT (HEAD), OUT (~/rig_results_safe/layout_identity/<stamp>).
#        Launch detached (~25 min for the defaults). The summary ends "=== layout_identity done".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091
. "$HERE/gate_v2_lib.sh"
TARGETS=${TARGETS:-aarch64 x86}
FLAGSETS=${FLAGSETS:-L D PI13}
PERTS=${PERTS:-T1 T2 T3 D R}
COMMIT=${COMMIT:-HEAD}
OUTD=${OUT:-$HOME/rig_results_safe/layout_identity/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD" || exit 1
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
mkdir -p "$TMPDIR"
WORK=$(mktemp -d "$TMPDIR/layout_identity.XXXXXX") || exit 1
SUM=$OUTD/summary.txt
rc=1
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() {
    if [ "${KEEP:-0}" = 1 ]; then echo "builds kept in $WORK" >>"$SUM"; else rm -rf "$WORK"; fi
    echo "=== layout_identity done rc=$rc $(date -Is) ===" | tee -a "$SUM"
    return 0
}
trap cleanup EXIT
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
flags_of() { # <flagset> <target>
    case $1 in
        L) # the gate's layout flags as gate_v2_lib.sh composes them for a build on <target>
            # shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
            # shellcheck disable=SC1091,SC2034 # GATE_BUILD_ARCH is read by gate_v2_lib.sh
            (GATE_BUILD_ARCH=$2 && . "$HERE/gate_v2_lib.sh" && echo "$TICKLE_LAYOUT_CFLAGS") ;;
        V3) echo "-falign-functions=64 -falign-loops=1 -falign-jumps=1 -falign-labels=1" ;; # gate v3: L without A1''
        D) echo "" ;;
        PI13) echo "-falign-loops=8" ;;
        *) echo "unknown flag set $1" >&2; return 1 ;;
    esac
}
sha=$(git -C "$REPO" rev-parse --short=8 "$COMMIT") || exit 1
say "=== layout_identity $(date -Is) commit=$sha targets=[$TARGETS] flagsets=[$FLAGSETS] perts=[$PERTS] out=$OUTD ==="
git -C "$REPO" archive "$sha" | tar -x -C "$WORK" -f - --one-top-level=base || exit 1
for p in $PERTS; do
    if ! cp -r "$WORK/base" "$WORK/$p" || ! "$HERE/layout_perturb.sh" "$WORK/$p" "$p"; then
        say "FATAL: perturbation $p"
        exit 1
    fi
done

build() { # <target> <tree> <flagset>: core objects, then each bench binary, into $WORK/obj/<target>/<tree>/<flagset>
    local cc t=$WORK/$2 o=$WORK/obj/$1/$2/$3 f u sd arr scen role
    case $1 in aarch64) cc=aarch64-linux-gnu-gcc ;; x86) cc=gcc ;; esac
    f=$(flags_of "$3" "$1") || return 1
    mkdir -p "$o"
    for u in tickle encoding log hal_linux; do
        # shellcheck disable=SC2086 # the flag set is deliberately word-split
        $cc -O2 -DNDEBUG $f -I"$t/include" -I"$t/src" -c "$t/src/$u.c" -o "$o/$u.o" 2>>"$OUTD/build.log" || return 1
    done
    for scen in reliable_latency:p1 reliable_throughput:p2; do
        sd=$t/examples/perf_hil/tickle/common/${scen#*:}
        arr=$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$sd/Bench.msg")
        # shellcheck disable=SC2086
        $cc -O2 $f -I"$sd" -I"$t/include" -c "$sd/Bench.c" -o "$o/Bench_${scen#*:}.o" 2>>"$OUTD/build.log" || return 1
        for role in client server; do
            # shellcheck disable=SC2086
            $cc -O2 $f -DBENCH_CORE_BUILD=release -DBENCH_SAMPLE_PATH=datagram -DBENCH_DATAGRAM_BYTES=1472 \
                -DBENCH_SAMPLE_BYTES=$((12 + arr)) -I"$sd" -I"$t/examples/perf_hil/tickle/common" -I"$t/include" \
                -c "$t/examples/perf_hil/tickle/${scen%:*}/$role.c" -o "$o/${scen%:*}_$role.o" 2>>"$OUTD/build.log" ||
                return 1
            $cc -o "$o/${scen%:*}_${scen#*:}_$role" "$o/${scen%:*}_$role.o" "$o/Bench_${scen#*:}.o" "$o/tickle.o" \
                "$o/encoding.o" "$o/log.o" "$o/hal_linux.o" -lm -lpthread 2>>"$OUTD/build.log" || return 1
        done
    done
}

bins="reliable_latency_p1_client reliable_latency_p1_server reliable_throughput_p2_client reliable_throughput_p2_server"
declare -A moved
for tgt in $TARGETS; do
    case $tgt in aarch64) od=aarch64-linux-gnu-objdump ;; x86) od=objdump ;; *) say "FATAL: target $tgt"; exit 1 ;; esac
    for fs in $FLAGSETS; do
        for tree in base $PERTS; do
            build "$tgt" "$tree" "$fs" || { say "FATAL: build $tgt $tree $fs (build.log)"; exit 1; }
        done
        stable=1
        for p in $PERTS; do
            for b in $bins; do
                line=$(python3 "$HERE/layout_identity.py" --objdump "$od" "$WORK/obj/$tgt/base/$fs/$b" \
                    "$WORK/obj/$tgt/$p/$fs/$b")
                st=$?
                [ $st = 2 ] && { say "FATAL: could not compare $tgt $fs $p $b: $line"; exit 1; }
                [ $st = 1 ] && stable=0 && moved[$tgt:$fs]=1
                say "$(printf '%-7s %-4s %-2s %-29s %s' "$tgt" "$fs" "$p" "$b" "$(printf '%s' "$line" | tr '\n' ' ')")"
            done
        done
        say "$(printf '%-7s %-4s VERDICT %s' "$tgt" "$fs" "$([ $stable = 1 ] && echo LAYOUT-STABLE || echo MOVES)")"
    done
    # the positive control: some non-gate flag set must move, or the instrument is blind on this target
    control=0
    for fs in $FLAGSETS; do [ "$fs" != L ] && [ -n "${moved[$tgt:$fs]:-}" ] && control=1; done
    if [ $control = 0 ]; then say "$tgt CONTROL: no non-gate flag set moved under any perturbation - VOID on $tgt"; fi
done

# THE PLACEMENT SWEEP. A perturbation is one draw of placement; the linker's page-offset rewrites (gate_v2_lib.sh A1'')
# hit about one placement in 64. So each binary is relinked SWEEP times with 64k+4 bytes of text ahead of the bench's
# own code (k = 1..SWEEP: every function moves by 64k under function alignment 64, as P and a real change move it) and
# compared with k = 0. A placement MOVES when any function differs by nops, a veneer or codegen. Reading, written before
# the first sweep: L must move in 0 placements on every target; V3 (gate v3's flags) on aarch64 is where the rewrite
# was seen - "not seen in SWEEP" there says only that this sweep did not draw it.
SWEEP=${SWEEP:-63}
SWEEP_FLAGSETS=${SWEEP_FLAGSETS:-L V3}
for tgt in $TARGETS; do
    case $tgt in aarch64) od=aarch64-linux-gnu-objdump cc=aarch64-linux-gnu-gcc ;; x86) od=objdump cc=gcc ;; esac
    [ "$SWEEP" -gt 0 ] || break
    for fs in $SWEEP_FLAGSETS; do
        [ -d "$WORK/obj/$tgt/base/$fs" ] || build "$tgt" base "$fs" || { say "FATAL: build $tgt base $fs"; exit 1; }
        f=$(flags_of "$fs" "$tgt") || exit 1
        o=$WORK/obj/$tgt/base/$fs
        s=$WORK/sweep/$tgt/$fs
        mkdir -p "$s"
        for b in $bins; do
            scen=${b%_p?_*} pay=${b#"$scen"_}; pay=${pay%%_*} role=${b##*_}
            sd=$WORK/base/examples/perf_hil/tickle/common/$pay
            arr=$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$sd/Bench.msg")
            hits=0 names=""
            for k in $(seq 0 "$SWEEP"); do
                {
                    printf '__asm__(".text\\n\\t.skip %d, 0x90");\n' $((64 * k + 4))
                    cat "$WORK/base/examples/perf_hil/tickle/$scen/$role.c"
                } >"$s/role.c"
                # shellcheck disable=SC2086
                if ! $cc -O2 $f -DBENCH_CORE_BUILD=release -DBENCH_SAMPLE_PATH=datagram -DBENCH_DATAGRAM_BYTES=1472 \
                    -DBENCH_SAMPLE_BYTES=$((12 + arr)) -I"$sd" -I"$WORK/base/examples/perf_hil/tickle/common" \
                    -I"$WORK/base/include" -c "$s/role.c" -o "$s/role.o" 2>>"$OUTD/build.log" ||
                    ! $cc $f -o "$s/bin_$k" "$s/role.o" "$o/Bench_$pay.o" "$o/tickle.o" "$o/encoding.o" "$o/log.o" \
                        "$o/hal_linux.o" -lm -lpthread 2>>"$OUTD/build.log"; then
                    say "FATAL: sweep build $tgt $fs $b $k"
                    exit 1
                fi
                [ "$k" = 0 ] && continue
                line=$(python3 "$HERE/layout_identity.py" --objdump "$od" "$s/bin_0" "$s/bin_$k")
                st=$?
                [ $st = 2 ] && { say "FATAL: could not compare sweep $tgt $fs $b $k: $line"; exit 1; }
                if [ $st = 1 ]; then
                    hits=$((hits + 1))
                    names+=" k=$k:$(printf '%s\n' "$line" | sed -n 's/^  \(nops\|veneer\|codegen\): //p' | tr '\n' ',' | tr ' ' '+')"
                fi
            done
            say "$(printf 'SWEEP %-7s %-4s %-29s placements moved %d of %d%s' "$tgt" "$fs" "$b" "$hits" "$SWEEP" "${names:0:300}")"
        done
    done
done
rc=0

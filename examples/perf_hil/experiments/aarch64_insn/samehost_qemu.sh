#!/usr/bin/env bash
# aarch64_insn/samehost_qemu.sh - EXACT aarch64 user instructions per sample (per round trip on a latency cell) of the
# same-host bench, client AND server, per function, on this PC: the rig's CPU architecture without the rig.
#
# WHY (2026-10-11): gate v2 on the rig read the segment fence (2587915b) +2..+3 client instructions a sample on
# best_effort_throughput and +20..+27 a round trip on reliable_latency, client and server, where the design claimed
# nothing per record. perf stat says how many, not where; this says where. Each arm is cross-compiled the way
# examples/perf_hil/tickle/build.sh builds it on the Pi (libtickle.a -O2 -DNDEBUG plus the gate's layout flag, the bench
# -O2 with the same flag, the default tt_ config, p4 with tt_MAX_SAMPLE_LENGTH=4096), and both processes run under
# qemu-aarch64 in ONE private network namespace with ONE private /dev/shm (the segment path, as the rig's same-host
# cells), each with its executable's translated blocks counted (-d in_asm,exec,nochain over its own text: core and
# bench, not libc, not the kernel) by qemu_tb_count.c.
#
# Caveat, stated before any run: qemu-user does not emulate io_uring, so the HAL runs its fallback receive-readiness
# path here and not the rig's. Core's own code (drain, publish, poll's sleep decision) is the same code either way; a
# difference found only in the HAL is not attributable from this harness.
#
# HOW TO READ IT, written before the counted runs: every arm runs REPS times, interleaved, and arm A2 is A's own
# binaries run again - the A/A spread of the count (timing-dependent paths: ACK solicits, timers, sleeps that a drain
# calls off). A function's B - A is attributed to the change only when it exceeds 2 x its |A2 - A| and repeats in
# every repetition; the totals are read the same way. compare_samehost.py prints both.
#
# Needs: aarch64-linux-gnu-gcc + /usr/aarch64-linux-gnu, qemu-aarch64 at $QEMU, passwordless `sudo -n ip`.
# Usage: ARMS="A=@/src/tree B=@/src/tree ..." QEMU=... samehost_qemu.sh   env: CELLS ("lat_p1 be_p1") REPS (2) OUT
#   The first arm is the reference; A2 (its binaries again) is added. Launch detached; the summary ends "=== done".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU=${QEMU:?QEMU=<path to qemu-aarch64>}
SYSROOT=/usr/aarch64-linux-gnu
CC=aarch64-linux-gnu-gcc
FLAGS=${FLAGS:--falign-functions=64}
TEXT=0xaaaaaaaa0000+0x100000 # a PIE under qemu-aarch64 loads at 0xaaaaaaaa0000

# ------------------------------------------------------------------------------------------- one run, root, in netns
if [ "${1:-}" = --job ]; then
    ulimit -v 2097152
    mount -t tmpfs -o size=256m tmpfs /dev/shm || exit 9
    mkfifo "$J_OUT.server.fifo" "$J_OUT.client.fifo" || exit 9
    "$J_COUNT" <"$J_OUT.server.fifo" >"$J_OUT.server.count" &
    "$J_COUNT" <"$J_OUT.client.fifo" >"$J_OUT.client.count" &
    # shellcheck disable=SC2086 # J_SARGS / J_CARGS are deliberately word-split into argv
    env BENCH_IFACE=lo timeout -s INT "$J_STO" "$QEMU" -L "$SYSROOT" -d in_asm,exec,nochain -dfilter "$TEXT" \
        -D "$J_OUT.server.fifo" "$J_BIN/server" $J_SARGS >"$J_OUT.server.log" 2>&1 &
    spid=$!
    sleep 2
    # shellcheck disable=SC2086
    env BENCH_IFACE=lo timeout -s INT 600 "$QEMU" -L "$SYSROOT" -d in_asm,exec,nochain -dfilter "$TEXT" \
        -D "$J_OUT.client.fifo" "$J_BIN/client" $J_CARGS >"$J_OUT.client.log" 2>&1
    echo "client_rc=$?" >"$J_OUT.status"
    # The server: the timeout launched above ($!), whose child is qemu; INT it by its exe, then wait for the timeout.
    for c in $(cat /proc/"$spid"/task/*/children 2>/dev/null | tr ' ' '\n'); do
        case "$(readlink "/proc/$c/exe")" in */qemu-aarch64) kill -INT "$c" ;; esac
    done
    wait "$spid"
    echo "server_rc=$?" >>"$J_OUT.status"
    wait
    rm -f "$J_OUT.server.fifo" "$J_OUT.client.fifo"
    exit 0
fi

ARMS=${ARMS:?ARMS=\"A=@dir B=@dir ...\"}
CELLS=${CELLS:-lat_p1 be_p1}
REPS=${REPS:-2}
OUTD=${OUT:-$HOME/rig_results_safe/aarch64_insn/samehost_$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD" || exit 1
SUM="$OUTD/summary.txt"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/samehost_qemu.XXXXXX") || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
on_exit() {
    local rc=$? ns
    while read -r ns; do [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null; done <"$NS_LIST"
    rm -rf "$WORK"
    echo "=== done rc=$rc $(date -Is): $OUTD ===" | tee -a "$SUM"
}
trap on_exit EXIT
gcc -O2 -o "$WORK/count" "$HERE/qemu_tb_count.c" || exit 1
echo "=== samehost_qemu $(date -Is) ARMS='$ARMS' CELLS='$CELLS' REPS=$REPS FLAGS='$FLAGS' $("$QEMU" --version | head -1)" | tee -a "$SUM"

build() { # $1 label $2 source tree
    local t=$2 o="$WORK/bin/$1" s core sd arr v role
    for s in p1 p4; do
        core=""
        [ $s = p4 ] && core="-Dtt_MAX_SAMPLE_LENGTH=4096"
        mkdir -p "$o/$s"
        for f in tickle encoding log hal_linux; do
            # shellcheck disable=SC2086 # FLAGS and core are deliberately word-split into flags
            $CC -O2 -DNDEBUG $FLAGS $core -w -I"$t/include" -c "$t/src/$f.c" -o "$o/$s/$f.o" || return 1
        done
        aarch64-linux-gnu-ar rcs "$o/$s/libtickle.a" "$o/$s/tickle.o" "$o/$s/encoding.o" "$o/$s/log.o" "$o/$s/hal_linux.o"
        sd=$t/examples/perf_hil/tickle/common/$s
        arr=$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$sd/Bench.msg")
        for v in reliable_latency best_effort_throughput; do
            mkdir -p "$o/${v}_$s"
            for role in client server; do
                # shellcheck disable=SC2046,SC2086 # FLAGS and core are deliberately word-split into flags
                $CC -O2 $FLAGS $core -DBENCH_CORE_BUILD=release \
                    -DBENCH_SAMPLE_PATH=$([ $s = p4 ] && echo frag || echo datagram) -DBENCH_DATAGRAM_BYTES=1472 \
                    -DBENCH_SAMPLE_BYTES=$((12 + arr)) -I"$sd" -I"$t/examples/perf_hil/tickle/common" -I"$t/include" \
                    -o "$o/${v}_$s/$role" "$t/examples/perf_hil/tickle/$v/$role.c" "$sd/Bench.c" "$o/$s/libtickle.a" \
                    -lm -lpthread || return 1
            done
        done
    done
}
LABELS=()
for spec in $ARMS; do
    l=${spec%%=*}
    src=${spec#*=}
    LABELS+=("$l")
    build "$l" "${src#@}" >"$OUTD/build_$l.log" 2>&1 || { echo "FATAL: build $l (build_$l.log)" | tee -a "$SUM"; exit 1; }
    echo "built $l from ${src#@}" | tee -a "$SUM"
done
cp -r "$WORK/bin/${LABELS[0]}" "$WORK/bin/${LABELS[0]}2"
cp -r "$WORK/bin" "$OUTD/bin" # kept, so a block's pc in a .count file can be disassembled afterwards
LABELS+=("${LABELS[0]}2")

NSID=0
run_one() { # $1 label $2 cell $3 rep
    NSID=$((NSID + 1))
    local ns="sq$$-$NSID" out="$OUTD/${2}_${1}_r$3" s=${2#*_} v cargs sargs sto
    case $2 in
    lat_*)
        v=reliable_latency_$s
        cargs="-i 0.005 -d 4 -W 200 -C 200 -I 0.001"
        sargs="-d 60 -W 200 -C 200 -I 0.001"
        sto=120
        ;;
    be_*)
        v=best_effort_throughput_$s
        cargs="-K 1 -d 3 --warmup-s 1 --cooldown-s 1"
        sargs="-K 1 -d 3 --warmup-s 1 --cooldown-s 1"
        sto=120
        ;;
    esac
    echo "$ns" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$ns" && sudo -n ip netns exec "$ns" ip link set lo up &&
        sudo -n ip netns exec "$ns" ip route add default dev lo &&
        sudo -n ip netns exec "$ns" ip link add bench0 type dummy &&
        sudo -n ip netns exec "$ns" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0 &&
        sudo -n ip netns exec "$ns" ip link set bench0 up; }; then
        echo "SETUP_FAIL $out" | tee -a "$SUM"
        return
    fi
    sudo -n ip netns exec "$ns" env QEMU="$QEMU" SYSROOT="$SYSROOT" TEXT="$TEXT" J_BIN="$WORK/bin/$1/$v" \
        J_COUNT="$WORK/count" J_CARGS="$cargs" J_SARGS="$sargs" J_STO="$sto" J_OUT="$out" bash "$0" --job
    sudo -n ip netns del "$ns"
    local sent ct st
    sent=$(grep -m1 '^RESULT' "$out.client.log" | grep -o ' sent=[0-9]*' | cut -d= -f2)
    ct=$(awk '/^TOTAL/{print $2}' "$out.client.count")
    st=$(awk '/^TOTAL/{print $2}' "$out.server.count")
    echo "QRUN arm=$1 cell=$2 rep=$3 sent=${sent:-0} client=${ct:-0} server=${st:-0} $(tr '\n' ' ' <"$out.status")" | tee -a "$SUM"
}
N=${#LABELS[@]}
for rep in $(seq 1 "$REPS"); do
    for cell in $CELLS; do
        for k in $(seq 0 $((N - 1))); do run_one "${LABELS[$(((k + rep) % N))]}" "$cell" "$rep"; done
    done
done
python3 "$HERE/compare_samehost.py" "$OUTD" "${LABELS[@]}" | tee -a "$SUM"

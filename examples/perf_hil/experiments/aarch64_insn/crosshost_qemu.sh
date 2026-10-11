#!/usr/bin/env bash
# aarch64_insn/crosshost_qemu.sh - EXACT aarch64 user instructions per sample (per round trip on a latency cell) of the
# CROSS-HOST core bench, client AND server, per function, on this PC: the rig's CPU architecture without the rig.
#
# WHY (2026-10-12): gate v2 cross-host on the rig read ab/hybrid-land d8980f9b against main 4fbb8fe1 +0.2..+2.7% user
# instructions on the core default build (reliable_throughput p2-p4, best_effort_throughput p4, reliable_latency
# p1-p4), where the hybrid transport should cost a core-default build nothing. samehost_qemu.sh counts the same-host
# (segment) path; this counts the UDP path the rig's cross-host cells take: the two processes in TWO private network
# namespaces joined by a veth pair, each with its own /dev/shm, so no segment is ever found. Built as samehost_qemu.sh
# builds (build.sh's way: libtickle.a -O2 -DNDEBUG plus the gate's layout flag, p4 with tt_MAX_SAMPLE_LENGTH=4096).
#
# Caveat, stated before any run: qemu-user does not emulate io_uring, so the HAL runs its fallback receive-readiness
# path here and not the rig's; and qemu's timing is not the Pi's, so timing-dependent paths differ in share.
#
# HOW TO READ IT, written before the counted runs: as samehost_qemu.sh - arm A2 is A's own binaries run again (the A/A
# spread); a function's B - A is attributed only when it exceeds 2 x its |A2 - A| and repeats in every repetition;
# compare_samehost.py prints both. Cells: lat_pN reliable_latency, rt_pN reliable_throughput -Q, be_pN
# best_effort_throughput.
#
# Needs: aarch64-linux-gnu-gcc + /usr/aarch64-linux-gnu, qemu-aarch64 at $QEMU, passwordless `sudo -n ip`.
# Usage: ARMS="A=@/src/tree B=@/src/tree ..." QEMU=... crosshost_qemu.sh   env: CELLS ("lat_p1 rt_p4") REPS (2) OUT
#   The first arm is the reference; A2 (its binaries again) is added. Launch detached; the summary ends "=== done".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU=${QEMU:?QEMU=<path to qemu-aarch64>}
SYSROOT=/usr/aarch64-linux-gnu
CC=aarch64-linux-gnu-gcc
FLAGS=${FLAGS:--falign-functions=64}
TEXT=0xaaaaaaaa0000+0x100000 # a PIE under qemu-aarch64 loads at 0xaaaaaaaa0000

# ----------------------------------------------------------------------------------- one run, root, in netns a
if [ "${1:-}" = --job ]; then
    ulimit -v 2097152
    mount -t tmpfs -o size=256m tmpfs /dev/shm || exit 9
    mkfifo "$J_OUT.server.fifo" "$J_OUT.client.fifo" || exit 9
    "$J_COUNT" <"$J_OUT.server.fifo" >"$J_OUT.server.count" &
    "$J_COUNT" <"$J_OUT.client.fifo" >"$J_OUT.client.count" &
    # The server in the other namespace, with its own /dev/shm (ip netns exec gives it its own mount namespace).
    # shellcheck disable=SC2016,SC2086 # $@ is the inner sh's; J_SARGS is deliberately word-split into argv
    ip netns exec "$J_SNS" sh -c 'mount -t tmpfs -o size=256m tmpfs /dev/shm && exec "$@"' sh \
        env BENCH_IFACE="$J_VB" timeout -s INT "$J_STO" "$QEMU" -L "$SYSROOT" -d in_asm,exec,nochain -dfilter "$TEXT" \
        -D "$J_OUT.server.fifo" "$J_BIN/server" $J_SARGS >"$J_OUT.server.log" 2>&1 &
    spid=$!
    sleep 2
    # shellcheck disable=SC2086
    env BENCH_IFACE="$J_VA" timeout -s INT 600 "$QEMU" -L "$SYSROOT" -d in_asm,exec,nochain -dfilter "$TEXT" \
        -D "$J_OUT.client.fifo" "$J_BIN/client" $J_CARGS >"$J_OUT.client.log" 2>&1
    echo "client_rc=$?" >"$J_OUT.status"
    # The server: $! became timeout (ip and sh exec into env, env into timeout); INT the qemu under it, by its exe.
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
CELLS=${CELLS:-lat_p1 rt_p4}
REPS=${REPS:-2}
OUTD=${OUT:-$HOME/rig_results_safe/aarch64_insn/crosshost_$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD" || exit 1
SUM="$OUTD/summary.txt"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/crosshost_qemu.XXXXXX") || exit 1
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
echo "=== crosshost_qemu $(date -Is) ARMS='$ARMS' CELLS='$CELLS' REPS=$REPS FLAGS='$FLAGS' $("$QEMU" --version | head -1)" | tee -a "$SUM"

# The shapes and variants the cells name, built per arm.
SHAPES=$(for c in $CELLS; do echo "${c#*_}"; done | sort -u | tr '\n' ' ')
build() { # $1 label $2 source tree
    local t=$2 o="$WORK/bin/$1" s core sd arr v role
    for s in $SHAPES; do
        core=""
        [ "$s" = p4 ] && core="-Dtt_MAX_SAMPLE_LENGTH=4096"
        mkdir -p "$o/$s"
        for f in tickle encoding log hal_linux; do
            # shellcheck disable=SC2086 # FLAGS and core are deliberately word-split into flags
            $CC -O2 -DNDEBUG $FLAGS $core -w -I"$t/include" -c "$t/src/$f.c" -o "$o/$s/$f.o" || return 1
        done
        aarch64-linux-gnu-ar rcs "$o/$s/libtickle.a" "$o/$s/tickle.o" "$o/$s/encoding.o" "$o/$s/log.o" "$o/$s/hal_linux.o"
        sd=$t/examples/perf_hil/tickle/common/$s
        arr=$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$sd/Bench.msg")
        for v in reliable_latency best_effort_throughput reliable_throughput; do
            mkdir -p "$o/${v}_$s"
            for role in client server; do
                # shellcheck disable=SC2046,SC2086 # FLAGS and core are deliberately word-split into flags
                $CC -O2 $FLAGS $core -DBENCH_CORE_BUILD=release \
                    -DBENCH_SAMPLE_PATH=$([ "$s" = p4 ] && echo frag || echo datagram) -DBENCH_DATAGRAM_BYTES=1472 \
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
    local id="xq$$-$NSID" out="$OUTD/${2}_${1}_r$3" s=${2#*_} v cargs sargs sto n
    case $s in p1) n=2048 ;; p2) n=405 ;; p3) n=368 ;; *) n=187 ;; esac
    case $2 in
    lat_*)
        v=reliable_latency_$s
        cargs="-i 0.005 -d 4 -W 200 -C 200 -I 0.001"
        sargs="-d 60 -W 200 -C 200 -I 0.001"
        ;;
    be_*)
        v=best_effort_throughput_$s
        cargs="-K 1 -d 3 --warmup-s 1 --cooldown-s 1"
        sargs=$cargs
        ;;
    *)
        v=reliable_throughput_$s
        cargs="-Q -N $n -B 100 -d 4 --warmup-s 0.5 --cooldown-s 0.5"
        sargs=$cargs
        ;;
    esac
    sto=120
    echo "${id}a" >>"$NS_LIST"
    echo "${id}b" >>"$NS_LIST"
    # interface names stay within IFNAMSIZ (15)
    if ! { sudo -n ip netns add "${id}a" && sudo -n ip netns add "${id}b" &&
        sudo -n ip link add "${id}va" type veth peer name "${id}vb" && sudo -n ip link set "${id}va" netns "${id}a" &&
        sudo -n ip link set "${id}vb" netns "${id}b" &&
        sudo -n ip -n "${id}a" addr add 192.168.10.1/24 brd + dev "${id}va" &&
        sudo -n ip -n "${id}b" addr add 192.168.10.2/24 brd + dev "${id}vb" &&
        sudo -n ip -n "${id}a" link set lo up && sudo -n ip -n "${id}b" link set lo up &&
        sudo -n ip -n "${id}a" link set "${id}va" up && sudo -n ip -n "${id}b" link set "${id}vb" up; }; then
        echo "SETUP_FAIL $out" | tee -a "$SUM"
        return
    fi
    sudo -n ip netns exec "${id}a" env QEMU="$QEMU" SYSROOT="$SYSROOT" TEXT="$TEXT" J_BIN="$WORK/bin/$1/$v" \
        J_SNS="${id}b" J_VA="${id}va" J_VB="${id}vb" J_COUNT="$WORK/count" J_CARGS="$cargs" J_SARGS="$sargs" \
        J_STO="$sto" J_OUT="$out" bash "$0" --job
    sudo -n ip netns del "${id}a"
    sudo -n ip netns del "${id}b"
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

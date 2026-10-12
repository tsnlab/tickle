#!/usr/bin/env bash
# aarch64_insn/layout_qemu.sh - does the EXACT aarch64 user instruction count of one TickLE commit depend on where its
# code is placed? Each arm is the same source tree built with a different flag set; both bench processes run under
# qemu-aarch64 and every executed translated block is counted (qemu_tb_count.c), split into the executable's own text
# (core + bench) and everything else (libc, ld.so), and the executable's executed NOPs are counted from its kept
# disassembly - padding the CPU executes.
#
# WHY (2026-10-12): gate v2 decides on instructions per sample (perf stat instructions:u on the Pis), and its own
# validation X2r (A vs A rebuilt -falign-functions=256) moved client user instructions by -2 a sample at c2. Gate v3
# builds every arm with TICKLE_LAYOUT_CFLAGS="-falign-functions=64 -falign-loops=1 -falign-jumps=1 -falign-labels=1"
# (gate_v2_lib.sh): function entries stay 64-aligned (padding BEFORE a function is never executed) and no padding is
# put inside a function, where a loop head's nops are executed on every iteration.
#
# HOW TO READ IT, written before the runs and enforced by layout_qemu.py:
#   - Arm A2 is the first arm's binaries run again: the A/A spread of each count (timing-dependent paths).
#   - INVARIANT for a family (arms sharing a prefix: OLD*, NEW*, PCO*, PCN*): every arm's executed app NOPs per sample
#     equal the family's first arm's to 0.01, and its app instructions per sample are within max(3 x the two arms'
#     combined standard error over REPS, 0.5) of it, in every cell and role. Otherwise DIFFERS. A run with fewer than
#     half its cell's median samples is VOID (start-up dominates it).
#     Revised 2026-10-12 after the first runs, where the rule was max(2 x |A2 - A|, 0.5): one A/A difference of means
#     is a single draw (0.04 at rt_p3 client, while that arm's repetitions spread by 5), so it flagged run-to-run
#     timing noise in EVERY family, and one 89-sample run (against ~880) read as +1,350 a round trip. The executed-NOP
#     count is exact and was not affected; the total is timing-dependent under qemu and can only be read statistically.
#   - The NEW family must be INVARIANT and execute 0 app NOPs. The positive control PCO (function alignment 4 vs 8
#     with the Pi's loop alignment 8: loop heads move within the 8-byte grain) must DIFFER in executed NOPs - if it
#     does not, this instrument cannot see padding and the run is VOID. PCN (the same pair with the new flags) must be
#     INVARIANT.
# Caveats: qemu-user does not emulate io_uring (the HAL's fallback readiness path runs); the cross compiler is gcc 15,
# the Pis' is gcc 13 (loop alignment 8 there, 32:16 here: the OLD/PCO arms state 8 explicitly, as the Pis build).
#
# Needs: aarch64-linux-gnu-gcc + /usr/aarch64-linux-gnu, qemu-aarch64 at $QEMU, passwordless `sudo -n ip`.
# Usage: SRC=<tree> QEMU=<qemu-aarch64> layout_qemu.sh
#   env: MODE crosshost|samehost (crosshost)  CELLS ("rt_p2 rt_p3 lat_p2")  REPS (2)  OUT
#        ARMS ("LABEL=flag+flag ..."; '+' separates flags; the default is the OLD/NEW/PCO/PCN set below)
#   Launch detached; the summary ends "=== done rc=".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU=${QEMU:?QEMU=<path to qemu-aarch64>}
SYSROOT=/usr/aarch64-linux-gnu
CC=aarch64-linux-gnu-gcc
APP_LO=0xaaaaaaaa0000 # a PIE under qemu-aarch64 loads here
APP_HI=0xaaaaaabb0000

# ----------------------------------------------------------------------------------- one run, root, in netns a
if [ "${1:-}" = --job ]; then
    ulimit -v 2097152
    mount -t tmpfs -o size=256m tmpfs /dev/shm || exit 9
    mkfifo "$J_OUT.server.fifo" "$J_OUT.client.fifo" || exit 9
    "$J_COUNT" <"$J_OUT.server.fifo" >"$J_OUT.server.count" &
    "$J_COUNT" <"$J_OUT.client.fifo" >"$J_OUT.client.count" &
    if [ -n "$J_SNS" ]; then
        # shellcheck disable=SC2016,SC2086 # $@ is the inner sh's; J_SARGS is deliberately word-split into argv
        ip netns exec "$J_SNS" sh -c 'mount -t tmpfs -o size=256m tmpfs /dev/shm && exec "$@"' sh \
            env BENCH_IFACE="$J_VB" timeout -s INT "$J_STO" "$QEMU" -L "$SYSROOT" -d in_asm,exec,nochain \
            -D "$J_OUT.server.fifo" "$J_BIN/server" $J_SARGS >"$J_OUT.server.log" 2>&1 &
    else
        # shellcheck disable=SC2086
        env BENCH_IFACE="$J_VB" timeout -s INT "$J_STO" "$QEMU" -L "$SYSROOT" -d in_asm,exec,nochain \
            -D "$J_OUT.server.fifo" "$J_BIN/server" $J_SARGS >"$J_OUT.server.log" 2>&1 &
    fi
    spid=$!
    sleep 2
    # shellcheck disable=SC2086
    env BENCH_IFACE="$J_VA" timeout -s INT 900 "$QEMU" -L "$SYSROOT" -d in_asm,exec,nochain \
        -D "$J_OUT.client.fifo" "$J_BIN/client" $J_CARGS >"$J_OUT.client.log" 2>&1
    echo "client_rc=$?" >"$J_OUT.status"
    # The server: $! is timeout (or ip -> sh -> env -> timeout, each exec'd); INT the qemu under it, by its exe.
    for c in $(cat /proc/"$spid"/task/*/children 2>/dev/null | tr ' ' '\n'); do
        case "$(readlink "/proc/$c/exe")" in */qemu-aarch64) kill -INT "$c" ;; esac
    done
    wait "$spid"
    echo "server_rc=$?" >>"$J_OUT.status"
    wait
    rm -f "$J_OUT.server.fifo" "$J_OUT.client.fifo"
    exit 0
fi

SRC=${SRC:?SRC=<source tree>}
MODE=${MODE:-crosshost}
case $MODE in crosshost | samehost) ;; *) echo "MODE must be crosshost or samehost" >&2; exit 2 ;; esac
CELLS=${CELLS:-rt_p2 rt_p3 lat_p2}
REPS=${REPS:-2}
OLD="-falign-loops=8"
NEW="-falign-loops=1+-falign-jumps=1+-falign-labels=1"
ARMS=${ARMS:-"OLD64=-falign-functions=64+$OLD OLD128=-falign-functions=128+$OLD OLD256=-falign-functions=256+$OLD \
NEW64=-falign-functions=64+$NEW NEW128=-falign-functions=128+$NEW NEW256=-falign-functions=256+$NEW \
PCO4=-falign-functions=4+$OLD PCO8=-falign-functions=8+$OLD PCN4=-falign-functions=4+$NEW PCN8=-falign-functions=8+$NEW"}
OUTD=${OUT:-$HOME/rig_results_safe/aarch64_insn/layout_${MODE}_$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD" || exit 1
SUM="$OUTD/summary.txt"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/layout_qemu.XXXXXX") || exit 1
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
echo "=== layout_qemu $(date -Is) MODE=$MODE SRC=$SRC ($(git -C "$SRC" rev-parse --short HEAD 2>/dev/null)) CELLS='$CELLS'" \
    "REPS=$REPS $("$QEMU" --version | head -1); $($CC --version | head -1)" | tee -a "$SUM"

SHAPES=$(for c in $CELLS; do echo "${c#*_}"; done | sort -u | tr '\n' ' ')
build() { # $1 label $2 flags (space-separated); as tickle/build.sh builds on the Pi
    local o="$WORK/bin/$1" fl=$2 s core sd arr v role
    for s in $SHAPES; do
        core=""
        [ "$s" = p4 ] && core="-Dtt_MAX_SAMPLE_LENGTH=4096"
        mkdir -p "$o/$s"
        for f in tickle encoding log hal_linux; do
            # shellcheck disable=SC2086 # fl and core are deliberately word-split into flags
            $CC -O2 -DNDEBUG $fl $core -w -I"$SRC/include" -c "$SRC/src/$f.c" -o "$o/$s/$f.o" || return 1
        done
        aarch64-linux-gnu-ar rcs "$o/$s/libtickle.a" "$o/$s/tickle.o" "$o/$s/encoding.o" "$o/$s/log.o" "$o/$s/hal_linux.o"
        sd=$SRC/examples/perf_hil/tickle/common/$s
        arr=$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$sd/Bench.msg")
        for v in reliable_latency reliable_throughput; do
            mkdir -p "$o/${v}_$s"
            for role in client server; do
                # shellcheck disable=SC2046,SC2086 # fl and core are deliberately word-split into flags
                $CC -O2 $fl $core -DBENCH_CORE_BUILD=release \
                    -DBENCH_SAMPLE_PATH=$([ "$s" = p4 ] && echo frag || echo datagram) -DBENCH_DATAGRAM_BYTES=1472 \
                    -DBENCH_SAMPLE_BYTES=$((12 + arr)) -I"$sd" -I"$SRC/examples/perf_hil/tickle/common" -I"$SRC/include" \
                    -o "$o/${v}_$s/$role" "$SRC/examples/perf_hil/tickle/$v/$role.c" "$sd/Bench.c" "$o/$s/libtickle.a" \
                    -lm -lpthread || return 1
                # the executable's NOP addresses, for the executed-NOP count (pc relative to the load address)
                aarch64-linux-gnu-objdump -d --no-show-raw-insn "$o/${v}_$s/$role" |
                    awk '$2 == "nop" { sub(":", "", $1); print $1 }' >"$o/${v}_$s/$role.nops"
            done
        done
    done
}
LABELS=()
for spec in $ARMS; do
    l=${spec%%=*}
    fl=$(printf '%s' "${spec#*=}" | tr '+' ' ')
    LABELS+=("$l")
    build "$l" "$fl" >"$OUTD/build_$l.log" 2>&1 || { echo "FATAL: build $l (build_$l.log)" | tee -a "$SUM"; exit 1; }
    echo "built $l with '$fl'" | tee -a "$SUM"
done
cp -r "$WORK/bin/${LABELS[0]}" "$WORK/bin/${LABELS[0]}2"
LABELS+=("${LABELS[0]}2")
cp -r "$WORK/bin" "$OUTD/bin" # kept: the .nops files are read afterwards, and a block's pc can be disassembled

NSID=0
run_one() { # $1 label $2 cell $3 rep
    NSID=$((NSID + 1))
    local id="lq$$-$NSID" out="$OUTD/${2}_${1}_r$3" s=${2#*_} v cargs sargs n sns va vb
    case $s in p1) n=2048 ;; p2) n=405 ;; p3) n=368 ;; *) n=187 ;; esac
    case $2 in
    lat_*)
        v=reliable_latency_$s
        cargs="-i 0.005 -d 4 -W 200 -C 200 -I 0.001"
        sargs="-d 60 -W 200 -C 200 -I 0.001"
        ;;
    *)
        v=reliable_throughput_$s
        cargs="-Q -N $n -B 100 -d 4 --warmup-s 0.5 --cooldown-s 0.5"
        sargs=$cargs
        ;;
    esac
    echo "${id}a" >>"$NS_LIST"
    if [ "$MODE" = crosshost ]; then
        echo "${id}b" >>"$NS_LIST"
        sns="${id}b" va="${id}va" vb="${id}vb"
        # interface names stay within IFNAMSIZ (15)
        if ! { sudo -n ip netns add "${id}a" && sudo -n ip netns add "${id}b" &&
            sudo -n ip link add "$va" type veth peer name "$vb" && sudo -n ip link set "$va" netns "${id}a" &&
            sudo -n ip link set "$vb" netns "${id}b" &&
            sudo -n ip -n "${id}a" addr add 192.168.10.1/24 brd + dev "$va" &&
            sudo -n ip -n "${id}b" addr add 192.168.10.2/24 brd + dev "$vb" &&
            sudo -n ip -n "${id}a" link set lo up && sudo -n ip -n "${id}b" link set lo up &&
            sudo -n ip -n "${id}a" link set "$va" up && sudo -n ip -n "${id}b" link set "$vb" up; }; then
            echo "SETUP_FAIL $out" | tee -a "$SUM"
            return
        fi
    else
        sns="" va=lo vb=lo
        if ! { sudo -n ip netns add "${id}a" && sudo -n ip netns exec "${id}a" ip link set lo up &&
            sudo -n ip netns exec "${id}a" ip route add default dev lo &&
            sudo -n ip netns exec "${id}a" ip link add bench0 type dummy &&
            sudo -n ip netns exec "${id}a" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0 &&
            sudo -n ip netns exec "${id}a" ip link set bench0 up; }; then
            echo "SETUP_FAIL $out" | tee -a "$SUM"
            return
        fi
    fi
    sudo -n ip netns exec "${id}a" env QEMU="$QEMU" SYSROOT="$SYSROOT" J_BIN="$WORK/bin/$1/$v" J_SNS="$sns" \
        J_VA="$va" J_VB="$vb" J_COUNT="$WORK/count" J_CARGS="$cargs" J_SARGS="$sargs" J_STO=180 J_OUT="$out" \
        bash "$0" --job
    sudo -n ip netns del "${id}a"
    [ -n "$sns" ] && sudo -n ip netns del "$sns"
    echo "$1 $v" >"$out.arm"
    echo "QRUN arm=$1 cell=$2 rep=$3 $(grep -m1 -o ' sent=[0-9]*' "$out.client.log") $(tr '\n' ' ' <"$out.status")" |
        tee -a "$SUM"
}
N=${#LABELS[@]}
for rep in $(seq 1 "$REPS"); do
    for cell in $CELLS; do
        for k in $(seq 0 $((N - 1))); do run_one "${LABELS[$(((k + rep) % N))]}" "$cell" "$rep"; done
    done
done
python3 "$HERE/layout_qemu.py" "$OUTD" "$APP_LO" "$APP_HI" "${LABELS[@]}" | tee -a "$SUM"

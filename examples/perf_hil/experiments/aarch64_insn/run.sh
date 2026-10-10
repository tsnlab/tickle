#!/usr/bin/env bash
# aarch64_insn/run.sh - EXACT aarch64 user instructions per sample of the core bench's publisher, per function, on this
# PC: the rig's CPU (Pi 5, Cortex-A76) without the rig.
#
# WHY (2026-10-10): largemsgL_l2x read client CPU +2% at p2/p3 for a5747a85 against 2e9e861c on the rig, and perf on
# this PC (largemsg_insn_ab.sh) found x86 instructions equal. x86 is not the rig's code, so the aarch64 code is counted
# here: each arm cross-compiled as examples/perf_hil/tickle/build.sh builds it on the Pi (libtickle.a -O2 -DNDEBUG, the
# bench -O2, PIE, the default tt_ config), reliable_throughput -Q -N <campaign bound> -B 100 run under qemu-aarch64
# between two private network namespaces (veth, each side its own /dev/shm - cross-host shaped), and the client's
# translated blocks counted (-d in_asm,exec,nochain over the executable's own text: core and bench, not libc or the
# kernel). qemu_tb_count.c folds the log into instructions per block start; compare.py prints per-function
# instructions per sample for two runs.
#
# HOW TO READ IT, written before the counted runs: every arm runs REPS times, interleaved; arm A2 is A's own binary
# again, so A2 vs A is the run-to-run spread of the count (the timing-dependent paths: KEEP_ALL refusals, ACK solicits,
# timers). X differs from A only by more than max(2 instructions a sample, 2 x that A/A spread); a function's difference
# is attributed only when it repeats in every repetition. Equal counts mean the rig's CPU gap is not work in core or the
# bench, and the next question is placement (the rig's own placement control) or the kernel.
#
# Needs: aarch64-linux-gnu-gcc and its sysroot (/usr/aarch64-linux-gnu), qemu-aarch64 at $QEMU (Ubuntu's qemu-user,
# unpacked without root: `apt-get download qemu-user && dpkg -x qemu-user_*.deb <dir>`), passwordless `sudo -n ip`.
# Usage: ARMS="A=<commit> B=<commit>" run.sh   env: QEMU SHAPES ("p1 p3") REPS (3) QDUR (4) OUT
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
QEMU=${QEMU:?QEMU=<path to qemu-aarch64>}
SYSROOT=/usr/aarch64-linux-gnu
CC=aarch64-linux-gnu-gcc

# ------------------------------------------------------------------------------------------------ one run, as root
if [ "${1:-}" = --job ]; then
    mount -t tmpfs -o size=256m tmpfs /dev/shm || exit 1
    # shellcheck disable=SC2016,SC2086 # $@ is the inner sh's; J_ARGS is deliberately word-split into argv
    ip netns exec "$J_SNS" sh -c 'mount -t tmpfs -o size=256m tmpfs /dev/shm && exec "$@"' sh \
        env BENCH_IFACE="$J_VB" "$QEMU" -L "$SYSROOT" "$J_BIN/server_$J_SHAPE" $J_ARGS >"$J_OUT.server.log" 2>&1 &
    spid=$!
    sleep 2
    mkfifo "$J_OUT.fifo"
    "$J_COUNT" <"$J_OUT.fifo" >"$J_OUT.count" &
    cpid=$!
    # The executable's text: a PIE under qemu-aarch64 loads at 0xaaaaaaaa0000 (checked with -d page).
    # shellcheck disable=SC2086 # J_ARGS is deliberately word-split into argv
    env BENCH_IFACE="$J_VA" timeout -s INT 300 "$QEMU" -L "$SYSROOT" -d in_asm,exec,nochain \
        -dfilter 0xaaaaaaaa0000+0x40000 -D "$J_OUT.fifo" "$J_BIN/client_$J_SHAPE" $J_ARGS >"$J_OUT.client.log" 2>&1
    echo "client_rc=$?" >"$J_OUT.status"
    wait "$cpid"
    case "$(readlink "/proc/$spid/exe")" in */qemu-aarch64) kill -INT "$spid" ;; esac
    sleep 2
    kill -KILL "$spid" 2>/dev/null
    rm -f "$J_OUT.fifo"
    exit 0
fi

ARMS=${ARMS:?ARMS=\"A=<commit> B=<commit> ...\"}
SHAPES=${SHAPES:-p1 p3}
REPS=${REPS:-3}
QDUR=${QDUR:-4}
OUTD=${OUT:-$HOME/rig_results_safe/aarch64_insn/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD" || exit 1
WORK=$(mktemp -d /tmp/aarch64_insn.XXXXXX) || exit 1
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() { [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf "$WORK"; return 0; }
trap cleanup EXIT
gcc -O2 -o "$WORK/count" "$HERE/qemu_tb_count.c" || exit 1
"$QEMU" --version | head -1 >"$OUTD/qemu_version"

build() { # $1 label $2 commit
    local t="$WORK/$1/src" o="$WORK/bin/$1" p sd arr
    mkdir -p "$t" "$o"
    git -C "$REPO" archive "$2" | tar -x -C "$t" || return 1
    for f in tickle encoding log hal_linux; do
        $CC -O2 -DNDEBUG -Wall -Wextra -I"$t/include" -c "$t/src/$f.c" -o "$o/$f.o" 2>>"$OUTD/build_$1.log" || return 1
    done
    aarch64-linux-gnu-ar rcs "$o/libtickle.a" "$o/tickle.o" "$o/encoding.o" "$o/log.o" "$o/hal_linux.o"
    for p in $SHAPES; do
        sd=$t/examples/perf_hil/tickle/common/$p
        arr=$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$sd/Bench.msg")
        for role in client server; do
            $CC -O2 -DBENCH_CORE_BUILD=release -DBENCH_SAMPLE_PATH=datagram -DBENCH_DATAGRAM_BYTES=1472 \
                -DBENCH_SAMPLE_BYTES=$((12 + arr)) -I"$sd" -I"$t/examples/perf_hil/tickle/common" -I"$t/include" \
                -o "$o/${role}_$p" "$t/examples/perf_hil/tickle/reliable_throughput/$role.c" "$sd/Bench.c" \
                "$o/libtickle.a" -lm -lpthread 2>>"$OUTD/build_$1.log" || return 1
        done
    done
}
LABELS=()
for spec in $ARMS; do
    l=${spec%%=*}
    LABELS+=("$l")
    build "$l" "${spec#*=}" || { echo "FATAL: build $l" | tee -a "$OUTD/summary.txt"; exit 1; }
done
# A2: A's binaries, run again - the A/A control.
cp -r "$WORK/bin/${LABELS[0]}" "$WORK/bin/A2"
LABELS+=(A2)

run_one() { # $1 label $2 shape $3 rep
    NSID=$((NSID + 1))
    local id="aq$$-$NSID" n # interface names below stay within IFNAMSIZ (15)
    case $2 in p1) n=2048 ;; p2) n=405 ;; p3) n=368 ;; p4) n=187 ;; esac
    local out="$OUTD/${2}_${1}_r$3"
    if ! { sudo -n ip netns add "${id}a" && sudo -n ip netns add "${id}b" &&
        sudo -n ip link add "${id}va" type veth peer name "${id}vb" && sudo -n ip link set "${id}va" netns "${id}a" &&
        sudo -n ip link set "${id}vb" netns "${id}b" &&
        sudo -n ip -n "${id}a" addr add 192.168.10.1/24 brd + dev "${id}va" &&
        sudo -n ip -n "${id}b" addr add 192.168.10.2/24 brd + dev "${id}vb" &&
        sudo -n ip -n "${id}a" link set lo up && sudo -n ip -n "${id}b" link set lo up &&
        sudo -n ip -n "${id}a" link set "${id}va" up && sudo -n ip -n "${id}b" link set "${id}vb" up; }; then
        echo "SETUP_FAIL $out" | tee -a "$OUTD/summary.txt"
        sudo -n ip netns del "${id}a" 2>/dev/null
        sudo -n ip netns del "${id}b" 2>/dev/null
        return
    fi
    sudo -n ip netns exec "${id}a" env QEMU="$QEMU" SYSROOT="$SYSROOT" J_SNS="${id}b" J_VA="${id}va" J_VB="${id}vb" \
        J_BIN="$WORK/bin/$1" J_SHAPE="$2" J_COUNT="$WORK/count" \
        J_ARGS="-Q -N $n -B 100 -d $QDUR --warmup-s 0.5 --cooldown-s 0.5" J_OUT="$out" bash "$0" --job
    sudo -n ip netns del "${id}a"
    sudo -n ip netns del "${id}b"
    local sent tot
    sent=$(grep -o ' sent=[0-9]*' "$out.client.log" | head -1 | cut -d= -f2)
    tot=$(awk '/^TOTAL/{print $2}' "$out.count")
    echo "QRUN arm=$1 shape=$2 rep=$3 sent=${sent:-0} insn=${tot:-0} per_sample=$(awk -v t="${tot:-0}" -v s="${sent:-0}" \
        'BEGIN{printf "%.1f", s ? t / s : 0}') drained=$(grep -o 'drained=[a-z]*' "$out.client.log" | head -1) \
        $(cat "$out.status")" | tee -a "$OUTD/summary.txt"
}
N=${#LABELS[@]}
NSID=0
for rep in $(seq 1 "$REPS"); do
    for shape in $SHAPES; do
        for k in $(seq 0 $((N - 1))); do run_one "${LABELS[$(((k + rep) % N))]}" "$shape" "$rep"; done
    done
done
echo "=== done: $OUTD ===" | tee -a "$OUTD/summary.txt"

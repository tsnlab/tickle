#!/usr/bin/env bash
# hybrid_v2_pc.sh - ab/hybrid-v2 against its parent on the PC: two private network namespaces, a veth pair, the
# publisher's egress shaped to 1 Gbit/s by tbf (the rig's link; tbf leaves packets charged to the socket until they go,
# so the send buffer fills as on the Pi's NIC), socket buffers 212,992 B as the rig grants. large_sample_bench.c at both
# ends, message in memory with the ranged encode (-M -G), forced IP (runs of 45, the reader's cap off), as
# hybrid_gaps_pc.sh ran.
#
# NOT a rig figure and never to be tabled as one. It checks, before the rig, that B does what it says on the PC.
#
# Arms (each built from a tree exported with git archive - the working tree is never modified):
#   A    PARENT (default c7262c89, origin/main): 64 runs a send call, the timer retry.
#   A2   A again, separate runs: the A/A floor.
#   B    TIP (default HEAD): a call of LARGE_BATCH units, the send resumed on writability.
#   Bt   TIP built -Dtt_LARGE_SEND_ON_WRITABLE=0: B with the timer retry - isolates the writability resume from the
#        call cap.
# PERF=1: every publisher runs under perf stat (instructions:u, instructions:k) - send-side instructions per MB, and
# nothing else from that session: counting perturbs the timing it would be read with. With perf stat on the publisher,
# B's 4 MB p99 read 39-48 ms against 34.5 ms (2026-10-12): its waits for writability lasted 9-24 ms while the send
# queue drained at a fifth of the link (SIOCOUTQ at the wait's end), and none did in the same runs without perf. So
# latency, CPU and send_waits are read from a PERF=0 session (the default), instructions from a PERF=1 one.
#
# HOW TO READ IT - written before the first run (2026-10-12), enforced by hybrid_v2_pc_read.py:
#   VOID     a run with torn > 0 or out_of_order > 0, a RELIABLE run with delivered < published, a BEST_EFFORT run
#            below 98%, a run without RESULT lines or perf counts; B with large_send_writable = 0 while
#            large_send_waits > 0 (the treatment did not apply), Bt or A with any large_send_writable= > 0. Fewer
#            than 3 runs of an arm in a cell: that cell's comparisons are not read.
#   FLOOR    per cell and metric, max(|mean(A2) - mean(A)|, 2 x SE of the difference read).
#   H1       (call cap) B latency median BETTER than A in every cell, by more in the 4 MB cells than in the 1 MB ones.
#   H2       (writability) Bt -> B: send_waits per MB down by at least half in every cell where Bt waited, and the
#            publisher's instructions per MB (user + kernel) BETTER or HELD - never WORSE; B -> A the same.
#   NO HARM  B against A: publisher and subscriber CPU ms per MB not WORSE in any cell.
#
# Usage: hybrid_v2_pc.sh [BLOCKS] [DURATION_S]   results appended to $OUT (default ~/largemsg_pc/hybrid_v2/results.txt)
# Needs passwordless `sudo ip` and `tc`. Run it detached (CLAUDE.md, "Long-running work"). Every bench process runs
# under `ulimit -v` and a timeout. No rig: PC only, in private namespaces.
set -u
BLOCKS=${1:-3}
DURATION=${2:-15}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
PARENT=${PARENT:-c7262c89}
TIP=${TIP:-HEAD}
WORK=${WORK:-$HOME/largemsg_pc/hybrid_v2}
OUT=${OUT:-$WORK/results.txt}
NS1=hv2-pub-$$
NS2=hv2-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
CELLS=${CELLS:-"be1m:1048576:30: be4m:4194304:15: rel1m:1048576:30:-R rel4m:4194304:15:-R"}
ARMS=${ARMS:-"A B Bt A2"}
PERF=${PERF:-0}
CFLAGS_BENCH=${CFLAGS_BENCH:-"-O2 -DNDEBUG"}
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK"
export_tree() { # <commit> <dir>
    rm -rf "$2"
    mkdir -p "$2"
    git -C "$TREE" archive "$1" src include examples/perf_hil/experiments/large_sample_bench.c | tar -x -C "$2"
}
build() { # <root> <out> [extra cflags]
    # shellcheck disable=SC2086 # CFLAGS_BENCH and the extra flags split on purpose
    cc $CFLAGS_BENCH ${3:-} -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$1/include" -I"$1/src" -o "$2" \
        "$1/examples/perf_hil/experiments/large_sample_bench.c" "$1/src/tickle.c" "$1/src/encoding.c" "$1/src/log.c" \
        "$1/src/hal_linux.c" -lpthread -lm
}
parent_sha=$(git -C "$TREE" rev-parse --short=8 "$PARENT") || exit 1
tip_sha=$(git -C "$TREE" rev-parse --short=8 "$TIP") || exit 1
export_tree "$parent_sha" "$WORK/src_A" || exit 1
export_tree "$tip_sha" "$WORK/src_B" || exit 1
build "$WORK/src_A" "$WORK/bench_A" || exit 1
build "$WORK/src_B" "$WORK/bench_B" || exit 1
build "$WORK/src_B" "$WORK/bench_Bt" "-Dtt_LARGE_SEND_ON_WRITABLE=0" || exit 1

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add hv2a netns "$NS1" type veth peer name hv2b netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.84.1/24 dev hv2a
sudo -n ip -n "$NS2" addr add 192.168.84.2/24 dev hv2b
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set hv2a up
sudo -n ip -n "$NS2" link set hv2b up
sudo -n ip netns exec "$NS1" tc qdisc add dev hv2a root tbf rate 1gbit burst 64kb latency 100ms || exit 1
echo "=== hybrid_v2_pc $(date -Is) perf=$PERF A=$parent_sha B=$tip_sha blocks=$BLOCKS duration=${DURATION}s cflags='$CFLAGS_BENCH' load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"
echo "SHAPE $(sudo -n ip netns exec "$NS1" tc qdisc show dev hv2a)" | tee -a "$OUT"

run() { # <cell> <arm> <block> <size> <rate> <qflag>
    local cell=$1 arm=$2 block=$3 size=$4 rate=$5 qflag=$6 bin
    case $arm in
        A | A2) bin=$WORK/bench_A ;;
        B) bin=$WORK/bench_B ;;
        Bt) bin=$WORK/bench_Bt ;;
        *) echo "unknown arm $arm"; return ;;
    esac
    local sublog=$WORK/sub.log publog=$WORK/pub.log perflog=$WORK/perf.txt
    rm -f "$perflog"
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin sub -s $size \
        -d $((DURATION + 8)) $qflag -C -b 192.168.84.255" >"$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    local counted=""
    [ "$PERF" = 1 ] && counted="perf stat -x, -e instructions:u,instructions:k -o $perflog"
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $counted $bin pub -s $size -r $rate -d $DURATION $qflag -F ip -M -G \
        -b 192.168.84.255" >"$publog" 2>&1
    wait "$sub_pid"
    local p s iu ik
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    iu=$(grep -m1 'instructions:u' "$perflog" 2>/dev/null | cut -d, -f1)
    ik=$(grep -m1 'instructions:k' "$perflog" 2>/dev/null | cut -d, -f1)
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN cell=$cell arm=$arm block=$block VOID no RESULT (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    echo "RUN cell=$cell arm=$arm block=$block insn_u=${iu:-none} insn_k=${ik:-none} | $p | $s" >>"$OUT"
    echo "RUN cell=$cell arm=$arm block=$block $(grep -o 'latency_median_us=[0-9.]*' <<<"$s") $(grep -o 'large_send_waits=[0-9]*' <<<"$p") $(grep -o 'large_send_writable=[0-9]*' <<<"$p")"
}

# Arms rotate per block (each block starts one arm later), cells in turn.
read -r -a arm_list <<<"$ARMS"
for block in $(seq 1 "$BLOCKS"); do
    for spec in $CELLS; do
        IFS=: read -r cell size rate qflag <<<"$spec"
        n=${#arm_list[@]}
        for i in $(seq 0 $((n - 1))); do
            run "$cell" "${arm_list[$(((i + block - 1) % n))]}" "$block" "$size" "$rate" "$qflag"
        done
    done
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"

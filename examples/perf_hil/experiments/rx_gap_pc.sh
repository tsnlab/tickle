#!/usr/bin/env bash
# rx_gap_pc.sh - the receive side's cost per MB of a large sample, one commit against another, on the PC: two private
# network namespaces, a veth pair, the publisher's egress shaped to 1 Gbit/s by tbf (the rig's link), socket buffers
# 212,992 B as the rig grants. large_sample_bench.c at both ends, built as rmw_tickle builds core (65507-byte datagram,
# large samples), the shared-memory transport compiled out (the namespaces share /dev/shm). The publisher sends the
# message from memory with the ranged encode (-M -G), forced IP (runs of 45 units; the reader's cap off, -C), so every
# arm receives the same datagrams. The SUBSCRIBER runs under perf stat: instructions and cycles, user and kernel.
#
# For the receive CPU gap to Fast DDS (~/claude_reports/rx_perf_gap_2026-10-13.md): each change of ab/rx-gap is
# measured against its parent here before the rig sees it. NOT a rig figure and never to be tabled as one: x86, veth,
# and the subscriber reads every cache line of every sample it receives (-K 64: a check of every 64th byte and the last;
# a check of every byte cost ~11 M user instructions per MB and buried the receive path under it).
#
# Arms (each built from a tree exported with git archive - the working tree is never modified):
#   A    PARENT (default HEAD~1)   A2   A again, separate runs: the A/A floor   B    TIP (default HEAD)
# Cells: rel1m (1 MiB 30 Hz RELIABLE) and rel4m (4 MiB 15 Hz RELIABLE), 0% loss - perf_test's Array1m and Array4m
# cells as the rig runs them. CELLS can add be1m/be4m (BEST_EFFORT: no -R).
#
# HOW TO READ IT - written before the first run (2026-10-13), enforced by rx_gap_pc_read.py:
#   VOID     a run with torn > 0 or out_of_order > 0, a RELIABLE run with delivered < published, a BEST_EFFORT run below
#            98%, a run without RESULT lines or perf counts. Fewer than 2 runs of an arm in a cell: not read.
#   FLOOR    per cell and metric, max(|mean(A2) - mean(A)|, 2 x SE of the B - A difference).
#   METRIC   subscriber instructions per MB delivered, user + kernel (PRIMARY), and each half; cycles per MB beside it.
#   VERDICT  per cell: BETTER when B < A by more than the floor, WORSE when B > A by more than it, else HELD. A change
#            whose claim is "fewer receive instructions" is supported only by BETTER in both RELIABLE cells, and is
#            refuted by WORSE in either; latency median is printed (from the same runs: perf stat counts, it does not
#            sample) and must not be WORSE by more than its own floor.
#
# Usage: rx_gap_pc.sh [BLOCKS] [DURATION_S]   PARENT= TIP= (commits), OUT= (default ~/largemsg_pc/rx_gap/results.txt),
#        EXTRA_B= (cflags for B only, e.g. an A/B switch). Needs passwordless `sudo ip` and `tc`. Run it detached
#        (CLAUDE.md, "Long-running work"). Every bench process runs under `ulimit -v` and a timeout. PC only.
# The last line is always "rx_gap_pc done rc=<n>".
set -u
BLOCKS=${1:-3}
DURATION=${2:-12}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
PARENT=${PARENT:-HEAD~1}
TIP=${TIP:-HEAD}
WORK=${WORK:-$HOME/largemsg_pc/rx_gap}
OUT=${OUT:-$WORK/results.txt}
NS1=rxg-pub-$$
NS2=rxg-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
CELLS=${CELLS:-"rel1m:1048576:30:-R rel4m:4194304:15:-R"}
ARMS=${ARMS:-"A B A2"}
CFLAGS_BENCH=${CFLAGS_BENCH:-"-O2 -g -DNDEBUG"}
EXTRA_B=${EXTRA_B:-}
CHECK_STRIDE=${CHECK_STRIDE:-64} # the subscriber checks every 64th byte: every cache line, not every byte (-K)
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "rx_gap_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK" "$TMPDIR"
export_tree() { # <commit> <dir>
    rm -rf "$2"
    mkdir -p "$2"
    git -C "$TREE" archive "$1" src include examples/perf_hil/experiments/large_sample_bench.c | tar -x -C "$2"
}
build() { # <root> <out> [extra cflags]   (the bench itself always from TIP: -K, the same harness in every arm)
    # shellcheck disable=SC2086 # CFLAGS_BENCH and the extra flags split on purpose
    cc $CFLAGS_BENCH ${3:-} -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$1/include" -I"$1/src" -o "$2" \
        "$WORK/src_B/examples/perf_hil/experiments/large_sample_bench.c" "$1/src/tickle.c" "$1/src/encoding.c" "$1/src/log.c" \
        "$1/src/hal_linux.c" -lpthread -lm
}
parent_sha=$(git -C "$TREE" rev-parse --short=8 "$PARENT") || exit 1
tip_sha=$(git -C "$TREE" rev-parse --short=8 "$TIP") || exit 1
export_tree "$parent_sha" "$WORK/src_A" || exit 1
export_tree "$tip_sha" "$WORK/src_B" || exit 1
build "$WORK/src_A" "$WORK/bench_A" || exit 1
build "$WORK/src_B" "$WORK/bench_B" "$EXTRA_B" || exit 1

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add rxga netns "$NS1" type veth peer name rxgb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.86.1/24 dev rxga
sudo -n ip -n "$NS2" addr add 192.168.86.2/24 dev rxgb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set rxga up
sudo -n ip -n "$NS2" link set rxgb up
sudo -n ip netns exec "$NS1" tc qdisc add dev rxga root tbf rate 1gbit burst 64kb latency 100ms || exit 1
echo "=== rx_gap_pc $(date -Is) A=$parent_sha B=$tip_sha extra_b='$EXTRA_B' blocks=$BLOCKS duration=${DURATION}s cflags='$CFLAGS_BENCH' load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

run() { # <cell> <arm> <block> <size> <rate> <qflag>
    local cell=$1 arm=$2 block=$3 size=$4 rate=$5 qflag=$6 bin
    case $arm in
        A | A2) bin=$WORK/bench_A ;;
        B) bin=$WORK/bench_B ;;
        *) echo "unknown arm $arm"; return ;;
    esac
    local sublog=$WORK/sub.log publog=$WORK/pub.log perflog=$WORK/perf.txt
    rm -f "$perflog"
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 40)) perf stat -x, \
        -e instructions:u,instructions:k,cycles:u,cycles:k -o $perflog $bin sub -s $size -d $((DURATION + 8)) $qflag -C \
        -K $CHECK_STRIDE -b 192.168.86.255" >"$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin pub -s $size -r $rate \
        -d $DURATION $qflag -F ip -M -G -b 192.168.86.255" >"$publog" 2>&1
    wait "$sub_pid"
    local p s iu ik cu ck rx
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    iu=$(grep -m1 'instructions:u' "$perflog" 2>/dev/null | cut -d, -f1)
    ik=$(grep -m1 'instructions:k' "$perflog" 2>/dev/null | cut -d, -f1)
    cu=$(grep -m1 'cycles:u' "$perflog" 2>/dev/null | cut -d, -f1)
    ck=$(grep -m1 'cycles:k' "$perflog" 2>/dev/null | cut -d, -f1)
    rx=$(grep -m1 -o 'rx: .*' "$sublog" | tr ' ' '\n' | grep -E '^(drain_starts|drain_datagrams|direct_[a-z]+)=' | tr '\n' ' ')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN cell=$cell arm=$arm block=$block VOID no RESULT (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    echo "RUN cell=$cell arm=$arm block=$block insn_u=${iu:-none} insn_k=${ik:-none} cyc_u=${cu:-none} cyc_k=${ck:-none} $rx| $p | $s" >>"$OUT"
    echo "RUN cell=$cell arm=$arm block=$block insn_u=${iu:-none} insn_k=${ik:-none} $(grep -o 'delivered=[0-9]*' <<<"$s") $(grep -o 'latency_median_us=[0-9.]*' <<<"$s") $rx"
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
python3 "$HERE/rx_gap_pc_read.py" "$OUT" | tee -a "$OUT.read"
rc=${PIPESTATUS[0]}

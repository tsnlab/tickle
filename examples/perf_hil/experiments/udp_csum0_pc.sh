#!/usr/bin/env bash
# udp_csum0_pc.sh - UDP checksum 0 on IP-fragmented datagrams (hal_linux.c "UDP checksum 0", TT_UDP_CSUM0=1) against
# the same binary without it, on the PC: rx_gap_pc.sh's network (two private network namespaces, a veth pair, the
# publisher's egress shaped to 1 Gbit/s by tbf, socket buffers 212,992 B as the rig grants), large_sample_bench.c at
# both ends built ONCE from TIP as rmw_tickle builds core. BOTH ends run under perf stat (instructions and cycles, user
# and kernel). The publisher in the rig's regime (AUTO, the reader's cap on), so its samples go as IP-fragmented runs.
#
# Arms (one binary; the environment is given to both ends):
#   OFF   TT_UDP_CSUM0 unset - the control     ON   TT_UDP_CSUM0=1     OFF2   OFF again, separate runs: the A/A floor
# Cells: be1m rel1m (1 MiB 30 Hz) and be4m rel4m (4 MiB 15 Hz), BEST_EFFORT and RELIABLE, 0% loss.
#
# Expected before the first run (2026-10-14): the receiver checks an IP-fragmented datagram's checksum in software (no
# NIC or veth can check a fragment's), as part of the copy to user. On x86 that copy and the checksum are one fused pass
# (csum_partial_copy_generic), so checksum 0 should show as fewer SUBSCRIBER kernel instructions per MB (the add-with-
# carry chain, one instruction per 8 bytes: of the order of 0.15-0.3 M per MB) and fewer cycles only if the fused pass
# was compute-bound. The PUBLISHER computes the checksum of an IP-fragmented datagram as it copies from user whether
# SO_NO_CHECK is set or not (the option only leaves it out of the header), and pays two setsockopt() calls per
# sendmmsg() call: expected HELD. The rig (arm64: the checksum is a separate csum_partial() pass over cold data before
# the copy) is where the saving is priced; the PC only shows the mechanism, and a HELD here is not read as "nothing".
#
# HOW TO READ IT - written before the first run (2026-10-14), enforced by udp_csum0_pc_read.py:
#   VOID       a run with torn > 0 or out_of_order > 0, a RELIABLE run with delivered < published, a BEST_EFFORT run
#              below 98%, no RESULT lines or no perf counts for either end.
#   TREATMENT  each arm reports its own on the publisher's "Node <n> rx:" line: ON needs csum0=1 and csum0_datagrams
#              at least 1 a sample published; OFF and OFF2 need csum0=0 and csum0_datagrams=0. A line or field missing:
#              the run is VOID (NOT READ - "could not look" is not "off").
#   METRICS    per MB delivered: subscriber instructions user + kernel (PRIMARY), subscriber kernel instructions, cycles;
#              publisher instructions user + kernel, kernel, cycles (per MB published).
#   FLOOR      per cell and metric, max(|mean(OFF2) - mean(OFF)|, 2 x SE of the ON - OFF difference). Fewer than 2 runs
#              of an arm in a cell: that cell not read.
#   VERDICT    per cell and metric: LOWER when ON < OFF by more than the floor, HIGHER when above it, else HELD.
#   READING    subscriber PRIMARY LOWER in all four cells: "VISIBLE on the PC". HELD in all four: "NOT VISIBLE on the
#              PC" (not evidence about the rig). HIGHER in any cell: "REFUTED on the PC". Anything else: "MIXED".
#              Publisher PRIMARY HIGHER in any cell: "PUBLISHER HARM" beside the reading (the setsockopt() pair).
# Usage: udp_csum0_pc.sh [BLOCKS] [DURATION_S]   TIP= (commit, default HEAD), CELLS=, OUT= (default
#        ~/largemsg_pc/udp_csum0/results.txt). Needs passwordless `sudo ip` and `tc`. Run it detached (CLAUDE.md,
#        "Long-running work"). Every bench process runs under `ulimit -v` and a timeout. PC only, never a rig figure.
# The last line is always "udp_csum0_pc done rc=<n>".
set -u
BLOCKS=${1:-3}
DURATION=${2:-12}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
TIP=${TIP:-HEAD}
WORK=${WORK:-$HOME/largemsg_pc/udp_csum0}
OUT=${OUT:-$WORK/results.txt}
NS1=uc0-pub-$$
NS2=uc0-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
CELLS=${CELLS:-"be1m:1048576:30: rel1m:1048576:30:-R be4m:4194304:15: rel4m:4194304:15:-R"}
ARMS="OFF ON OFF2"
CFLAGS_BENCH=${CFLAGS_BENCH:-"-O2 -g -DNDEBUG"}
PUB_FLAGS=${PUB_FLAGS:-"-F auto -M -G"} # the rig's regime: AUTO, the reader's cap on
SUB_FLAGS=${SUB_FLAGS-}
CHECK_STRIDE=${CHECK_STRIDE:-64} # the subscriber checks every 64th byte: every cache line (rx_gap_pc.sh)
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "udp_csum0_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK" "$TMPDIR"
sha=$(git -C "$TREE" rev-parse --short=8 "$TIP") || exit 1
rm -rf "$WORK/src" && mkdir -p "$WORK/src"
git -C "$TREE" archive "$sha" src include examples/perf_hil/experiments/large_sample_bench.c | tar -x -C "$WORK/src" ||
    exit 1
grep -q 'TT_UDP_CSUM0' "$WORK/src/src/hal_linux.c" || { echo "REFUSED: $sha's hal_linux.c has no TT_UDP_CSUM0"; exit 1; }
# shellcheck disable=SC2086 # CFLAGS_BENCH splits on purpose
cc $CFLAGS_BENCH -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$WORK/src/include" -I"$WORK/src/src" -o "$WORK/bench" \
    "$WORK/src/examples/perf_hil/experiments/large_sample_bench.c" "$WORK/src/src/tickle.c" "$WORK/src/src/encoding.c" \
    "$WORK/src/src/log.c" "$WORK/src/src/hal_linux.c" -lpthread -lm 2>"$WORK/build.log" || { cat "$WORK/build.log"; exit 1; }

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add uc0a netns "$NS1" type veth peer name uc0b netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.88.1/24 dev uc0a
sudo -n ip -n "$NS2" addr add 192.168.88.2/24 dev uc0b
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set uc0a up
sudo -n ip -n "$NS2" link set uc0b up
sudo -n ip netns exec "$NS1" tc qdisc add dev uc0a root tbf rate 1gbit burst 64kb latency 100ms || exit 1
echo "=== udp_csum0_pc $(date -Is) tip=$sha arms='$ARMS' pub_flags='$PUB_FLAGS' sub_flags='$SUB_FLAGS' blocks=$BLOCKS duration=${DURATION}s cflags='$CFLAGS_BENCH' load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

EVENTS=instructions:u,instructions:k,cycles:u,cycles:k
run() { # <cell> <arm> <block> <size> <rate> <qflag>
    local cell=$1 arm=$2 block=$3 size=$4 rate=$5 qflag=$6 envs="env -u TT_UDP_CSUM0"
    [ "$arm" = ON ] && envs="env TT_UDP_CSUM0=1"
    local sublog=$WORK/sub.log publog=$WORK/pub.log subperf=$WORK/perf_sub.txt pubperf=$WORK/perf_pub.txt
    rm -f "$subperf" "$pubperf"
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 40)) perf stat -x, -e $EVENTS \
        -o $subperf $envs $WORK/bench sub -s $size -d $((DURATION + 8)) $qflag $SUB_FLAGS -K $CHECK_STRIDE \
        -b 192.168.88.255" >"$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) perf stat -x, -e $EVENTS \
        -o $pubperf $envs $WORK/bench pub -s $size -r $rate -d $DURATION $qflag $PUB_FLAGS -b 192.168.88.255" \
        >"$publog" 2>&1
    wait "$sub_pid"
    if [ "${KEEP_LOGS:-0}" = 1 ]; then
        mkdir -p "$WORK/logs"
        cp "$sublog" "$WORK/logs/${cell}_${arm}_${block}.sub.log"
        cp "$publog" "$WORK/logs/${cell}_${arm}_${block}.pub.log"
    fi
    local p s counts="" side f v tx
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN cell=$cell arm=$arm block=$block VOID no RESULT (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    for side in sub pub; do
        f=$WORK/perf_$side.txt
        for v in instructions:u instructions:k cycles:u cycles:k; do
            counts+="${side}_${v/:/_}=$(grep -m1 "$v" "$f" 2>/dev/null | cut -d, -f1 | grep -E '^[0-9]+$' || echo none) "
        done
    done
    # The publisher's treatment (csum0*) and the subscriber's, from each end's rx line; absent fields stay absent.
    tx=$(grep -m1 -o 'rx: .*' "$publog" | tr ' ' '\n' | grep -E '^csum0[a-z_]*=' | sed 's/^/pub_/' | tr '\n' ' ')
    tx+=$(grep -m1 -o 'rx: .*' "$sublog" | tr ' ' '\n' | grep -E '^(csum0|socket_drops)=' | sed 's/^/sub_/' | tr '\n' ' ')
    echo "RUN cell=$cell arm=$arm block=$block $counts$tx| $p | $s" >>"$OUT"
    echo "RUN cell=$cell arm=$arm block=$block $counts$tx$(grep -o 'delivered=[0-9]*' <<<"$s")"
}

# Arms rotate per block (each block starts one arm later), cells in turn.
read -r -a arm_list <<<"$ARMS"
n=${#arm_list[@]}
for block in $(seq 1 "$BLOCKS"); do
    for spec in $CELLS; do
        IFS=: read -r cell size rate qflag <<<"$spec"
        for i in $(seq 0 $((n - 1))); do
            run "$cell" "${arm_list[$(((i + block - 1) % n))]}" "$block" "$size" "$rate" "$qflag"
        done
    done
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"
python3 "$HERE/udp_csum0_pc_read.py" "$OUT" | tee -a "$OUT.read"
rc=${PIPESTATUS[0]}

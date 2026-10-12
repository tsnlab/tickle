#!/usr/bin/env bash
# hybrid_gaps_pc.sh - where the hybrid transport's latency goes against MTU fragments (2026-10-12 rig scorecard: 1 MB
# +0.14 ms, 4 MB +0.44 ms median against main), on the PC: two private network namespaces, a veth pair, the publisher's
# egress shaped to 1 Gbit/s by tbf (the rig's link; tbf leaves packets charged to the socket until they go, so the send
# buffer fills as on the Pi's NIC), send and receive buffers 212,992 B as the rig grants. large_sample_bench.c at both
# ends, message already in memory with the ranged encode (-M -G: rmw_tickle's Array encode, interleaved with the send).
#
# NOT a rig figure and never to be tabled as one. It decides which candidate fix is worth a rig A/B.
#
# The question: is the run path's extra latency (a) the encode moved back onto the critical path - large_build_runs()
# (tickle.c) packs up to LARGE_BATCH = 64 runs (2,880 units, the whole of a 1 MB sample, 4.1 MB of a 4 MB one) into one
# send call and large_encode_through() encodes all of it before the first sendmmsg(), where the MTU path encodes 64
# units (93 KB) a call - or (b) store-and-forward: the kernel hands a run up only once its last IP fragment is in, so the
# last run's copies are not overlapped with the wire, or (c) something else.
#
# Arms (binaries: head = this tree as is; cK = a COPY of this tree's src with at most K runs per send call - the tree
# itself is never modified; the patch is checked to have matched exactly once):
#   mtu     head, -F mtu: MTU fragments, the path main had. CONTROL: the cK patch provably cannot touch it (it edits only
#           large_build_runs(), which MTU mode never calls), so head-mtu vs cK-mtu is the patch's noise.
#   ip45    head, -F ip, reader cap off: runs of 45, up to 64 runs a call - what the rig ran (AUTO reached 45 at 0%).
#   ip45c1  c1, -F ip: runs of 45, one run a call (65 KB): candidate fix A, the encode pipelined run by run.
#   ip45c2  c2, -F ip: two runs a call (131 KB, about the MTU path's 93 KB).
#   ip8c11  c11, -F ip -n 8: runs of 8, 11 a call (88 units, ~ip45c2's 90): same bytes a call, runs 5.6x shorter -
#           isolates store-and-forward (b) from the batching (a).
#   mtuc    c2's binary, -F mtu: the control's second half (patched binary, untouched path).
#
# HOW TO READ IT - written before the first run (2026-10-12), enforced by hybrid_gaps_read.py:
#   VOID     a run with torn > 0 or out_of_order > 0; a RELIABLE run with delivered < published; a BEST_EFFORT run
#            delivering < 98% of published; an ip* arm whose publisher's n histogram's largest bucket is not the forced
#            n (45: bucket n_max; 8: the bucket holding 8), or an mtu arm that sent any run. Fewer than 3 runs of an arm
#            in a cell: that cell's comparisons are not read.
#   FLOOR    per cell, floor = max(|mean(mtuc) - mean(mtu)|, 2 x SE of the difference read). Verdicts by d against it.
#   H_A      (batching) ip45c1 latency median BETTER than ip45 in every cell, and the 4 MB gain larger than the 1 MB
#            gain. FALSIFIED if ip45c1 is HELD in either 4 MB cell.
#   H_A2     ip45c1 vs mtu: HELD or BETTER in every cell (the run path costs no latency once pipelined). If WORSE, the
#            remainder is (b) or (c); its size is printed.
#   H_B      (store-and-forward) ip8c11 vs ip45c2: BETTER means the last run's size is on the latency path; its
#            size is printed (expected: the copy time of ~57 KB, tens of us). HELD: (b) is below the floor here.
#   NO HARM  for any fix: publisher and subscriber cpu_ms_per_mb of ip45c1 not WORSE than ip45 beyond the floor.
#
# Usage: hybrid_gaps_pc.sh [BLOCKS] [DURATION_S]    results appended to $OUT (default ~/largemsg_pc/hybrid_gaps/results.txt)
# Needs passwordless `sudo ip` and `tc`. Run it detached (CLAUDE.md, "Long-running work"). Every bench process runs
# under `ulimit -v` and a timeout. No rig: PC only, in private namespaces.
set -u
BLOCKS=${1:-3}
DURATION=${2:-20}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc/hybrid_gaps}
OUT=${OUT:-$WORK/results.txt}
NS1=hgap-pub-$$
NS2=hgap-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
CELLS=${CELLS:-"be1m:1048576:30: be4m:4194304:15: rel1m:1048576:30:-R rel4m:4194304:15:-R"}
ARMS=${ARMS:-"mtu ip45 ip45c1 ip45c2 ip8c11 mtuc"}
CFLAGS_BENCH=${CFLAGS_BENCH:-"-O2 -DNDEBUG"}
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK"
# A copy of the tree's sources with at most K runs a send call. The tree is not touched.
patched_src() { # <K> -> prints the copy's root
    local k=$1 root=$WORK/src_c$1
    rm -rf "$root"
    mkdir -p "$root"
    cp -r "$TREE/src" "$TREE/include" "$root/"
    local pattern='for (; k < LARGE_BATCH \&\& next < last; k++) {'
    local hits
    hits=$(grep -c "for (; k < LARGE_BATCH && next < last; k++) {" "$root/src/tickle.c")
    if [ "$hits" != 1 ]; then
        echo "PATCH c$k: expected 1 match in large_build_runs(), found $hits" >&2
        return 1
    fi
    sed -i "s/$pattern/for (; k < LARGE_BATCH \&\& k < $k \&\& next < last; k++) {/" "$root/src/tickle.c"
    if [ "$(grep -c "k < LARGE_BATCH && k < $k && next < last" "$root/src/tickle.c")" != 1 ]; then
        echo "PATCH c$k: the substitution did not land" >&2
        return 1
    fi
    echo "$root"
}
build() { # <root> <out>
    # shellcheck disable=SC2086 # CFLAGS_BENCH split on purpose
    cc $CFLAGS_BENCH -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$1/include" -I"$1/src" -o "$2" "$HERE/large_sample_bench.c" \
        "$1/src/tickle.c" "$1/src/encoding.c" "$1/src/log.c" "$1/src/hal_linux.c" -lpthread -lm
}
build "$TREE" "$WORK/bench_head" || exit 1
for k in 1 2 11; do
    root=$(patched_src "$k") || exit 1
    build "$root" "$WORK/bench_c$k" || exit 1
done
head_sha=$(git -C "$TREE" rev-parse --short HEAD)
dirty=$(git -C "$TREE" status --porcelain -- src include examples/perf_hil/experiments/large_sample_bench.c | wc -l)

# One cold 1 MiB and 4 MiB memcpy on this PC: what a whole-sample encode costs before the first send.
memcpy_us=$(python3 - <<'EOF'
import ctypes, time
out = []
for n in (1048576, 4194304):
    src = ctypes.create_string_buffer(b"\x5a" * n, n)
    best = None
    for _ in range(20):
        dst = ctypes.create_string_buffer(n)
        t = time.perf_counter_ns(); ctypes.memmove(dst, src, n); dt = time.perf_counter_ns() - t
        best = dt if best is None else min(best, dt)
    out.append(f"{best / 1000:.0f}")
print("/".join(out))
EOF
)

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add hgap1 netns "$NS1" type veth peer name hgap2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.83.1/24 dev hgap1
sudo -n ip -n "$NS2" addr add 192.168.83.2/24 dev hgap2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set hgap1 up
sudo -n ip -n "$NS2" link set hgap2 up
sudo -n ip netns exec "$NS1" tc qdisc add dev hgap1 root tbf rate 1gbit burst 64kb latency 100ms || exit 1
echo "=== hybrid_gaps_pc $(date -Is) head=$head_sha dirty=$dirty blocks=$BLOCKS duration=${DURATION}s memcpy_us(1MiB/4MiB)=$memcpy_us cflags='$CFLAGS_BENCH' load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"
echo "SHAPE $(sudo -n ip netns exec "$NS1" tc qdisc show dev hgap1)" | tee -a "$OUT"

run() { # <cell> <arm> <block> <size> <rate> <qflag>
    local cell=$1 arm=$2 block=$3 size=$4 rate=$5 qflag=$6 bin fflag
    case $arm in
        mtu) bin=$WORK/bench_head fflag="-F mtu" ;;
        mtuc) bin=$WORK/bench_c2 fflag="-F mtu" ;;
        ip45) bin=$WORK/bench_head fflag="-F ip" ;;
        ip45c1) bin=$WORK/bench_c1 fflag="-F ip" ;;
        ip45c2) bin=$WORK/bench_c2 fflag="-F ip" ;;
        ip8c11) bin=$WORK/bench_c11 fflag="-F ip -n 8" ;;
        *) echo "unknown arm $arm"; return ;;
    esac
    local sublog=$WORK/sub.log publog=$WORK/pub.log
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin sub -s $size \
        -d $((DURATION + 8)) $qflag -C -b 192.168.83.255" >"$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin pub -s $size \
        -r $rate -d $DURATION $qflag $fflag -M -G -b 192.168.83.255" >"$publog" 2>&1
    wait "$sub_pid"
    local p s
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN cell=$cell arm=$arm block=$block VOID no RESULT (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    echo "RUN cell=$cell arm=$arm block=$block | $p | $s" >>"$OUT"
    echo "RUN cell=$cell arm=$arm block=$block $(grep -o 'latency_median_us=[0-9.]*' <<<"$s")"
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

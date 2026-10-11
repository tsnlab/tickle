#!/usr/bin/env bash
# interleave_encode_pc.sh - PC A/B of encoding a large sample while it is sent (docs/DESIGN.md section 8, "Encoding
# while sending"): the publish copies each range of a 4 MB sample just before the send call that carries it, instead of
# all of it before the first datagram leaves.
#
# Two private network namespaces joined by a veth pair, the publisher's egress shaped to 1 Gbit/s (the rig's link; a
# 4 MB sample is ~35 ms of it) in two ways, a cell each: netem rate (as asked), which takes a whole burst into its queue
# at once, so the socket's send buffer never fills (send_waits 0); and tbf, which leaves the packets charged to the
# socket until they go, so the buffer fills and the send waits as on the rig's NIC (send_waits in the thousands). Found
# by a shaping probe before any A/B was read (2026-10-11); the second cell is the one that exercises the encode of the
# rest when the buffer fills. large_sample_bench.c at both ends with rmw's datagram and the shared-memory
# transport compiled out. The sample is a message already in memory, encoded by memcpy (-M: rmw_tickle's encode of a
# type whose wire bytes are its message, perf_test's Array4m); its send time is stamped just before the publish.
#
# Arms, one binary each, run in ABBA order within every block (P H0 H1 H1 H0 P):
#   P   the parent tree (BASE, default origin/ab/largemsg-latency): whole encode, then send. The bench source is this
#       tree's, built there with -DBENCH_NO_RANGE.
#   H0  this tree, the topic WITHOUT a ranged encode (-M): the old path through the new code. P vs H0 is the control -
#       the change provably does not touch this path - and its delta is the noise floor.
#   H1  this tree WITH the ranged encode (-M -G): the treatment.
#
# NOT a rig figure and never to be tabled as one. The rig's I4 cells are the evidence (interleave_encode_ab.py).
#
# HOW TO READ IT - written before the first run (2026-10-11), and enforced by interleave_encode_ab.py, not here:
#   VOID     a run with torn > 0, or out_of_order > 0, or (RELIABLE) delivered < published; an H1 run with
#            range_calls = 0, or a P/H0 run with range_calls != 0 (the arm did not get its treatment); fewer than 3
#            runs of an arm in a cell.
#   FLOOR    per metric, floor = max(|mean(H0) - mean(P)|, 2 x SE(H1 - H0)) (Welch).
#   VERDICT  d = mean(H1) - mean(H0). BETTER iff d < -floor; WORSE iff d > +floor; else HELD.
#   HYPOTHESIS the latency median comes out BETTER in every 4 MB cell (both shapings), by at least half the time of the
#            encode it moves off the critical path (the bench prints none; it is taken from P: one 4 MiB memcpy, which
#            the reader measures in-process at the start, `memcpy_us`). FALSIFIED if any 4 MB cell's median is HELD
#            or WORSE: the first datagram's wait was not on the latency path here (the wire queue hid it).
#   NO HARM  latency p99 and publisher CPU ms/MB not WORSE in any cell; subscriber CPU ms/MB not WORSE.
#
# Usage: interleave_encode_pc.sh [BLOCKS] [DURATION_S]   results appended to $OUT (default /tmp/interleave_pc/results.txt)
# Needs passwordless `sudo ip` and `tc`. Takes the rig lock? No: nothing here touches the rig; it is PC-only, in netns.
set -u
BLOCKS=${1:-2}
DURATION=${2:-20}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
BASE=${BASE:-origin/ab/largemsg-latency}
WORK=${WORK:-/tmp/interleave_pc}
OUT=${OUT:-$WORK/results.txt}
NS1=ilv-pub-$$
NS2=ilv-sub-$$
CELLS=${CELLS:-"rel4m_netem:4194304:15:-R:netem be4m_netem:4194304:15::netem rel4m_tbf:4194304:15:-R:tbf be4m_tbf:4194304:15::tbf"}
declare -A SHAPER=([netem]="netem rate 1gbit limit 100000" [tbf]="tbf rate 1gbit burst 64kb latency 500ms")

netns_cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
cleanup() { # the one EXIT trap
    netns_cleanup
    [ -d "$WORK/parent" ] && git -C "$TREE" worktree remove --force "$WORK/parent" 2>/dev/null
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK"
build() { # build TREE_DIR OUT_BIN EXTRA_FLAGS
    cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 ${3:+"$3"} \
        -I"$1/include" -I"$1/src" -o "$2" "$HERE/large_sample_bench.c" "$1/src/tickle.c" "$1/src/encoding.c" \
        "$1/src/log.c" "$1/src/hal_linux.c" -lpthread -lm
}
git -C "$TREE" worktree remove --force "$WORK/parent" 2>/dev/null
git -C "$TREE" worktree add --detach "$WORK/parent" "$BASE" >/dev/null 2>&1 || { echo "cannot check out $BASE"; exit 1; }
build "$WORK/parent" "$WORK/bench_P" -DBENCH_NO_RANGE || exit 1
build "$TREE" "$WORK/bench_H" "" || exit 1
base_sha=$(git -C "$WORK/parent" rev-parse --short HEAD)
head_sha=$(git -C "$TREE" rev-parse --short HEAD)
dirty=$(git -C "$TREE" status --porcelain -- src include examples/perf_hil/experiments/large_sample_bench.c | wc -l)

# One cold 4 MiB memcpy on this PC, for the hypothesis's threshold: what the whole encode costs before the first send.
memcpy_us=$(python3 - <<'EOF'
import ctypes, time
n = 4194304
src = ctypes.create_string_buffer(b"\x5a" * n, n)
best = None
for _ in range(20):
    dst = ctypes.create_string_buffer(n)  # fresh, as a large buffer is
    t = time.perf_counter_ns(); ctypes.memmove(dst, src, n); dt = time.perf_counter_ns() - t
    best = dt if best is None else min(best, dt)
print(f"{best / 1000:.0f}")
EOF
)

netns_cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add ilv1 netns "$NS1" type veth peer name ilv2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev ilv1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev ilv2
for ns in "$NS1" "$NS2"; do
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NS1" link set ilv1 up
sudo -n ip -n "$NS2" link set ilv2 up
echo "=== interleave_encode_pc $(date -Is) base=$base_sha head=$head_sha dirty=$dirty blocks=$BLOCKS duration=${DURATION}s memcpy_us=$memcpy_us ===" | tee -a "$OUT"

shape() { # the rig's link, 1 Gbit/s, by netem or tbf (see the header); what the kernel installed is printed
    sudo -n ip netns exec "$NS1" tc qdisc del dev ilv1 root 2>/dev/null
    # shellcheck disable=SC2086 # the qdisc's arguments, split on purpose
    sudo -n ip netns exec "$NS1" tc qdisc add dev ilv1 root ${SHAPER[$1]} || return 1
    echo "SHAPE $1: $(sudo -n ip netns exec "$NS1" tc qdisc show dev ilv1)" | tee -a "$OUT"
}

run() { # run CELL ARM BLOCK SIZE RATE FLAGS
    local cell=$1 arm=$2 block=$3 size=$4 rate=$5 flags=$6 bin pub_flags
    case $arm in
        P) bin=$WORK/bench_P pub_flags="-M" ;;
        H0) bin=$WORK/bench_H pub_flags="-M" ;;
        H1) bin=$WORK/bench_H pub_flags="-M -G" ;;
    esac
    local sublog=$WORK/sub.log publog=$WORK/pub.log
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" "$bin" sub -s "$size" -d $((DURATION + 8)) $flags > "$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" "$bin" pub -s "$size" -r "$rate" -d "$DURATION" $flags $pub_flags > "$publog" 2>&1
    wait "$sub_pid"
    local p s
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN cell=$cell arm=$arm block=$block VOID no RESULT line (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    echo "RUN cell=$cell arm=$arm block=$block | $p | $s" >> "$OUT"
    echo "RUN cell=$cell arm=$arm block=$block $(grep -o 'latency_median_us=[0-9.]*' <<<"$s") $(grep -o 'large_send_waits=[0-9]*' <<<"$p")"
}

for spec in $CELLS; do
    IFS=: read -r cell size rate flags shaper <<<"$spec"
    shape "$shaper" || exit 1
    for block in $(seq 1 "$BLOCKS"); do
        for arm in P H0 H1 H1 H0 P; do
            run "$cell" "$arm" "$block" "$size" "$rate" "$flags"
        done
    done
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"

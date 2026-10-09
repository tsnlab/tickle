#!/usr/bin/env bash
# large_sample_pc.sh - large-message stage 2, step A (docs/DESIGN.md section 8): PC evidence for the socket path.
#
# A 1 MB and a 4 MB sample stream between two private network namespaces on this PC, joined by a veth pair, RELIABLE
# KEEP_LAST (ROS's default depth 10) and KEEP_ALL, at 0% and 5% netem loss on the publisher's egress, and BEST_EFFORT
# at 0% - large_sample_bench.c at both ends, built against this tree with rmw's datagram (tt_MAX_BUFFER_LENGTH 65507)
# and the shared-memory transport compiled out (-Dtt_SEGMENT_ENABLED=0): the namespaces share /dev/shm, so with it the
# two would be same-host peers and this would be step B's path, not A's.
#
# NOT a rig figure and never to be tabled as one: one PC, a veth pair, no wire. It answers "does it deliver, whole,
# in order, under loss" and gives the order of the latency and CPU figures. The rig's I1/I4/I1L cells are what DESIGN.md
# section 8's L4 reads (~/rig_queue_largemsg_A.sh).
#
# HOW TO READ IT, written before the first run (2026-10-09):
#   RELIABLE at 0% and 5%: delivered = published, torn = 0, out_of_order = 0 - every sample, whole, in order. Anything
#     less is a correctness failure of stage 2, whatever the latency says.
#   KEEP_LAST at 0%: the same; at 5%, delivered may fall short of published only by samples the 10-deep history let go
#     (tail_abandoned counts samples replaced before they went) - torn must still be 0.
#   BEST_EFFORT at 0%: delivered = published on an unloaded veth; torn = 0.
#   Latency median at 0% is the copy and syscall cost of ~723 (1 MB) or ~2889 (4 MB) datagrams; at 5% p99 shows the
#     repair round trip. cpu_ms_per_mb on both sides is the figure the design's copy budget is about.
#   CONTROL: the 1 MB BEST_EFFORT 0% cell must deliver every sample; if it does not, the setup is broken and the other
#     rows say nothing.
#
# Usage: large_sample_pc.sh [REPS] [DURATION_S]      results to $OUT (default ~/largemsg_pc/results.txt)
# Needs passwordless `sudo ip` and `tc` (tc runs inside the private namespace through `ip netns exec`).
set -u
REPS=${1:-3}
DURATION=${2:-20}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc}
OUT=${OUT:-$WORK/results.txt}
NS1=lgmsg-pub-$$
NS2=lgmsg-sub-$$
BIN=$WORK/large_sample_bench

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK"
sha=$(git -C "$TREE" rev-parse --short HEAD)
dirty=$(git -C "$TREE" status --porcelain -- src include | wc -l)
cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 -I"$TREE/include" -I"$TREE/src" \
    -o "$BIN" "$HERE/large_sample_bench.c" "$TREE/src/tickle.c" "$TREE/src/encoding.c" "$TREE/src/log.c" \
    "$TREE/src/hal_linux.c" -lpthread -lm || exit 1

cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add lgmsg1 netns "$NS1" type veth peer name lgmsg2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev lgmsg1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev lgmsg2
for ns in "$NS1" "$NS2"; do
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NS1" link set lgmsg1 up
sudo -n ip -n "$NS2" link set lgmsg2 up

echo "=== large_sample_pc $(date -Is) tree=$sha dirty_core_files=$dirty reps=$REPS duration=${DURATION}s ===" | tee -a "$OUT"

# cell NAME SIZE RATE LOSS PUB_FLAGS SUB_FLAGS
cell() {
    local name=$1 size=$2 rate=$3 loss=$4 pub_flags=$5 sub_flags=$6 rep
    sudo -n ip netns exec "$NS1" tc qdisc del dev lgmsg1 root 2>/dev/null
    if [ "$loss" != 0 ]; then
        sudo -n ip netns exec "$NS1" tc qdisc add dev lgmsg1 root netem loss "${loss}%" || return 1
    fi
    for rep in $(seq 1 "$REPS"); do
        local sublog=$WORK/sub.log publog=$WORK/pub.log
        # The logs are written by this shell, as the invoking user, and not by the sudo'd process: intended.
        # shellcheck disable=SC2024,SC2086
        sudo -n ip netns exec "$NS2" "$BIN" sub -s "$size" -d $((DURATION + 8)) $sub_flags > "$sublog" 2>&1 &
        local sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024,SC2086
        sudo -n ip netns exec "$NS1" "$BIN" pub -s "$size" -r "$rate" -d "$DURATION" $pub_flags > "$publog" 2>&1
        wait "$sub_pid"
        local p s
        p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
        s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
        if [ -z "$p" ] || [ -z "$s" ]; then
            echo "$name rep$rep VOID: no RESULT line (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
            continue
        fi
        echo "$name loss=${loss}% rep$rep | $p | $s" | tee -a "$OUT"
    done
}

# CELLS="name:loss ..." runs only those, e.g. CELLS="KEEP_LAST_4MB:5".
want() { [ -z "${CELLS:-}" ] || case " $CELLS " in *" $1:$2 "*) return 0 ;; *) return 1 ;; esac; }
for size in 1048576 4194304; do
    tag=$((size / 1048576))MB
    want "BE_${tag}" 0 && cell "BE_${tag}" "$size" 30 0 "" ""
    want "KEEP_LAST_${tag}" 0 && cell "KEEP_LAST_${tag}" "$size" 30 0 "-R" "-R"
    want "KEEP_LAST_${tag}" 5 && cell "KEEP_LAST_${tag}" "$size" 30 5 "-R" "-R"
    want "KEEP_ALL_${tag}" 0 && cell "KEEP_ALL_${tag}" "$size" 30 0 "-R -A" "-R"
    want "KEEP_ALL_${tag}" 5 && cell "KEEP_ALL_${tag}" "$size" 30 5 "-R -A" "-R"
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"

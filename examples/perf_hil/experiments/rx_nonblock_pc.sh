#!/usr/bin/env bash
# rx_nonblock_pc.sh - where the subscriber's checksum is made, sockets blocking against non-blocking, on the PC: the
# rx_gap_pc.sh network (two private namespaces, a veth, tbf 1 Gbit/s on the publisher's egress, 212,992 B buffers),
# large_sample_bench.c built ONCE from TIP at both ends, the subscriber under `perf record -g` (user and kernel: it runs
# as root in its namespace). Arm A: TT_RX_NONBLOCK=0 (the sockets blocking, as before 2026-10-13); arm B: unset.
#
# HOW TO READ IT - written before the first run (2026-10-13):
#   CSUM_POLL   the share of the subscriber's cycles samples whose stack holds the checksum (csum_partial / do_csum /
#               __skb_checksum_complete) under udp_poll() (the wait's own pass, first_packet_length()).
#   CSUM_COPY   the same under the read (skb_copy_and_csum_datagram / skb_copy_and_csum_datagram_msg).
#   The change's claim - the wait's pass is gone and the checksum moved into the copy - holds when B's CSUM_POLL is 0.0%
#   (at most 0.1%: stray samples) in every run while A's is above 1%, and B's CSUM_COPY is above A's. A's CSUM_POLL at or
#   below 1% means this PC never made the pass (the claim cannot be read here: NOT READ, not a pass). Arm A is the
#   control: the same binary, only the socket mode differs.
# Usage: rx_nonblock_pc.sh [RUNS] [DURATION_S]   TIP= (commit, default HEAD), OUT= (default ~/largemsg_pc/rx_nonblock).
# Run it detached. Every bench process runs under `ulimit -v` and a timeout. The last line is "rx_nonblock_pc done rc=".
set -u
RUNS=${1:-2}
DURATION=${2:-8}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
TIP=${TIP:-HEAD}
WORK=${WORK:-$HOME/largemsg_pc/rx_nonblock}
OUT=${OUT:-$WORK/results.txt}
NS1=rxn-pub-$$
NS2=rxn-sub-$$
VMEM_KB=2097152
SIZE=${SIZE:-1048576}
RATE=${RATE:-30}
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "rx_nonblock_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK" "$TMPDIR"
sha=$(git -C "$TREE" rev-parse --short=8 "$TIP") || exit 1
rm -rf "$WORK/src"
mkdir -p "$WORK/src"
git -C "$TREE" archive "$sha" src include examples/perf_hil/experiments/large_sample_bench.c | tar -x -C "$WORK/src" || exit 1
cc -O2 -g -DNDEBUG -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$WORK/src/include" -I"$WORK/src/src" -o "$WORK/bench" \
    "$WORK/src/examples/perf_hil/experiments/large_sample_bench.c" "$WORK/src/src/tickle.c" "$WORK/src/src/encoding.c" \
    "$WORK/src/src/log.c" "$WORK/src/src/hal_linux.c" -lpthread -lm || exit 1
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add rxna netns "$NS1" type veth peer name rxnb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.87.1/24 dev rxna
sudo -n ip -n "$NS2" addr add 192.168.87.2/24 dev rxnb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set rxna up
sudo -n ip -n "$NS2" link set rxnb up
sudo -n ip netns exec "$NS1" tc qdisc add dev rxna root tbf rate 1gbit burst 64kb latency 100ms || exit 1
echo "=== rx_nonblock_pc $(date -Is) tip=$sha size=$SIZE rate=$RATE runs=$RUNS duration=${DURATION}s ===" | tee -a "$OUT"

share() { # <perf script file> <awk regex for the caller> -> percent of samples with csum under that caller
    awk -v caller="$2" '
        /^[^ \t]/ { if (n) { total++; if (hit) hits++ } n = 1; hit = 0; seen = 0; next }
        /^[ \t]*$/ { if (n) { total++; if (hit) hits++ } n = 0; next }
        { if ($0 ~ /(csum_partial|do_csum|__skb_checksum_complete)/) seen = 1; else if (seen && $0 ~ caller) hit = 1 }
        END { if (n) { total++; if (hit) hits++ } printf "%.2f", total ? 100 * hits / total : 0 }' "$1"
}
for run in $(seq 1 "$RUNS"); do
    for arm in A B; do
        env_sub=""
        [ "$arm" = A ] && env_sub="TT_RX_NONBLOCK=0"
        data=$WORK/perf_${arm}_$run.data
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 40)) env $env_sub perf record -g \
            -F 2999 -e cycles -o $data $WORK/bench sub -s $SIZE -d $((DURATION + 6)) -R -C -K 64 -b 192.168.87.255" \
            > "$WORK/sub_$arm.log" 2>&1 &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $WORK/bench pub -s $SIZE \
            -r $RATE -d $DURATION -R -F ip -M -G -b 192.168.87.255" > "$WORK/pub_$arm.log" 2>&1
        wait "$sub_pid"
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS2" bash -c "perf script -i $data 2>/dev/null" > "$WORK/script_${arm}_$run.txt"
        poll=$(share "$WORK/script_${arm}_$run.txt" "udp_poll|first_packet_length")
        copy=$(share "$WORK/script_${arm}_$run.txt" "skb_copy_and_csum_datagram")
        delivered=$(grep -m1 '^RESULT:' "$WORK/sub_$arm.log" | grep -o ' delivered=[0-9]*' | head -1)
        nb=$(grep -c 'O_NONBLOCK refused' "$WORK/sub_$arm.log")
        echo "RUN arm=$arm run=$run CSUM_POLL=$poll% CSUM_COPY=$copy%$delivered nonblock_refused=$nb" | tee -a "$OUT"
    done
done
python3 - "$OUT" <<'EOF' | tee -a "$OUT"
import re, sys
lines = open(sys.argv[1]).read().split("=== rx_nonblock_pc")[-1].splitlines()
runs = {"A": [], "B": []}
for line in lines:
    m = re.match(r"RUN arm=(\w) run=\d+ CSUM_POLL=([\d.]+)% CSUM_COPY=([\d.]+)%", line)
    if m:
        runs[m.group(1)].append((float(m.group(2)), float(m.group(3))))
if not runs["A"] or not runs["B"]:
    print("READING: NOT READ (an arm has no run)"); sys.exit(2)
if min(p for p, _ in runs["A"]) <= 1.0:
    print("READING: NOT READ - A's CSUM_POLL is at or below 1%: this PC never made the wait's pass"); sys.exit(2)
ok = max(p for p, _ in runs["B"]) <= 0.1 and min(c for _, c in runs["B"]) > max(c for _, c in runs["A"])
print("READING: " + ("SUPPORTED - the wait's checksum pass is gone, the copy makes it" if ok else "NOT SUPPORTED"))
sys.exit(0 if ok else 1)
EOF
rc=${PIPESTATUS[0]}

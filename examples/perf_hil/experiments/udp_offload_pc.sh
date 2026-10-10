#!/usr/bin/env bash
# udp_offload_pc.sh - UDP offload (hal_linux.c "UDP offload", 2026-10-10): instructions per MB on both sides of a large
# sample stream, offload on against off, on this PC. large_sample_bench.c at both ends in two private network
# namespaces joined by a veth pair, built as rmw_tickle builds core (tt_MAX_BUFFER_LENGTH 65507, large samples), the
# shared-memory transport compiled out so the namespaces stay cross-host peers.
#
# NOT a CPU-time result and never to be tabled as one: perf stat's instruction counts (user + kernel, the processes
# run as root inside the namespaces) on one PC and a veth, no wire, no NIC. It says whether the socket layer's share
# moved and by how much in instructions; the rig A/B (DESIGN.md section 8, the L4 cells) is what says what it is worth.
#
# Arms, interleaved in every rep (the order rotates): the SAME binary each time, TT_UDP_OFFLOAD set at both ends.
#   off   TT_UDP_OFFLOAD=0: every datagram sent and read one at a time - the code before the change, by switch.
#   on    offload on.
#   off2  off again: the A/A noise control. on-vs-off is read against off2-vs-off, never against a bare figure.
#   small_off / small_on (/ small_off2, its A/A)  1 KB samples, one datagram each, never a run: offload cannot apply, so its on/off delta must
#         stay within the A/A one - a control the change provably cannot touch (bar the recvmsg() in place of
#         recvfrom() on the receive side, which is what this arm measures).
# Two veth settings, as a NIC can have:
#   SEG=off (default) the veth cuts nothing: the sender's kernel segments each run in software before the device, as
#           for a NIC without UDP segmentation (the Pi 5's). The receiver sees separate datagrams; this veth does not
#           merge them (checked: gro_merged=0), so receive offload is present but idle here - the rig's NIC is what
#           merges.
#   SEG=on  runs cross the veth whole and reach the receiver merged: both offloads exercised, and an upper bound -
#           no device ever cuts them, which a real link must.
#
# HOW TO READ IT (written before the first run):
#   - treatment: in every "on" row gso_sends > 0 (pub); with SEG=on gro_merged > 0 (sub); every off row 0 for both.
#     A row that fails it is VOID, not a reading.
#   - correctness: delivered = published, torn = 0 in every row; anything else is a failure whatever the counts say.
#   - effect: median over reps of instructions/MB per side; on vs off is the effect only where it exceeds the off2 vs
#     off spread; the small arms' on/off delta must stay inside it too, or the setup moved.
#
# Usage: udp_offload_pc.sh [REPS] [DURATION_S]   results to $OUT (default ~/udp_offload_pc/results.txt)
set -u
REPS=${1:-5}
DURATION=${2:-10}
SEG=${SEG:-off}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/udp_offload_pc}
OUT=${OUT:-$WORK/results.txt}
NS1=uopc-pub-$$
NS2=uopc-sub-$$
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
cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    -I"$TREE/include" -I"$TREE/src" -o "$BIN" "$HERE/large_sample_bench.c" "$TREE/src/tickle.c" "$TREE/src/encoding.c" \
    "$TREE/src/log.c" "$TREE/src/hal_linux.c" -lpthread -lm 2>"$WORK/build.log" || {
    cat "$WORK/build.log"
    exit 1
}

cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add uopc1 netns "$NS1" type veth peer name uopc2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev uopc1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev uopc2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set uopc1 up
sudo -n ip -n "$NS2" link set uopc2 up
sudo -n ip netns exec "$NS1" ethtool -K uopc1 tx-udp-segmentation "$SEG" >/dev/null || exit 1
seg_actual=$(sudo -n ip netns exec "$NS1" ethtool -k uopc1 | sed -n 's/^tx-udp-segmentation: \([a-z]*\).*/\1/p')

echo "=== udp_offload_pc $(date -Is) tree=$sha dirty_core_files=$dirty reps=$REPS duration=${DURATION}s" \
    "veth_tx_udp_segmentation=$seg_actual kernel=$(uname -r) ===" | tee -a "$OUT"

EV=instructions:u,instructions:k
insns() { awk -F, '$3 ~ /^instructions/ { s += $1 } END { printf "%.0f", s }' "$1"; }
field() { tr ' ' '\n' <<<"$2" | sed -n "s/^$1=//p" | head -1; }

# run ARM OFFLOAD SIZE RATE: one pub/sub pair, RELIABLE KEEP_LAST, perf stat around each end.
run() {
    local arm=$1 offload=$2 size=$3 rate=$4
    # shellcheck disable=SC2024 # the logs are this shell's: intended
    sudo -n ip netns exec "$NS2" env TT_UDP_OFFLOAD="$offload" perf stat -x, -e "$EV" -o "$WORK/sub.csv" -- \
        "$BIN" sub -s "$size" -d $((DURATION + 6)) -R >"$WORK/sub.log" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" env TT_UDP_OFFLOAD="$offload" perf stat -x, -e "$EV" -o "$WORK/pub.csv" -- \
        "$BIN" pub -s "$size" -r "$rate" -d "$DURATION" -R >"$WORK/pub.log" 2>&1
    wait "$sub_pid"
    local p s
    p=$(grep -m1 '^RESULT:' "$WORK/pub.log" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$WORK/sub.log" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "$arm rep$rep VOID: no RESULT line (pub: $(tail -1 "$WORK/pub.log") / sub: $(tail -1 "$WORK/sub.log"))" |
            tee -a "$OUT"
        return
    fi
    local published delivered torn mb pub_i sub_i verdict=ok
    published=$(field published "$p")
    delivered=$(field delivered "$s")
    torn=$(field torn "$s")
    mb=$(awk -v n="$delivered" -v b="$size" 'BEGIN { printf "%.3f", n * b / 1e6 }')
    pub_i=$(insns "$WORK/pub.csv")
    sub_i=$(insns "$WORK/sub.csv")
    # Treatment and correctness, as pre-registered above.
    if [ "$delivered" != "$published" ] || [ "$torn" != 0 ]; then verdict=FAIL_delivery; fi
    local gso gro
    gso=$(field gso_sends "$p")
    gro=$(field gro_merged "$s")
    if [ "$offload" = 0 ] && { [ "$gso" != 0 ] || [ "$gro" != 0 ]; }; then verdict=VOID_treatment; fi
    if [ "$offload" = 1 ] && [ "$size" -gt 65507 ] && [ "${gso:-0}" = 0 ]; then verdict=VOID_treatment; fi
    if [ "$offload" = 1 ] && [ "$seg_actual" = on ] && [ "$size" -gt 65507 ] && [ "${gro:-0}" = 0 ]; then
        verdict=VOID_treatment
    fi
    echo "$arm rep$rep $verdict size=$size published=$published delivered=$delivered torn=$torn mb=$mb" \
        "pub_insns_per_mb=$(awk -v i="$pub_i" -v m="$mb" 'BEGIN { printf "%.0f", i / m }')" \
        "sub_insns_per_mb=$(awk -v i="$sub_i" -v m="$mb" 'BEGIN { printf "%.0f", i / m }')" \
        "gso_sends=$gso gso_datagrams=$(field gso_datagrams "$p") gro_reads=$(field gro_reads "$s") gro_merged=$gro" \
        "gro_copied=$(field gro_copied "$s") gro_off8=$(field gro_off8 "$s")" \
        "latency_median_us=$(field latency_median_us "$s") latency_p99_us=$(field latency_p99_us "$s")" | tee -a "$OUT"
}

# ARMS="..." runs a subset (e.g. "small_off small_on small_off2", the 1 KB control with its own A/A).
read -r -a ARMS <<<"${ARMS:-off on off2 small_off small_on}"
for rep in $(seq 1 "$REPS"); do
    for k in $(seq 0 $((${#ARMS[@]} - 1))); do
        arm=${ARMS[$(((k + rep) % ${#ARMS[@]}))]}
        case $arm in
        off | off2) run "$arm" 0 1048576 30 ;;
        on) run "$arm" 1 1048576 30 ;;
        small_off | small_off2) run "$arm" 0 1024 1000 ;;
        small_on) run "$arm" 1 1024 1000 ;;
        esac
    done
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"

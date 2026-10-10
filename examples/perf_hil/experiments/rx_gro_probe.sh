#!/usr/bin/env bash
# rx_gro_probe.sh - instructions per MB of a 1 MB / 30 Hz stream of 1472-byte datagrams, by receive method, between
# two private namespaces on a veth pair (rx_gro_probe.c; no TickLE in it). Sizes the candidates of
# largemsg_cpu_profile.sh's ranking before any of them is written into core.
#
# ARMS, interleaved within each rep (veth GRO = `ethtool -K <rx veth> gro on`, which gives veth a NAPI and so the
# kernel's GRO stage):
#   A      recvfrom, veth GRO off           - today's rmw build (tt_RX_BATCH 1)
#   A_gro  recvfrom, veth GRO on            - CONTROL: GRO enabled on the device but not on the socket. UDP GRO
#                                             coalesces only for a socket that asked for it, so this must match A
#   M      recvmmsg 32 x 1472-byte slots    - batching the syscall only
#   G      UDP_GRO socket, veth GRO on      - the kernel hands over a train of same-size datagrams in one skb
#   S      sender UDP_SEGMENT (GSO), receiver recvfrom, veth GRO off - the send-side lever. On veth the sender's
#                                             context also runs the receive softirq, so read S's total only.
#   SG     sender GSO, receiver UDP_GRO     - veth hands the GSO skb to a GRO socket whole: the upper bound of a
#                                             NIC whose GRO merges every train (ARMS=... selects arms)
# Finding of the first run (2026-10-10 14:31): G merged nothing on veth - each sendmmsg datagram's softirq runs at
# its own bh-enable, so veth's NAPI polls one packet at a time and GRO has nothing to merge. veth cannot show G; SG
# is its ceiling and the rig NIC (interrupt-coalesced NAPI) decides where between A and SG it lands.
#
# HOW TO READ IT (written before the run):
#   Per arm: instructions (user + kernel) per MB delivered, for the receiver and for the sender separately and summed.
#   On veth the receive softirq (ip_rcv .. udp_queue_rcv_skb) runs in the SENDER's context, so the receiver column is
#   the syscall side only and the sum is the honest whole.
#   VOID rep: any arm delivers < 99% of what was sent, or A_gro's receiver instructions/MB differ from A's by more than
#   the spread of A across reps plus 5% (the control: then the device toggle itself changed the path).
#   G is worth building if its receiver per_call > 4 (coalescing happens) and receiver+sender instructions/MB fall by
#   more than M's do; if G's per_call stays ~1 the rig NIC question (GRO on its driver) decides nothing here.
#   No CPU time is read from this PC (shared with other sessions): instructions only, and per_call.
#
# RESULT 2026-10-10 14:35 (base 237b36e2 era, kernel 7.0.0-34, ARMS="A S SG A2 M", 3 reps, 10 s, 214k datagrams each,
# every arm 100% delivered). Instructions per MB, receiver + sender (receiver alone):
#   A 8.57 M (1.66) | A2 control 8.56 M (1.64) | M recvmmsg 8.54 M (1.65) | S GSO send 3.26 M (1.43) | SG 0.58 M (0.25)
#   Reading: the control holds (A2 = A within 1%). recvmmsg saves nothing measurable - the per-datagram kernel cost is
#   the skb (alloc, IP/UDP receive, dequeue, free, wake-up), not the syscall. Per-skb levers are the ones that move:
#   S -62% (veth-flattered: the GSO skb crosses veth unsegmented), SG -93% (ceiling).
#
# Usage: rx_gro_probe.sh [REPS] [SECONDS]     results to $OUT (default ~/largemsg_profile/rx_gro_probe.txt)
set -u
REPS=${1:-3}
SECS=${2:-10}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HOME/largemsg_profile}
OUT=${OUT:-$WORK/rx_gro_probe.txt}
BIN=$WORK/rx_gro_probe
NS1=rxgro-tx-$$
NS2=rxgro-rx-$$
PORT=7411

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK"
cc -O2 -Wall -o "$BIN" "$HERE/rx_gro_probe.c" || exit 1
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add rxg1 netns "$NS1" type veth peer name rxg2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev rxg1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev rxg2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set rxg1 up
sudo -n ip -n "$NS2" link set rxg2 up
echo "=== rx_gro_probe $(date -Is) reps=$REPS secs=$SECS kernel=$(uname -r) ===" | tee -a "$OUT"

EV=instructions:u,instructions:k,cycles:u,cycles:k
arm() { # NAME RXMODE VETH_GRO(on|off) SENDMODE
    local name=$1 mode=$2 gro=$3 smode=$4
    sudo -n ip netns exec "$NS2" ethtool -K rxg2 gro "$gro" >/dev/null 2>&1
    local actual
    actual=$(sudo -n ip netns exec "$NS2" ethtool -k rxg2 | grep -m1 '^generic-receive-offload' | awk '{print $2}')
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" perf stat -x, -e "$EV" -o "$WORK/rx.csv" -- "$BIN" recv $PORT $((SECS + 2)) "$mode" \
        > "$WORK/rx.log" 2>&1 &
    local rx=$!
    sleep 0.5
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" perf stat -x, -e "$EV" -o "$WORK/tx.csv" -- "$BIN" send 192.168.10.2 $PORT "$SECS" $smode \
        > "$WORK/tx.log" 2>&1
    wait "$rx"
    local rxu rxk txu txk rxc txc
    rxc=$(awk -F, '$3~/^cycles/{s+=$1} END{print s}' "$WORK/rx.csv")
    txc=$(awk -F, '$3~/^cycles/{s+=$1} END{print s}' "$WORK/tx.csv")
    rxu=$(awk -F, '$3=="instructions:u"{print $1}' "$WORK/rx.csv")
    rxk=$(awk -F, '$3=="instructions:k"{print $1}' "$WORK/rx.csv")
    txu=$(awk -F, '$3=="instructions:u"{print $1}' "$WORK/tx.csv")
    txk=$(awk -F, '$3=="instructions:k"{print $1}' "$WORK/tx.csv")
    echo "$name rep$rep veth_gro=$actual | $(grep -m1 RESULT "$WORK/tx.log") | $(grep -m1 RESULT "$WORK/rx.log") | rx_u=$rxu rx_k=$rxk tx_u=$txu tx_k=$txk rx_cyc=$rxc tx_cyc=$txc" |
        tee -a "$OUT"
}
ARMS=${ARMS:-A M G A_gro S SG}
for rep in $(seq 1 "$REPS"); do
    for a in $ARMS; do
        case $a in
        A) arm A recvfrom off "" ;;
        A2) arm A2 recvfrom off "" ;;
        M) arm M mmsg off "" ;;
        G) arm G gro on "" ;;
        A_gro) arm A_gro recvfrom on "" ;;
        S) arm S recvfrom off gso ;;
        SG) arm SG gro off gso ;;
        esac
    done
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"

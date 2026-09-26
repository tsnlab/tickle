#!/usr/bin/env bash
# Why does per-datagram recovery still deliver duplicated fragments? On the rig (bceddf2a, DATAFRAG_PLAN
# 14) frag_duplicate was ~5.7% of samples at p4 (2 fragments) and ~0.2% at 4 fragments; veth at ~0 RTT
# gave ~0.3% at p4. frag_duplicate counts a FRAG datagram arriving whose seq_no the subscriber already
# holds, so it is a datagram the writer really sent twice.
#
# HYPOTHESIS (Dev, 2026-09-26): the reader's acknack_retry timer is per writer proxy and re-names every
# seq_no still missing on each tick, including a gap whose own NACK went out less than one repair latency
# ago; the writer has no in-flight suppression, so it resends again and the first repair arrives too.
# Then duplicates per lost datagram ~ repair latency / retry interval, capped near the number of ticks
# that fit in one repair latency.
#
# ARMS - p4 reliable_throughput builds with RSTATS, 5% loss on the data direction, netem delay D each way:
#   dyn       dynamic retry interval (the default)        D = 0 and 250us
#   fixed1ms  tt_RELIABLE_RETRY_INTERVAL fixed at 1 ms     D = 0 and 250us
#   dyn800    dynamic, 800-byte datagrams (4 fragments)    D = 0 and 250us
#
# HOW TO READ IT, written before running. dup_per_loss = server frag_duplicate / server missing_opened.
#   CONTROL: every row drained=acked and lost=0, or the row says nothing. At D=0 every arm is expected
#   low (veth baseline ~0.3% of samples, i.e. dup_per_loss well under 0.1).
#   fixed1ms, D=250us (RTT ~0.5 ms, interval 1 ms): H1 predicts dup_per_loss ~0.5 (0.3-0.7). If it stays
#   near the D=0 value, H1 is wrong - a fixed interval shorter than nothing cannot be what duplicates.
#   dyn, D=250us: H1 predicts dup_per_loss ~ srtt / (srtt + 4 rttvar) from the server's own
#   recovery_srtt_ns/recovery_rttvar_ns - high where netem's constant delay leaves rttvar small.
#   Cross-check: client retransmitted - server missing_opened should be about duplicates plus lost
#   repairs (~5% of retransmitted). If duplicates far exceed extra resends, something other than the
#   writer resending is duplicating datagrams.
#   dyn800 vs dyn at D=250us: whether the rig's 2-vs-4 fragment asymmetry shows here at all, and if so
#   whether it follows the arms' own srtt/rttvar. veth is not the rig: read direction and ratio.
#
# SEEN, 2 reps at 47189580 (dup_per_loss; every row drained=acked, lost=0):
#   dyn       D=0 0.027 0.030   D=250us 0.223 0.214
#   fixed1ms  D=0 0.011 0.010   D=250us 0.519 0.509   <- H1's intervention, inside its predicted 0.3-0.7
#   dyn800    D=0 0.004 0.004   D=250us 0.207 0.206
#   Extra resends match duplicates + lost repairs (fixed1ms 250us: 16.7k vs 14.4k + ~2.2k), so every
#   duplicate is a second resend by the writer. What failed: the dynamic arm's srtt/(srtt + 4 rttvar)
#   predicted 0.34 and 0.58 and got 0.22 both times - the mechanism holds, that quantitative model does
#   not. And the rig's 2-vs-4 fragment asymmetry does not appear here (0.22 vs 0.21).
#
# Usage: veth_frag_duplicate.sh <reps>     Results: $OUT (default /tmp/veth_frag_duplicate.txt)
# Needs passwordless `sudo ip`; everything is shaped inside two private namespaces.
set -u
REPS=${1:?usage: veth_frag_duplicate.sh <reps>}
OUT=${OUT:-/tmp/veth_frag_duplicate.txt}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF="$(cd "$HERE/.." && pwd)"
ARMS_DIR=${ARMS_DIR:-/tmp/veth_frag_duplicate_arms}
NS1=fragdup-ns1
NS2=fragdup-ns2
CLIENT_LOG=/tmp/veth_frag_duplicate_client.log
SERVER_LOG=/tmp/veth_frag_duplicate_server.log

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

build_arm() { # <name> <env...>
    local name=$1
    shift
    env TICKLE_RELIABLE_STATS=1 "$@" "$PERF/tickle/build.sh" reliable_throughput p4 >"$ARMS_DIR/$name.build.log" 2>&1 ||
        { echo "build $name failed - $ARMS_DIR/$name.build.log" >>"$OUT"; exit 1; }
    mkdir -p "$ARMS_DIR/$name"
    cp "$PERF/tickle/reliable_throughput_p4/client" "$PERF/tickle/reliable_throughput_p4/server" "$ARMS_DIR/$name/"
}

run_one() { # <arm> <delay_us>
    local arm=$1 delay=$2
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add fragdup1 type veth peer name fragdup2 || exit 1
    sudo -n ip link set fragdup1 netns "$NS1"
    sudo -n ip link set fragdup2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev fragdup1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev fragdup2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set fragdup1 up
    sudo -n ip -n "$NS2" link set fragdup2 up
    local d=""
    [ "$delay" != 0 ] && d="delay ${delay}us"
    # shellcheck disable=SC2086 # $d is deliberately word-split into netem's arguments
    sudo -n ip netns exec "$NS1" tc qdisc add dev fragdup1 root netem $d loss 5% || exit 1
    if [ -n "$d" ]; then
        # shellcheck disable=SC2086
        sudo -n ip netns exec "$NS2" tc qdisc add dev fragdup2 root netem $d || exit 1
    fi
    # shellcheck disable=SC2024 # the logs are this shell's, on purpose
    sudo -n ip netns exec "$NS2" "$ARMS_DIR/$arm/server" -d 10 >"$SERVER_LOG" 2>&1 &
    local server_pid=$!
    sleep 2
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" "$ARMS_DIR/$arm/client" -d 4 -Q >"$CLIENT_LOG" 2>&1
    wait "$server_pid"
    local srv cli dup missing retx
    srv=$(grep -h "^RESULT" "$SERVER_LOG" | tr ' ' '\n' |
        grep -E "^(recv|lost|frag_duplicate|recovery_srtt_ns|recovery_rttvar_ns|retry_interval_cfg_ns)=" | paste -sd' ')
    cli=$(grep -h "^RESULT" "$CLIENT_LOG" | tr ' ' '\n' | grep -E "^(sent|drained|retransmitted)=" | paste -sd' ')
    missing=$(grep -h "RSTATS: role=server sub" "$SERVER_LOG" | grep -o "missing_opened=[0-9]*" | cut -d= -f2)
    dup=$(echo "$srv" | grep -o "frag_duplicate=[0-9]*" | cut -d= -f2)
    retx=$(echo "$cli" | grep -o "retransmitted=[0-9]*" | cut -d= -f2)
    local ratio=na
    if [ -n "$missing" ] && [ "$missing" -gt 0 ] && [ -n "$dup" ]; then
        ratio=$(echo "scale=3; $dup / $missing" | bc)
    fi
    echo "arm=$arm delay_us=$delay $cli $srv missing_opened=${missing:-na} dup_per_loss=$ratio extra_resends=$(( ${retx:-0} - ${missing:-0} ))" >>"$OUT"
}

mkdir -p "$ARMS_DIR"
: >"$OUT"
build_arm dyn
build_arm fixed1ms TICKLE_DYNAMIC_RETRY=0
build_arm dyn800 TICKLE_DATAGRAM_BYTES=800
for rep in $(seq 1 "$REPS"); do
    for arm in dyn fixed1ms dyn800; do
        for delay in 0 250; do
            echo "rep=$rep" >>"$OUT"
            run_one "$arm" "$delay"
        done
    done
done
echo "DONE" >>"$OUT"

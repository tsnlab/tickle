#!/usr/bin/env bash
# c6's condition - P4, RELIABLE + KEEP_ALL (-Q), 5% loss on the client's egress - reproduced off the
# rig, on a veth pair between two private network namespaces on this PC. For a functional check of a
# p4 build in minutes, before it goes to the rig; not a measurement to table.
#
# The harness's wire_* fields read eth0, which does not exist in the namespaces, so they print 0 here.
# What stands in for them is the client veth's own tx counters, which count every datagram the client
# handed to the link - netem drops after that point, as it does on the rig's eth0.
#
# HOW TO READ IT, written before the first run (2026-09-26):
#   drained=acked and server lost=0         -> recovery completed inside the harness's drain window,
#                                              criterion 1 of DATAFRAG_PLAN.md section 8.
#   tx_packets_per_sample near 2 at loss 5  -> criterion 2: no retransmission storm.
#   CONTROL: loss 0 must give ~2.0 packets a sample and drained=acked for BOTH paths. If it does not,
#   the setup is broken and the lossy rows say nothing.
# Seen on the first runs: ipfrag at 5% gave 94.7 and then 561 packets a sample, drained=timeout both
# times - c6's failure on the rig, including its run-to-run spread - and frag gave 2.10, drained=acked.
#
# Usage: veth_c6_check.sh <p4 scenario dir> <loss percent>   (build it first: build.sh reliable_throughput p4)
# Needs passwordless `sudo ip` (as platform/linux/test.sh does); tc runs inside the private namespace
# through `ip netns exec`, so nothing outside it is shaped.
set -u
DIR=${1:?usage: veth_c6_check.sh <p4 scenario dir> <loss percent>}
LOSS=${2:?usage: veth_c6_check.sh <p4 scenario dir> <loss percent>}
NS1=fragchk-ns1
NS2=fragchk-ns2
CLIENT_LOG=/tmp/veth_c6_client.log
SERVER_LOG=/tmp/veth_c6_server.log

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
cleanup

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add fragchk1 type veth peer name fragchk2 || exit 1
sudo -n ip link set fragchk1 netns "$NS1"
sudo -n ip link set fragchk2 netns "$NS2"
# The addresses the harness hard-codes its broadcast for (192.168.10.255), as on the rig.
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev fragchk1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev fragchk2
for ns in "$NS1" "$NS2"; do
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NS1" link set fragchk1 up
sudo -n ip -n "$NS2" link set fragchk2 up
if [ "$LOSS" != 0 ]; then
    sudo -n ip netns exec "$NS1" tc qdisc add dev fragchk1 root netem loss "${LOSS}%" || exit 1
fi
sudo -n ip netns exec "$NS1" tc qdisc show dev fragchk1

# The logs are written by this shell, as the invoking user, and not by the sudo'd process: intended.
# shellcheck disable=SC2024
sudo -n ip netns exec "$NS2" "$DIR/server" -d 14 >"$SERVER_LOG" 2>&1 &
server_pid=$!
sleep 2
# shellcheck disable=SC2024
sudo -n ip netns exec "$NS1" "$DIR/client" -d 5 -Q >"$CLIENT_LOG" 2>&1
wait "$server_pid"

stat_of() { sudo -n ip netns exec "$NS1" cat "/sys/class/net/fragchk1/statistics/$1"; }
tx_packets=$(stat_of tx_packets)
tx_bytes=$(stat_of tx_bytes)
sent=$(grep -ho "role=client sent=[0-9]*" "$CLIENT_LOG" | grep -o "[0-9]*$")
if [ -z "$sent" ] || [ "$sent" = 0 ]; then
    echo "no client RESULT line - see $CLIENT_LOG and $SERVER_LOG" >&2
    exit 1
fi
echo "loss=${LOSS}% samples=$sent tx_packets_per_sample=$(echo "scale=3; $tx_packets / $sent" | bc)" \
    "tx_bytes_per_sample=$(echo "scale=1; $tx_bytes / $sent" | bc)"
grep -h "RESULT" "$CLIENT_LOG" "$SERVER_LOG" | tr ' ' '\n' |
    grep -E "^(role|sent|recv|lost|drained|write_fail|send_mbps|core_build|sample_path)=" | paste -sd' '

#!/usr/bin/env bash
# Does /proc/net/dev tx count packets that netem DROPS?
#
# Pre-registered reading, written before running:
#   arm "no netem"  : tx_packets must rise by ~100. If it does not, the instrument cannot see
#                     packets at all and NEITHER arm says anything.
#   arm "loss 100%" : tx rises by ~100  -> the counter includes netem's drops, so every N1 wire
#                                          figure in the campaign is inflated and c6's 53.6x is an
#                                          artefact.
#                     tx rises by ~0    -> the counter reflects the wire, and c6's amplification is
#                                          a real TickLE behaviour.
set -u
K=$HOME/.ssh/tickle_ci_ed25519; H=10.1.1.214; PEER=192.168.10.2
sh() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$H" "$@"; }
ctr() { sh "awk -F'[: ]+' '/eth0/{print \$11, \$12}' /proc/net/dev"; }
run_arm() {
    local label="$1" setup="$2"
    sh "$setup" >/dev/null 2>&1
    read -r b0 p0 <<<"$(ctr)"
    sh "ping -c 100 -i 0.01 -s 1000 -W 1 $PEER" >/dev/null 2>&1
    read -r b1 p1 <<<"$(ctr)"
    echo "$label: tx_packets +$((p1-p0))  tx_bytes +$((b1-b0))  (100 pings of 1000 B were attempted)"
}
run_arm "no netem  " "sudo -n tc qdisc del dev eth0 root"
run_arm "loss 100% " "sudo -n tc qdisc replace dev eth0 root netem loss 100%"
sh "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1
echo "restored: $(sh 'tc qdisc show dev eth0')"

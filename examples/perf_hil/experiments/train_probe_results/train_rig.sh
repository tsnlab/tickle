#!/usr/bin/env bash
# shellcheck disable=SC2086,SC2029,SC2016  # ssh-side expansion and word lists are deliberate
# Receive-path probe on the Pis' netem rig (netns + GRE tap; the real eth0/wlan0 are never touched). Usage: train_rig.sh up | down | rate <kbit> | slot <kbit> | build
#   up/down: the namespaces wtc_sta (rpi-1, sender) and wtc_srv (rpi-0, receiver) with the tap and a base netem (5 ms one way, no loss), via ~/wtc_scripts/wtc_topo.sh
#   rate:    the shaped link rate on both taps (netem rate, queue 1000 packets, 5 ms, no loss)
#   slot:    a slotted link: netem slot 4 ms with a byte budget per slot that gives the rate (kbit/s x 500 / 1000 bytes... = rate x 4 ms / 8)
#   build:   ships the checked-out tree to both Pis and builds /tmp/train_probe on each
set -eu
KEY=${PI_KEY:-$HOME/.ssh/tickle_ci_ed25519}; SSHO="-i $KEY -o StrictHostKeyChecking=no -o BatchMode=yes"
CLI=ci@10.1.1.214; SRV=ci@10.1.1.213; T='$HOME/wtc_scripts/wtc_topo.sh'
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); REPO=$(cd "$HERE/../../../.." && pwd)
tcset() { # NETEM-args: delete and add (a `change` keeps an earlier `slot` setting)
    ssh -n $SSHO $CLI "sudo -n ip netns exec wtc_sta tc qdisc del dev wtcgc root; sudo -n ip netns exec wtc_sta tc qdisc add dev wtcgc root netem $1"
    ssh -n $SSHO $SRV "sudo -n ip netns exec wtc_srv tc qdisc del dev wtcgs root; sudo -n ip netns exec wtc_srv tc qdisc add dev wtcgs root netem $1"
}
case ${1:-} in
up)
    ssh -n $SSHO $SRV "$T modules load; $T srv down; NETEM='limit 1000 delay 5ms rate 30mbit' LOCAL_IP=192.168.10.2 REMOTE_IP=192.168.10.3 $T srv up"
    ssh -n $SSHO $CLI "$T modules load; $T cli down; NETEM='limit 1000 delay 5ms rate 30mbit' LOCAL_IP=192.168.10.3 REMOTE_IP=192.168.10.2 $T cli up"
    ssh -n $SSHO $CLI "sudo -n ip netns exec wtc_sta ping -c 2 -W 2 10.90.0.3 | tail -2" ;;
down)
    ssh -n $SSHO $CLI "$T cli down; $T modules unload"; ssh -n $SSHO $SRV "$T srv down; $T modules unload" ;;
rate) tcset "limit 1000 delay 5ms rate ${2}kbit" ;;
slot) tcset "limit 1000 delay 5ms slot 4ms 4ms bytes $((${2} / 2))" ;; # rate x 4 ms / 8 = rate_kbit / 2 bytes per slot
build)
    for h in $CLI $SRV; do
        git -C "$REPO" archive HEAD | ssh $SSHO $h 'rm -rf tpr_src && mkdir tpr_src && tar -x -C tpr_src'
        ssh -n $SSHO $h 'cd tpr_src && examples/perf_hil/experiments/train_probe_rig.sh build /tmp/train_probe 2>&1 | tail -1'
    done ;;
*) echo "usage: $0 up|down|rate K|slot K|build"; exit 2 ;;
esac

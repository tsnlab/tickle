#!/usr/bin/env bash
# rx_uring_prof_large_pc.sh - where the large-sample SUBSCRIBER's instructions go, per arm: rx_uring_pc.sh's LARGE network
# (two private namespaces, a veth, the publisher's egress shaped to 1 Gbit/s by tbf, 212,992-byte socket buffers) and
# its bench binary, the subscriber under `perf record -e instructions:u,instructions:k` (no call graphs), reported by
# symbol, with the MB it delivered beside it (units.txt) for rx_uring_prof_diff.py. For naming the cost behind a
# rx_uring_pc.sh LARGE verdict - not a figure of its own.
# Usage: rx_uring_prof_large_pc.sh <bench binary> <out dir> <size> <rate> <qflag|-> [ENV=V ...]   (the env goes to the
# subscriber; the publisher is the same binary without it). Run it detached; every process runs under ulimit -v and a
# timeout. The last line is always "rx_uring_prof_large_pc done rc=<n>".
set -u
BIN=${1:?bench binary}
OUTDIR=${2:?output directory}
SIZE=${3:?bytes}
RATE=${4:?Hz}
QFLAG=${5:?-R or -}
shift 5
ENVS="$*"
[ "$QFLAG" = - ] && QFLAG=""
DURATION=${DURATION:-12}
VMEM_KB=${VMEM_KB:-2097152}
NS1=rupl-pub-$$
NS2=rupl-sub-$$
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "rx_uring_prof_large_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT
mkdir -p "$OUTDIR"
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add rupla netns "$NS1" type veth peer name ruplb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.89.1/24 dev rupla
sudo -n ip -n "$NS2" addr add 192.168.89.2/24 dev ruplb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set rupla up
sudo -n ip -n "$NS2" link set ruplb up
sudo -n ip netns exec "$NS1" tc qdisc add dev rupla root tbf rate 1gbit burst 64kb latency 100ms || exit 1
# shellcheck disable=SC2024,SC2086
sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 40)) perf record -q -c ${PERIOD:-20000} \
    -e instructions:u,instructions:k -o $OUTDIR/perf.data env $ENVS $BIN sub -s $SIZE -d $((DURATION + 8)) $QFLAG \
    -K 64 -b 192.168.89.255" >"$OUTDIR/sub.log" 2>&1 &
sub_pid=$!
sleep 1
# shellcheck disable=SC2024,SC2086
sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $BIN pub -s $SIZE -r $RATE \
    -d $DURATION $QFLAG -F auto -M -G -b 192.168.89.255" >"$OUTDIR/pub.log" 2>&1
wait "$sub_pid"
# shellcheck disable=SC2024
sudo -n ip netns exec "$NS2" perf report -i "$OUTDIR/perf.data" --stdio --sort sym --percent-limit 0.2 >"$OUTDIR/report.txt" 2>&1
delivered=$(grep -m1 '^RESULT:' "$OUTDIR/sub.log" | grep -o 'delivered=[0-9]*' | cut -d= -f2)
[ -n "$delivered" ] || { echo "no RESULT from the subscriber"; exit 1; }
echo "$delivered * $SIZE / 1000000" | bc -l >"$OUTDIR/units.txt"
echo "delivered=$delivered MB=$(cat "$OUTDIR/units.txt")"
rc=0

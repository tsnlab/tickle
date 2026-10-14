#!/usr/bin/env bash
# rx_uring_prof_pc.sh - where the core ping-pong client's instructions go, per arm: lat_gap_pc.sh's network (two private
# namespaces over a veth) and its binaries, the CLIENT under `perf record -e instructions:u,instructions:k` (no call
# graphs), reported by symbol. For naming the cost behind a rx_uring_pc.sh PING verdict - not a figure of its own.
# Usage: rx_uring_prof_pc.sh <reliable_latency dir> <out dir> [ENV=V ...]   (the env goes to both ends). Run it
# detached; every process runs under ulimit -v and a timeout. The last line is always "rx_uring_prof_pc done rc=<n>".
set -u
DIR=${1:?reliable_latency directory}
OUTDIR=${2:?output directory}
shift 2
ENVS="$*"
SECS=${SECS:-5}
VMEM_KB=${VMEM_KB:-2097152}
NS1=rup-cli-$$
NS2=rup-srv-$$
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "rx_uring_prof_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT
mkdir -p "$OUTDIR"
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add rupa netns "$NS1" type veth peer name rupb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev rupa
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev rupb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set rupa up
sudo -n ip -n "$NS2" link set rupb up
# shellcheck disable=SC2024,SC2086
sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; cd $DIR && exec timeout -s INT $((SECS + 9)) env $ENVS \
    ./server -d $((SECS + 7)) -W 100 -C 100 -I 0.001" >"$OUTDIR/srv.log" 2>&1 &
spid=$!
sleep 1
# shellcheck disable=SC2024,SC2086
sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; cd $DIR && exec timeout -s INT $((SECS + 20)) perf record -q -c ${PERIOD:-10000} \
    -e instructions:u,instructions:k -o $OUTDIR/perf.data env $ENVS ./client -i 0.001 -d $SECS -W 100 -C 100 -I 0.001 \
    -T 3" >"$OUTDIR/cli.log" 2>&1
wait "$spid"
# shellcheck disable=SC2024
sudo -n ip netns exec "$NS1" perf report -i "$OUTDIR/perf.data" --stdio --sort sym --percent-limit 0.3 >"$OUTDIR/report.txt" 2>&1
grep -m1 '^RESULT' "$OUTDIR/cli.log" | grep -oE ' (sent|recv)=[0-9]*' | tr '\n' ' '
echo
rc=0

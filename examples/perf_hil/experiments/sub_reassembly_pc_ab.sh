#!/usr/bin/env bash
# sub_reassembly_pc_ab.sh - per-Subscriber reassembly slots on the PC (docs/DESIGN.md section 8, "Reassembly slots";
# the user, 2026-10-27): does a BEST_EFFORT stream of fragmented samples beside 4-8 RELIABLE ones whose Subscribers
# have no reorder buffer get more of its samples when every Subscriber owns a slot - and does nothing else get worse?
# Registered before its first run; the rules are sub_reassembly_pc_ab.py's docstring, in its code.
#
# ARMS, built here from git (each tree's src/ and include/, this harness's sub_reassembly_node.c for both):
#   M  the parent (M_SHA, default origin/main)
#   A  M's binaries run again: A - M is this session's noise, THE FLOOR
#   F  the change (F_SHA, default HEAD)
# Arm order rotated per repetition and cell, so drift lands on every arm. The build is rmw_tickle's sample size
# (-Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1) without the same-host segment (both namespaces are this host:
# the segment would carry the samples past the veth and its loss).
#
# TRAFFIC (sub_reassembly_node.c). Publisher and subscriber in two private network namespaces joined by a veth pair
# (never the default namespace: it reaches the rig), netem "delay 1ms loss L%" on the publisher's egress. N streams of
# SIZE-byte samples at RATE Hz, RELIABLE KEEP_LAST 10, read by Subscribers with no reorder buffer; one BEST_EFFORT
# stream of the same; the control, 64-byte RELIABLE samples at 100 Hz read through a reorder buffer. Cells "n<N>_l<L>"
# for N in STREAMS (default "4 8") and L in LOSSES (default "0 1 5"), DUR seconds each.
#
# Usage: [M_SHA=..] [F_SHA=..] [REPS=10] [DUR=10] [SIZE=30000] [RATE=30] [STREAMS="4 8"] [LOSSES="0 1 5"]
#        [OUT=/home/semih/sub_reasm_results/ab_<time>] sub_reassembly_pc_ab.sh
# Needs passwordless `sudo -n ip` (tc runs inside the namespace through `ip netns exec`). Long (~55 min at the
# defaults): launch detached and wait for the line it always ends with, "=== sub_reassembly_pc_ab ended".
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
M_SHA=${M_SHA:-$(git -C "$REPO" rev-parse --short origin/main)}
F_SHA=${F_SHA:-$(git -C "$REPO" rev-parse --short HEAD)}
REPS=${REPS:-10}
DUR=${DUR:-10}
SIZE=${SIZE:-30000}
RATE=${RATE:-30}
STREAMS=${STREAMS:-4 8}
LOSSES=${LOSSES:-0 1 5}
OUTD=${OUT:-/home/semih/sub_reasm_results/ab_$(date +%Y%m%d-%H%M%S)}
NS1=sra-pub-$$
NS2=sra-sub-$$
T0=$(date +%s)

on_exit() {
    local rc=$?
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "=== sub_reassembly_pc_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s, out $OUTD ==="
}
trap on_exit EXIT
trap 'exit 130' INT TERM

if [ -e "$OUTD/runs.txt" ]; then
    echo "REFUSED: $OUTD/runs.txt exists"
    exit 2
fi
mkdir -p "$OUTD/runs" "$OUTD/build" || exit 1
RUNS="$OUTD/runs.txt"
: >"$RUNS"
{
    echo "=== sub_reassembly_pc_ab $(date -Is) M=$M_SHA F=$F_SHA harness=$(git -C "$REPO" rev-parse --short HEAD)" \
        "(harness dirty: $(git -C "$REPO" status --porcelain -- "$HERE/sub_reassembly_node.c" "$HERE/sub_reassembly_pc_ab.sh" "$HERE/sub_reassembly_pc_ab.py" | wc -l))" \
        "reps=$REPS dur=$DUR size=$SIZE rate=$RATE streams=($STREAMS) losses=($LOSSES) ==="
    echo "load at start: $(cut -d' ' -f1-3 /proc/loadavg)"
} | tee "$OUTD/header.txt"

# One tree per SHA, from git, and the node built against it.
for arm in M F; do
    sha=$M_SHA
    [ $arm = F ] && sha=$F_SHA
    tree="$OUTD/build/tree_$arm"
    mkdir -p "$tree" && git -C "$REPO" archive "$sha" src include | tar -x -C "$tree" || exit 1
    nice -n 10 cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -I"$tree/include" -I"$tree/src" -o "$OUTD/build/node_$arm" "$HERE/sub_reassembly_node.c" "$tree/src/tickle.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" "$tree/src/hal_linux.c" -lpthread -lm >"$OUTD/build/build_$arm.log" 2>&1 || {
        echo "FATAL: build of $arm ($sha) failed - $OUTD/build/build_$arm.log" | tee -a "$OUTD/header.txt"
        exit 1
    }
    echo "arm $arm = $sha node sha256 $(sha256sum "$OUTD/build/node_$arm" | cut -c1-16)" | tee -a "$OUTD/header.txt"
done
echo "arm A = arm M's binary, run again" | tee -a "$OUTD/header.txt"

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add sra1 netns "$NS1" type veth peer name sra2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev sra1 || exit 1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev sra2 || exit 1
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set sra1 up && sudo -n ip -n "$NS2" link set sra2 up || exit 1

set_loss() {
    sudo -n ip netns exec "$NS1" tc qdisc del dev sra1 root 2>/dev/null
    sudo -n ip netns exec "$NS1" tc qdisc add dev sra1 root netem delay 1ms loss "$1%" || return 1
}

# one ARM STREAMS LOSS REP: runs it and appends one line to runs.txt
one() {
    local arm=$1 n=$2 loss=$3 rep=$4 bin sublog publog sub_pid p s
    bin="$OUTD/build/node_$arm"
    [ "$arm" = A ] && bin="$OUTD/build/node_M"
    sublog="$OUTD/runs/n${n}_l${loss}-$arm-$rep.sub"
    publog="$OUTD/runs/n${n}_l${loss}-$arm-$rep.pub"
    # shellcheck disable=SC2024 # the logs are written by this shell, as the invoking user: intended
    sudo -n ip netns exec "$NS2" "$bin" sub -n "$n" -s "$SIZE" -d "$DUR" -b 192.168.10.255 >"$sublog" 2>&1 &
    sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" "$bin" pub -n "$n" -s "$SIZE" -r "$RATE" -d "$DUR" -b 192.168.10.255 >"$publog" 2>&1
    wait "$sub_pid"
    p=$(grep -m1 '^RESULT' "$publog" | tr '\n' ' ')
    s=$(grep -m1 '^RESULT' "$sublog" | tr '\n' ' ')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "cell=n${n}_l${loss} arm=$arm rep=$rep VOID no_result" | tee -a "$RUNS"
        return
    fi
    echo "cell=n${n}_l${loss} arm=$arm rep=$rep | $p | $s" >>"$RUNS"
}

ARMS=(M A F)
for rep in $(seq 1 "$REPS"); do
    k=0
    for loss in $LOSSES; do
        set_loss "$loss" || exit 1
        for n in $STREAMS; do
            for j in 0 1 2; do
                one "${ARMS[$(((j + rep + k) % 3))]}" "$n" "$loss" "$rep"
            done
            k=$((k + 1))
        done
    done
    echo "rep $rep done $(date -Is) load $(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUTD/header.txt"
done
python3 -I "$HERE/sub_reassembly_pc_ab.py" "$RUNS" | tee "$OUTD/summary.txt"
echo "AB_DONE $OUTD"

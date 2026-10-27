#!/usr/bin/env bash
# hybrid_rx_writers_ab.sh - the hybrid reader's writer table and whole bodies of unknown layout, PC netns A/B (DESIGN.md
# section 8, "What the reader measures"; 2026-10-27). Does a BEST_EFFORT stream beside 8 or 12 RELIABLE fragmented
# streams get more of its samples when the reader keeps 16 writers and counts a lost body of unknown layout - so that
# its collapse test sees the runs the kernel's reassembly memory refuses - and does nothing else get worse?
# Registered before its first run; the rules are hybrid_rx_writers_ab.py's docstring, in its code.
#
# ARMS, built here from git (each tree's src/ and include/, this harness's sub_reassembly_node.c for both):
#   M  the parent (M_SHA, default origin/main)
#   A  M's binaries run again: A - M is this session's noise, THE FLOOR
#   F  the change (F_SHA, default HEAD)
# Arm order rotated per repetition and cell. rmw_tickle's sample size (-Dtt_MAX_BUFFER_LENGTH=65507
# -Dtt_LARGE_SAMPLES=1), no same-host segment (it would carry the samples past the veth and its loss).
#
# TRAFFIC (sub_reassembly_node.c): N RELIABLE streams of SIZE-byte samples at RATE Hz (KEEP_LAST 10, Subscribers with
# no reorder buffer), one BEST_EFFORT stream of the same, a control of 64-byte RELIABLE samples through a reorder
# buffer; netem "delay 1ms loss L%" on the publisher's veth. Cells n<N>_l<L>: N in STREAMS (default "4 8 12"; 4 is the
# control - N + 1 writers within M's 8 entries), L in LOSSES (default "1 5"). A FRESH namespace pair for every run: the
# kernel keeps incomplete IP reassemblies 30 s, and a namespace reused from a lossy run starts with its reassembly
# memory full (sub_reassembly_pc_ab.sh reused one, and its BEST_EFFORT reading was not independent between runs).
# DUR 40 s (set before the first registered run, after two probes): in a fresh namespace the reassembly memory takes
# ~8 s of 5% loss to fill, so 3 and 10 s runs ended before the squeeze the change is about (a 40 s probe at n8 5%:
# M's reader cap 45, collapses 0, ReasmFails ~17,000, BEST_EFFORT 8.9%; F's collapsed to cap 1, BEST_EFFORT 34%).
# The reader's hybrid line (collapses, cap, rx_writer_evictions) and its namespace's IP reassembly counters
# (/proc/net/snmp Reasm*) are kept per run: the treatment and its witness.
#
# Usage: [M_SHA=..] [F_SHA=..] [REPS=8] [DUR=40] [SIZE=30000] [RATE=30] [STREAMS="4 8 12"] [LOSSES="1 5"]
#        [OUT=/home/semih/hybrid_rx_results/ab_<time>] hybrid_rx_writers_ab.sh
# Needs passwordless `sudo -n ip` (tc runs inside the namespace through `ip netns exec`). Long (~115 min at the
# defaults): launch detached and wait for the line it always ends with, "=== hybrid_rx_writers_ab ended".
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
M_SHA=${M_SHA:-$(git -C "$REPO" rev-parse --short origin/main)}
F_SHA=${F_SHA:-$(git -C "$REPO" rev-parse --short HEAD)}
REPS=${REPS:-8}
DUR=${DUR:-40}
SIZE=${SIZE:-30000}
RATE=${RATE:-30}
STREAMS=${STREAMS:-4 8 12}
LOSSES=${LOSSES:-1 5}
OUTD=${OUT:-/home/semih/hybrid_rx_results/ab_$(date +%Y%m%d-%H%M%S)}
NS1=hrw-pub-$$
NS2=hrw-sub-$$
T0=$(date +%s)

netns_down() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
on_exit() {
    local rc=$?
    netns_down
    echo "=== hybrid_rx_writers_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s, out $OUTD ==="
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
    echo "=== hybrid_rx_writers_ab $(date -Is) M=$M_SHA F=$F_SHA harness=$(git -C "$REPO" rev-parse --short HEAD)" \
        "(harness dirty: $(git -C "$REPO" status --porcelain -- "$HERE/sub_reassembly_node.c" "$HERE/hybrid_rx_writers_ab.sh" "$HERE/hybrid_rx_writers_ab.py" | wc -l))" \
        "reps=$REPS dur=$DUR size=$SIZE rate=$RATE streams=($STREAMS) losses=($LOSSES) ==="
    echo "load at start: $(cut -d' ' -f1-3 /proc/loadavg)"
} | tee "$OUTD/header.txt"

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

# A fresh pair of namespaces joined by a veth, netem on the publisher's side.
netns_up() {
    netns_down
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || return 1
    sudo -n ip link add hrw1 netns "$NS1" type veth peer name hrw2 netns "$NS2" || return 1
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev hrw1 || return 1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev hrw2 || return 1
    sudo -n ip -n "$NS1" link set lo up && sudo -n ip -n "$NS2" link set lo up || return 1
    sudo -n ip -n "$NS1" link set hrw1 up && sudo -n ip -n "$NS2" link set hrw2 up || return 1
    sudo -n ip netns exec "$NS1" tc qdisc add dev hrw1 root netem delay 1ms loss "$1%" || return 1
}

reasm() {
    sudo -n ip netns exec "$NS2" cat /proc/net/snmp |
        awk '/^Ip:/{if(!h){split($0,k);h=1}else{for(i=1;i<=NF;i++) if(k[i]~/Reasm(OKs|Fails)/) printf "%s=%s ", k[i], $i}}'
}

# one ARM STREAMS LOSS REP: runs it in fresh namespaces and appends one line to runs.txt
one() {
    local arm=$1 n=$2 loss=$3 rep=$4 bin sublog publog sub_pid p s h r
    bin="$OUTD/build/node_$arm"
    [ "$arm" = A ] && bin="$OUTD/build/node_M"
    sublog="$OUTD/runs/n${n}_l${loss}-$arm-$rep.sub"
    publog="$OUTD/runs/n${n}_l${loss}-$arm-$rep.pub"
    if ! netns_up "$loss"; then
        echo "cell=n${n}_l${loss} arm=$arm rep=$rep VOID netns" | tee -a "$RUNS"
        return
    fi
    # shellcheck disable=SC2024 # the logs are written by this shell, as the invoking user: intended
    sudo -n ip netns exec "$NS2" "$bin" sub -n "$n" -s "$SIZE" -d "$DUR" -b 192.168.10.255 >"$sublog" 2>&1 &
    sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" "$bin" pub -n "$n" -s "$SIZE" -r "$RATE" -d "$DUR" -b 192.168.10.255 >"$publog" 2>&1
    wait "$sub_pid"
    r=$(reasm)
    netns_down
    p=$(grep -m1 '^RESULT' "$publog" | tr '\n' ' ')
    s=$(grep -m1 '^RESULT' "$sublog" | tr '\n' ' ')
    h=$(grep -m1 -o 'Node [0-9]* hybrid: .*' "$sublog" | grep -o 'collapses=[0-9]*\|path_failures=[0-9]*\|cap=[0-9]*\|rx_writer_evictions=[0-9]*' | tr '\n' ' ')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "cell=n${n}_l${loss} arm=$arm rep=$rep VOID no_result" | tee -a "$RUNS"
        return
    fi
    echo "cell=n${n}_l${loss} arm=$arm rep=$rep | $p | $s | READER $h $r" >>"$RUNS"
}

ARMS=(M A F)
for rep in $(seq 1 "$REPS"); do
    k=0
    for loss in $LOSSES; do
        for n in $STREAMS; do
            for j in 0 1 2; do
                one "${ARMS[$(((j + rep + k) % 3))]}" "$n" "$loss" "$rep"
            done
            k=$((k + 1))
        done
    done
    echo "rep $rep done $(date -Is) load $(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUTD/header.txt"
done
python3 -I "$HERE/hybrid_rx_writers_ab.py" "$RUNS" | tee "$OUTD/summary.txt"
echo "AB_DONE $OUTD"

#!/usr/bin/env bash
# hybrid_rx_drop_pc.sh - a reader whose socket drops foreign datagrams just before its first sample, on a 5% path, on
# the PC: two private network namespaces, a veth pair, netem + tbf on the publisher's egress (hybrid_first_n_pc.sh's
# link), large_sample_bench.c at both ends (1 MB at 30 Hz, RELIABLE KEEP_LAST 10, the reader's cap on - the rig's I1L
# cell), and a flood of FLOOD_COUNT datagrams of FLOOD_BYTES sent inside the reader's namespace to its well-known port
# FLOOD_AT_MS after the publisher starts (its first sample goes at PUB_WAIT_MS), which overruns the reader's socket.
#
# WHY (2026-10-16, the rig's first-n2 r11: 5% netem, Array1m RELIABLE KL10 30 Hz): the reader's well-known socket
# dropped 158 datagrams during its first sample; tt_rx_receiver_drops() cannot say whose they were, and the reader put
# its own path losses down to them one by one (receiver_lost=158), read the excused gaps as a clean stretch, forgot
# its last few events (stale_forgets=1 stale_events=4) and announced "none seen": the writer sent at n = 45 on a 5%
# path. Fix (fix/rx-drop-credit): an excused gap is a doubt - "none seen" and the stale rule start after it.
#
# Arms (each built from a tree exported with git archive or copied from a working tree - never modified here):
#   A    PARENT (default origin/main).
#   B    TIP (default HEAD).
# Cells: l5f (5% loss, the flood), l5 (5% loss, no flood: the control), l0f (no loss, the flood: the cost at 0%).
#
# HOW TO READ IT - written before the first run (2026-10-16), enforced by hybrid_rx_drop_read.py:
#   VOID     a run without both RESULT lines; an l5f/l0f run whose reader counted no receiver drop (the flood did not
#            overrun the socket); an l5f run whose reader excused no gap (receiver_lost 0: the treatment did not reach
#            the estimate); an l5 run whose reader counted no loss event; an l0 run that published fewer than 90% of
#            DURATION x 30 samples.
#   REPRO    A at l5f: at least one run in which the writer heard "none seen" (0xff00) or sent a sample at n = 45.
#            Without one, the route is not reproduced on the PC and B's l5f verdict is NOT READ.
#   FIXED    B at l5f: no run in which the writer heard 0xff00 or sent a sample at n = 45, and the reader's
#            stale_forgets is 0 in every run. Runs with any sample at n > 1 are counted for both arms and reported, not
#            judged: the excused gaps still leave p's mean low until the drops are used up or let go (at most two
#            announce intervals), which the fix does not change.
#   CONTROL  l5 (no flood): no run of either arm sends a sample at n > 1 or hears 0xff00 - without drops the fix
#            changes nothing.
#   KEPT     B at l0f: the median of its samples below n_max is at most A's + 1.
#   Sample size: 40 runs a flood arm; 0 of 40 bounds B's rate below 7.2% at 95%.
#
# Usage: hybrid_rx_drop_pc.sh [REPS_L5F] [REPS_L5] [REPS_L0F] [DURATION_S]   results to $WORK
# Needs passwordless `sudo -n ip` and `tc`. Run it detached (CLAUDE.md, "Long-running work"); every bench process runs
# under `ulimit -v` and a timeout. No rig: PC only, in private namespaces.
set -u
REPS_L5F=${1:-40}
REPS_L5=${2:-10}
REPS_L0F=${3:-10}
DURATION=${4:-4}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
PARENT=${PARENT:-origin/main}
TIP=${TIP:-HEAD}
STAMP=$(date +%Y%m%d-%H%M%S)
WORK=${WORK:-$HOME/hybrid_rx_drop/$STAMP}
OUT=$WORK/results.txt
NS1=hrd-pub-$$
NS2=hrd-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
PUB_WAIT_MS=${PUB_WAIT_MS:-2000}
FLOOD_AT_MS=${FLOOD_AT_MS:-1500}
FLOOD_COUNT=${FLOOD_COUNT:-2000}
FLOOD_BYTES=${FLOOD_BYTES:-8000}
PORT=8282 # _tt_CONTEXT_PORT: the reader's well-known socket
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK/runs"
export_tree() { # <commit> <dir>
    rm -rf "$2"
    mkdir -p "$2"
    git -C "$TREE" archive "$1" src include | tar -x -C "$2" || return 1
    mkdir -p "$2/bench"
    cp "$HERE/large_sample_bench.c" "$2/bench/"
}
build() { # <root> <out>
    cc -O2 -DNDEBUG -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$1/include" -I"$1/src" -o "$2" "$1/bench/large_sample_bench.c" \
        "$1/src/tickle.c" "$1/src/encoding.c" "$1/src/log.c" "$1/src/hal_linux.c" -lpthread -lm
}
parent_sha=$(git -C "$TREE" rev-parse --short=8 "$PARENT") || exit 1
tip_sha=$(git -C "$TREE" rev-parse --short=8 "$TIP") || exit 1
export_tree "$parent_sha" "$WORK/src_A" || exit 1
export_tree "$tip_sha" "$WORK/src_B" || exit 1
build "$WORK/src_A" "$WORK/bench_A" 2>"$WORK/build_A.log" || { echo "build A failed: $WORK/build_A.log"; exit 1; }
build "$WORK/src_B" "$WORK/bench_B" 2>"$WORK/build_B.log" || { echo "build B failed: $WORK/build_B.log"; exit 1; }

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add hrda netns "$NS1" type veth peer name hrdb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.87.1/24 dev hrda
sudo -n ip -n "$NS2" addr add 192.168.87.2/24 dev hrdb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set hrda up
sudo -n ip -n "$NS2" link set hrdb up

shape() { # <loss percent, 0 = none>
    sudo -n ip netns exec "$NS1" tc qdisc del dev hrda root 2>/dev/null
    if [ "$1" != 0 ]; then
        sudo -n ip netns exec "$NS1" tc qdisc add dev hrda root handle 1: netem loss "$1%" limit 10000 || return 1
        sudo -n ip netns exec "$NS1" tc qdisc add dev hrda parent 1:1 handle 10: tbf rate 1gbit burst 64kb \
            latency 100ms || return 1
    else
        sudo -n ip netns exec "$NS1" tc qdisc add dev hrda root tbf rate 1gbit burst 64kb latency 100ms || return 1
    fi
    echo "SHAPE loss=$1 $(sudo -n ip netns exec "$NS1" tc qdisc show dev hrda | tr '\n' ' ')" | tee -a "$OUT"
}

echo "=== hybrid_rx_drop_pc $(date -Is) A=$parent_sha B=$tip_sha reps_l5f=$REPS_L5F reps_l5=$REPS_L5 reps_l0f=$REPS_L0F duration=${DURATION}s pub_wait_ms=$PUB_WAIT_MS flood=${FLOOD_COUNT}x${FLOOD_BYTES}B@${FLOOD_AT_MS}ms load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

# The flood: FLOOD_COUNT datagrams of FLOOD_BYTES, as fast as one socket sends them, from inside the reader's
# namespace to its well-known port. Not TickLE datagrams (no magic): the reader drops them as foreign, if it gets them.
flood() {
    sudo -n ip netns exec "$NS2" timeout 20 python3 -c "
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
payload = b'\x00' * $FLOOD_BYTES
sent = 0
for _ in range($FLOOD_COUNT):
    try:
        s.sendto(payload, ('192.168.87.2', $PORT))
        sent += 1
    except OSError:
        pass
print('FLOOD sent=%d' % sent)
"
}

run() { # <cell> <arm> <rep>
    local cell=$1 arm=$2 rep=$3 bin
    bin=$WORK/bench_$arm
    local tag=$WORK/runs/${cell}_${arm}_r${rep}
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin sub \
        -s 1048576 -d $((DURATION + 7)) -R -b 192.168.87.255" >"$tag.sub" 2>&1 &
    local sub_pid=$!
    sleep 1
    local flood_pid=""
    if [ "${cell%f}" != "$cell" ]; then
        (sleep "$(echo "$FLOOD_AT_MS / 1000" | bc -l)"; flood) >"$tag.flood" 2>&1 &
        flood_pid=$!
    fi
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin pub \
        -s 1048576 -r 30 -d $DURATION -R -M -D $PUB_WAIT_MS -b 192.168.87.255" >"$tag.pub" 2>&1
    wait "$sub_pid"
    [ -n "$flood_pid" ] && wait "$flood_pid"
    local p s peer f
    p=$(grep -m1 '^RESULT:' "$tag.pub" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$tag.sub" | sed 's/^RESULT: //')
    peer=$(grep '^PEER_P ' "$tag.pub" | sed 's/^PEER_P //; s/ /,/g' | tr '\n' ';')
    f=$(grep -m1 '^FLOOD ' "$tag.flood" 2>/dev/null | sed 's/^FLOOD //')
    echo "RUN cell=$cell arm=$arm rep=$rep flood_${f:-sent=0} peer_p=${peer:-none} | ${p:-NO_RESULT} | ${s:-NO_RESULT}" >>"$OUT"
}

# Arms interleaved within each rep, rotated by rep, so drift on the PC falls on both alike.
cells() { # <cell> <loss> <reps>
    local cell=$1 loss=$2 reps=$3 arms=(A B)
    shape "$loss" || exit 1
    for rep in $(seq 1 "$reps"); do
        for i in 0 1; do
            run "$cell" "${arms[$(((i + rep - 1) % 2))]}" "$rep"
        done
        echo "cell=$cell rep=$rep done $(date +%H:%M:%S)"
    done
}
cells l5f 5 "$REPS_L5F"
cells l5 5 "$REPS_L5"
cells l0f 0 "$REPS_L0F"
echo "=== done $(date -Is) ===" | tee -a "$OUT"
python3 "$HERE/hybrid_rx_drop_read.py" "$OUT" | tee "$WORK/verdict.txt"

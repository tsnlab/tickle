#!/usr/bin/env bash
# hybrid_first_n_pc.sh - the first n a RELIABLE AUTO writer sends a reader at, on the PC: two private network
# namespaces, a veth pair, netem on the publisher's egress, large_sample_bench.c at both ends (1 MB at 30 Hz, RELIABLE
# KEEP_LAST 10, the reader's cap on - the rig's I1L cell, Array1m RELIABLE KEEP_LAST 10 at 5% loss).
#
# WHY (2026-10-15, the rig's L4 re-measure on main f5fcda06): in 2 of 5 I1L runs the writer sent its first 4-5 samples
# in runs of 45 (n_hist=1796/0/0/0/0/0/5) on a 5% path, where a run of 45 IP fragments arrives 0.95^45 = 10% of the
# time, and in one of them 2 samples were never delivered (gap_abandoned=2: a LOSE). Hypothesis: netem drops a UDP_SEGMENT
# (GSO) send whole - it sits above the segmentation - so the first sample's 723 MTU units are ~22 chances to lose, not
# 723; the first sample comes through clean ~0.95^22 = 32% of the time, its reader announces "none seen" at its tail
# (tt_HYBRID_NONE_SEEN_FRAMES: 500 clean frames), and the writer reads that as p ~ 0: n_max. The writer is not acting
# before an announce: it has one, and it is wrong.
#
# Arms (each built from a tree exported with git archive or copied from a working tree - never modified here):
#   A    PARENT (default origin/main affea967).
#   Ag   A with TT_UDP_OFFLOAD=0: no UDP_SEGMENT trains, so netem drops frame by frame - the mechanism's control. The
#        hypothesis predicts no sample at n > 1 in any Ag run at 5%.
#   B    TIP (default HEAD; TIP=WORKTREE copies this tree's src/ and include/ as they are).
# Cells: l5 (netem loss 5%), l0 (no netem). Every run prints the publisher's view of the reader's p as it changes
# (PEER_P lines: the sample about to go, the code - 0xff00 is "none seen" - and the n histogram so far).
#
# HOW TO READ IT - written before the first run (2026-10-15), enforced by hybrid_first_n_read.py:
#   VOID     a run without both RESULT lines; an l5 run whose reader counted no loss event (hybrid_rx_loss events 0:
#            the loss did not apply); an l0 run that published fewer than 90% of DURATION x 30 samples.
#   REPRO    (the mechanism, A at l5) at least one A run sent a sample at n > 1, and in every such run the first p the
#            writer heard was "none seen" (0xff00). With no such A run the hypothesis is not reproduced and B's l5
#            cannot fail: its l5 verdict is NOT READ.
#   CONTROL  Ag at l5: no run sends a sample at n > 1. One that does refutes "the trains alone".
#   FIXED    B at l5: no run sends any sample at n > 1, and in every run delivered = published with large_abandoned 0.
#   KEPT     B at l0: every run sends at most 2 samples below n_max (n_hist buckets 1..44), and B's median of them is at
#            most A's + 1 - the slow start's removal (2026-10-13) kept.
#   Sample size: at ~32% a run (the rig's 2 of 5), 30 A runs show none 1e-5 of the time; 0 of 30 B runs bounds B's rate
#   below 10% at 95%.
#
# SECOND CAMPAIGN (2026-10-15, written before its run): PARENT=29223ef8 (the one-a-call fix), TIP=WORKTREE, PUB_WAIT_MS=0.
# On the rig 1 run of 8 at 5% still sent 4 samples at n = 45 (I1L head r1); every rig run broadcast exactly one sample
# before the writer knew its reader (tx_udp_broadcast 788 = 723 + discovery), as performance_test publishes at once.
# Hypothesis: that sample - or one to a reader claimed from its ACKNACK - went as trains (hybrid_peer_unmeasured() did
# not see the reader), the reader measured it and announced "none seen" at its tail. With PUB_WAIT_MS=0 the PC writer
# also publishes before the reader's announce is in. The same rules above, read the same way: REPRO needs A's n > 1 runs
# and their first p 0xff00; FIXED needs B's 0. Sample size: at the rig's 1/8, 40 B runs miss it (7/8)^40 = 0.5% of the
# time; 0 of 40 bounds B's rate below 7.2% at 95%.
#
# THIRD CAMPAIGN (2026-10-16, written before its run): both arms carry no-send-unmatched (a Publisher with no matched
# reader sends nothing). PARENT = main + no-send (ab/hybrid-first-n3's control), TIP = ab/hybrid-first-n3 (first-n's
# one-a-call + the !update_seen half of first-n2), at PUB_WAIT_MS=0 and the default 2000, ARMS_L5="A B". A VOLATILE
# reader now starts at its match point, so FIXED's delivered = published cannot hold for a reader matched after the
# first sample; MATCHED (hybrid_first_n_read.py) reads delivery from the reader's first sample instead: nothing missing
# between its first and last, the last the last published, nothing abandoned, and no sample at n > 1. B passes on 0 of
# >= 60 runs with n > 1 or a gap (rate < 5% at 95%); A's rate is the control's - if A also has 0 runs with n > 1, the
# trigger is gone under no-send and B's 0 shows nothing of the fix (say so; the unit tests carry it).
#
# Usage: hybrid_first_n_pc.sh [REPS_L5] [REPS_L0] [DURATION_S]   results to $WORK (default ~/hybrid_first_n/<stamp>)
# Needs passwordless `sudo -n ip` and `tc`. Run it detached (CLAUDE.md, "Long-running work"); every bench process runs
# under `ulimit -v` and a timeout. No rig: PC only, in private namespaces.
set -u
REPS_L5=${1:-30}
REPS_L0=${2:-10}
DURATION=${3:-4}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
PARENT=${PARENT:-affea967}
TIP=${TIP:-HEAD}
ARMS_L5=${ARMS_L5:-"A Ag B"}
ARMS_L0=${ARMS_L0:-"A B"}
STAMP=$(date +%Y%m%d-%H%M%S)
WORK=${WORK:-$HOME/hybrid_first_n/$STAMP}
OUT=$WORK/results.txt
NS1=hfn-pub-$$
NS2=hfn-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
# How long the publisher polls before its first sample (large_sample_bench.c -D). 2000: the reader's announce is in
# before the first sample. 0: as performance_test under rmw_tickle on the rig, which publishes at once - the writer
# learns its reader from an ACKNACK or a late announce while samples are already going (2026-10-15, I1L head r1).
PUB_WAIT_MS=${PUB_WAIT_MS:-2000}
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK/runs"
export_tree() { # <commit|WORKTREE> <dir>
    rm -rf "$2"
    mkdir -p "$2"
    if [ "$1" = WORKTREE ]; then
        cp -r "$TREE/src" "$TREE/include" "$2/" || return 1
    elif [ "${1#DIR:}" != "$1" ]; then # DIR:<path>: a tree's src/ and include/ copied as they are (an instrumented copy)
        cp -r "${1#DIR:}/src" "${1#DIR:}/include" "$2/" || return 1
    else
        git -C "$TREE" archive "$1" src include | tar -x -C "$2" || return 1
    fi
    # One harness for every arm: this tree's bench (its PEER_P trace), against each arm's core.
    mkdir -p "$2/bench"
    cp "$HERE/large_sample_bench.c" "$2/bench/"
}
build() { # <root> <out>
    cc -O2 -DNDEBUG -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$1/include" -I"$1/src" -o "$2" "$1/bench/large_sample_bench.c" \
        "$1/src/tickle.c" "$1/src/encoding.c" "$1/src/log.c" "$1/src/hal_linux.c" -lpthread -lm
}
parent_sha=$(git -C "$TREE" rev-parse --short=8 "$PARENT") || exit 1
if [ "${TIP#DIR:}" != "$TIP" ]; then
    tip_sha="dir(${TIP#DIR:})"
    tip_ref=$TIP
elif [ "$TIP" = WORKTREE ]; then
    tip_sha="worktree($(git -C "$TREE" rev-parse --short=8 HEAD)+$(git -C "$TREE" status --porcelain -- src include | wc -l)dirty)"
    tip_ref=WORKTREE
else
    tip_sha=$(git -C "$TREE" rev-parse --short=8 "$TIP") || exit 1
    tip_ref=$tip_sha
fi
export_tree "$parent_sha" "$WORK/src_A" || exit 1
export_tree "$tip_ref" "$WORK/src_B" || exit 1
build "$WORK/src_A" "$WORK/bench_A" 2>"$WORK/build_A.log" || { echo "build A failed: $WORK/build_A.log"; exit 1; }
build "$WORK/src_B" "$WORK/bench_B" 2>"$WORK/build_B.log" || { echo "build B failed: $WORK/build_B.log"; exit 1; }

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add hfna netns "$NS1" type veth peer name hfnb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.86.1/24 dev hfna
sudo -n ip -n "$NS2" addr add 192.168.86.2/24 dev hfnb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set hfna up
sudo -n ip -n "$NS2" link set hfnb up

# The rig's link: tbf at 1 Gbit/s under the loss, so a 1 MB sample leaves over ~8 ms as on the Pi's NIC rather than
# arriving at once and overrunning the reader's socket (the first smoke run, veth unshaped: 2% receiver loss at 0%,
# p announced 0x0564 and every sample MTU). netem's loss is decided as a packet is enqueued - a UDP_SEGMENT send is
# still one packet there - and tbf below it then cuts and paces it.
shape() { # <loss percent, 0 = none>
    sudo -n ip netns exec "$NS1" tc qdisc del dev hfna root 2>/dev/null
    if [ "$1" != 0 ]; then
        sudo -n ip netns exec "$NS1" tc qdisc add dev hfna root handle 1: netem loss "$1%" limit 10000 || return 1
        sudo -n ip netns exec "$NS1" tc qdisc add dev hfna parent 1:1 handle 10: tbf rate 1gbit burst 64kb \
            latency 100ms || return 1
    else
        sudo -n ip netns exec "$NS1" tc qdisc add dev hfna root tbf rate 1gbit burst 64kb latency 100ms || return 1
    fi
    echo "SHAPE loss=$1 $(sudo -n ip netns exec "$NS1" tc qdisc show dev hfna | tr '\n' ' ')" | tee -a "$OUT"
}

echo "=== hybrid_first_n_pc $(date -Is) A=$parent_sha B=$tip_sha reps_l5=$REPS_L5 reps_l0=$REPS_L0 duration=${DURATION}s pub_wait_ms=$PUB_WAIT_MS load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

run() { # <cell> <arm> <rep>
    local cell=$1 arm=$2 rep=$3 bin env=""
    case $arm in
        A) bin=$WORK/bench_A ;;
        Ag) bin=$WORK/bench_A; env="TT_UDP_OFFLOAD=0" ;;
        B) bin=$WORK/bench_B ;;
        *) echo "unknown arm $arm"; return ;;
    esac
    local tag=$WORK/runs/${cell}_${arm}_r${rep}
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" env $env bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin sub \
        -s 1048576 -d $((DURATION + 7)) -R -b 192.168.86.255" >"$tag.sub" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" env $env bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin pub \
        -s 1048576 -r 30 -d $DURATION -R -M -D $PUB_WAIT_MS -b 192.168.86.255" >"$tag.pub" 2>&1
    wait "$sub_pid"
    local p s peer
    p=$(grep -m1 '^RESULT:' "$tag.pub" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$tag.sub" | sed 's/^RESULT: //')
    peer=$(grep '^PEER_P ' "$tag.pub" | sed 's/^PEER_P //; s/ /,/g' | tr '\n' ';')
    echo "RUN cell=$cell arm=$arm rep=$rep peer_p=${peer:-none} | ${p:-NO_RESULT} | ${s:-NO_RESULT}" >>"$OUT"
}

# Arms interleaved within each rep, rotated by rep, so drift on the PC falls on every arm alike.
cells() { # <cell> <loss> <reps> <arms>
    local cell=$1 loss=$2 reps=$3 arms
    read -r -a arms <<<"$4"
    shape "$loss" || exit 1
    local n=${#arms[@]}
    for rep in $(seq 1 "$reps"); do
        for i in $(seq 0 $((n - 1))); do
            run "$cell" "${arms[$(((i + rep - 1) % n))]}" "$rep"
        done
        echo "cell=$cell rep=$rep done $(date +%H:%M:%S)"
    done
}
cells l5 5 "$REPS_L5" "$ARMS_L5"
cells l0 0 "$REPS_L0" "$ARMS_L0"
echo "=== done $(date -Is) ===" | tee -a "$OUT"
python3 "$HERE/hybrid_first_n_read.py" "$OUT" | tee "$WORK/verdict.txt"

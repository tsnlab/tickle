#!/usr/bin/env bash
# keep_last_repair_pc_ab.sh - RELIABLE KEEP_LAST repair as DDS/RTPS has it (DESIGN.md section 6, 2026-10-15): does a
# writer that resends for as long as its history holds a datagram, and a reader that gives a gap up only on the
# writer's eviction HEARTBEAT (backing its requests off past tt_RELIABLE_RETRY), deliver more, without costing wire
# bytes or tail latency under plain loss? The PC side; the rig's core loss cells and I1L are the lead's.
#
# ARMS, three, each a set of binaries built beforehand (each tree with its own install prefix, harness held at the
# parent: only src/ and include/ differ between BASE and NEW):
#   base     the parent (b265e86f)
#   base_aa  the same binaries run again: its difference from base is this session's noise, THE FLOOR
#   new      the change
# Arm order rotated per repetition and cell, so drift lands on every arm.
#
# TRAFFIC. Publisher (client) and subscriber (server) in two private network namespaces joined by a veth pair (never
# the default namespace: it reaches the rig), netem loss on the publisher's egress.
#   T<p>  reliable_throughput, payload shape p1-p4, KEEP_LAST at the bench's default depth (64), as fast as it goes,
#         DUR seconds.
#   L<p>  reliable_latency, p1-p4, pinging at most every millisecond, LATDUR seconds measured between warm-up and
#         cool-down edges of 50 round trips (each at most 2 s).
#   I1L   the large-sample bench, 1 MB at 30 Hz, RELIABLE KEEP_LAST 10 (the rig's I1L shape), LDUR seconds.
# at LOSSES (default "1 5") percent.
#
# HOW TO READ IT, written before the first run (2026-10-15) and implemented in keep_last_repair_pc_ab.py:
#   Per run: delivered % (recv / sent; for I1L delivered / published), gap_abandoned, gap_evicted, wire bytes per
#     sample (both veths' tx_bytes over samples sent: data, repairs, HEARTBEATs and ACKNACKs), and for L and I1L the
#     p99 latency.
#   Run VOID: a missing RESULT line on either side, or nothing sent.
#   Overall VOID for a cell: fewer than 3 valid runs of any arm.
#   Per (cell, metric): d = mean over reps of (X_arm - X_base) for delivered % and the gap counters, and of
#     (X_arm / X_base - 1) for wire bytes and p99; FLOOR = max(1% relative / 0.1 points, |d(base_aa)| + 2 SE(base_aa)).
#     BETTER / WORSE beyond the floor and beyond 2 SE of new's own d; otherwise HELD.
#   Expectation (falsifiable): gap_abandoned falls to ~0 in every cell (a window jump can still count one); delivered
#     % HELD or BETTER everywhere, BETTER where the parent gave up units the writer still held (5% loss, and I1L);
#     wire bytes per sample HELD (the extra requests and resends past the third are rare under plain loss), p99 HELD.
#     A WORSE delivered % or p99 anywhere refutes "no cost"; gap_abandoned not falling refutes the treatment (the arm
#     did not apply the change).
#
# Usage: keep_last_repair_pc_ab.sh <base_tree> <new_tree> <base_large_bench> <new_large_bench>
#        env: REPS (5) DUR (5) LATDUR (10) LDUR (20) LOSSES ("1 5") CELLS (all: "T1 T2 T3 T4 L1 L2 L3 L4 I1L") OUT (dir)
# Each tree must hold examples/perf_hil/tickle/{reliable_throughput,reliable_latency}_p{1..4}/{client,server}, built by
# that tree's build.sh. Needs passwordless `sudo -n ip`; tc runs inside the private namespace through `ip netns exec`.
# Long: launch detached, read $OUT/summary.txt.
set -uo pipefail
BASE_TREE=${1:?base tree}
NEW_TREE=${2:?new tree}
BASE_LARGE=${3:?base large bench}
NEW_LARGE=${4:?new large bench}
REPS=${REPS:-5}
DUR=${DUR:-5}
LDUR=${LDUR:-20}
LATDUR=${LATDUR:-10}
LOSSES=${LOSSES:-1 5}
CELLS=${CELLS:-T1 T2 T3 T4 L1 L2 L3 L4 I1L}
OUTD=${OUT:-$HOME/keep_last_repair_pc_ab/$(date +%Y%m%d-%H%M%S)}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUTD/runs" || exit 1
RUNS="$OUTD/runs.txt"
: >"$RUNS"
NS1=klr-pub-$$
NS2=klr-sub-$$

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add klr1 netns "$NS1" type veth peer name klr2 netns "$NS2" || exit 1
# The addresses the benches broadcast for (192.168.10.255), as on the rig.
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev klr1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev klr2
for ns in "$NS1" "$NS2"; do
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NS1" link set klr1 up
sudo -n ip -n "$NS2" link set klr2 up

tree_of() { case $1 in new) echo "$NEW_TREE" ;; *) echo "$BASE_TREE" ;; esac; }
large_of() { case $1 in new) echo "$NEW_LARGE" ;; *) echo "$BASE_LARGE" ;; esac; }
txb() { sudo -n ip netns exec "$1" cat "/sys/class/net/$2/statistics/tx_bytes"; }

{
    echo "=== keep_last_repair_pc_ab $(date -Is) reps=$REPS dur=$DUR ldur=$LDUR losses=($LOSSES) cells=($CELLS) ==="
    for arm in base new; do
        t=$(tree_of $arm)
        echo "arm $arm tree=$t throughput_p1_client_md5=$(md5sum "$t/examples/perf_hil/tickle/reliable_throughput_p1/client" | cut -c1-12) large_md5=$(md5sum "$(large_of $arm)" | cut -c1-12)"
    done
    echo "uptime: $(uptime)"
} | tee "$OUTD/header.txt"
# IDENTITY: the parent's core carries the KEEP_LAST give-up and its log line, the change does not. Checked in every
# binary an arm runs, so a tree built from the wrong source cannot be measured as the other arm.
for arm in base new; do
    t=$(tree_of $arm)
    for bin in "$t"/examples/perf_hil/tickle/reliable_{throughput,latency}_p[1-4]/{client,server} "$(large_of $arm)"; do
        n=$(strings "$bin" | grep -c 'Giving up on a reliable sample')
        if { [ $arm = base ] && [ "$n" = 0 ]; } || { [ $arm = new ] && [ "$n" != 0 ]; }; then
            echo "IDENTITY VOID: $bin (arm $arm) has $n give-up strings" | tee -a "$OUTD/header.txt"
            exit 1
        fi
    done
done
echo "identity: every base binary has the give-up, no new binary does" | tee -a "$OUTD/header.txt"

set_loss() {
    sudo -n ip netns exec "$NS1" tc qdisc del dev klr1 root 2>/dev/null
    if [ "$1" != 0 ]; then
        sudo -n ip netns exec "$NS1" tc qdisc add dev klr1 root netem loss "$1%" || return 1
    fi
}

# one ARM CELL LOSS REP: runs it and appends one line to runs.txt
one() {
    local arm=$1 cell=$2 loss=$3 rep=$4 t sublog publog sub_pid b1 b2 p s
    t=$(tree_of "$arm")
    sublog="$OUTD/runs/$cell-$loss-$arm-$rep.sub"
    publog="$OUTD/runs/$cell-$loss-$arm-$rep.pub"
    b1=$(txb "$NS1" klr1)
    b2=$(txb "$NS2" klr2)
    case $cell in
    T?)
        local dir=$t/examples/perf_hil/tickle/reliable_throughput_p${cell#T}
        # shellcheck disable=SC2024 # the logs are written by this shell, as the invoking user: intended
        sudo -n ip netns exec "$NS2" "$dir/server" -d $((DUR + 6)) >"$sublog" 2>&1 &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS1" "$dir/client" -d "$DUR" >"$publog" 2>&1
        wait "$sub_pid"
        ;;
    L?)
        local dir=$t/examples/perf_hil/tickle/reliable_latency_p${cell#L}
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS2" "$dir/server" -d $((LATDUR + 6)) -W 50 -C 50 -T 2 >"$sublog" 2>&1 &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS1" "$dir/client" -i 0.001 -d "$LATDUR" -W 50 -C 50 -T 2 >"$publog" 2>&1
        wait "$sub_pid"
        ;;
    I1L)
        local bin
        bin=$(large_of "$arm")
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS2" "$bin" sub -s 1048576 -d $((LDUR + 8)) -R >"$sublog" 2>&1 &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS1" "$bin" pub -s 1048576 -r 30 -d "$LDUR" -R >"$publog" 2>&1
        wait "$sub_pid"
        ;;
    esac
    local wire=$(($(txb "$NS1" klr1) - b1 + $(txb "$NS2" klr2) - b2))
    p=$(grep -m1 '^RESULT' "$publog" | tr '\n' ' ')
    s=$(grep -m1 '^RESULT' "$sublog" | tr '\n' ' ')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "$cell loss=$loss arm=$arm rep=$rep VOID no_result" | tee -a "$RUNS"
        return
    fi
    echo "$cell loss=$loss arm=$arm rep=$rep wire_bytes=$wire | $p | $s" >>"$RUNS"
}

ARMS=(base base_aa new)
for rep in $(seq 1 "$REPS"); do
    for loss in $LOSSES; do
        set_loss "$loss" || exit 1
        k=0
        for cell in $CELLS; do
            for j in 0 1 2; do
                arm=${ARMS[$(((j + rep + k) % 3))]}
                one "$arm" "$cell" "$loss" "$rep"
            done
            k=$((k + 1))
        done
    done
    echo "rep $rep done $(date -Is) uptime: $(uptime)" | tee -a "$OUTD/header.txt"
done
set_loss 0
python3 "$HERE/keep_last_repair_pc_ab.py" "$RUNS" | tee "$OUTD/summary.txt"
echo "AB_DONE $OUTD"

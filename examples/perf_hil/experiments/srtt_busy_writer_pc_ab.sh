#!/usr/bin/env bash
# srtt_busy_writer_pc_ab.sh - the reader's retry interval and a busy writer (DESIGN.md section 6, 2026-10-16): does
# leaving out of srtt/rttvar a recovery whose wait saw the writer's large sample still arriving (tt_WriterProxy.
# probe_busy), and not asking that writer again while it sends and answers nothing (repair_arrival_ns), make a large
# RELIABLE sample whole sooner after its send under loss, without costing wire bytes, delivery or tail latency? The PC
# side, in two private network namespaces; a rig A/B is the lead's.
#
# ARMS, three, each built by this script from `git archive <sha> src include examples/perf_hil` into $OUT/trees/:
#   base     the parent
#   base_aa  the same binaries run again: its difference from base is this session's noise, THE FLOOR
#   new      the change
# Arm order rotated per repetition and cell, so drift lands on every arm.
#
# TRAFFIC. Publisher and subscriber in two private network namespaces joined by a veth pair (never the default
# namespace: it reaches the rig), netem loss on the publisher's egress.
#   I4  large_sample_bench.c, 4 MB at 15 Hz, RELIABLE KEEP_LAST 10, LDUR seconds: latency is publish (encode start) to
#       callback, so at 0% loss it is the send itself and its excess at 5% is the time after the send until the
#       sample is whole - the quantity the change is about.
#   T4  reliable_throughput p4 (2,812 B, DATA_FRAG), KEEP_LAST 64, as fast as it goes, DUR seconds.
#   L4  reliable_latency p4, pinging at most every millisecond, LATDUR seconds.
# I4 at 0% and 5%, T4 and L4 at 1% and 5%.
#
# HOW TO READ IT, written before the first run (2026-10-16) and implemented in srtt_busy_writer_pc_ab.py:
#   Per run: delivered %, wire bytes per sample (both veths' tx_bytes over samples sent), for I4 the latency median
#     and p99 and the writer's and reader's datagrams per sample (tx_datagrams), for L4 the p99 round trip.
#   Run VOID: a missing RESULT line on either side, or nothing sent. Cell VOID: fewer than 3 valid runs of any arm.
#   Per (cell, metric): d = mean over reps of (X_arm - X_base) for delivered %, of (X_arm / X_base - 1) otherwise;
#     FLOOR = max(1% relative / 0.1 points, |d(base_aa)| + 2 SE(base_aa)); BETTER / WORSE beyond the floor and beyond
#     2 SE of new's own d; otherwise HELD.
#   CONTROL: T4 at 1%. Core's p4 build has no large samples, so neither the busy mark nor the held retry exists in it,
#     and the one change it carries - the retry ceiling at 64 x max(srtt, G) - binds only for an srtt below G / 64,
#     ~1 us, which no recovery across a veth reaches. Anything but HELD there means the session is too noisy to read
#     the rest, and the run says so. (I4 at 0% is no control: on this PC the 4 MB reader drops datagrams at its
#     socket - 48-106 thousand a 4 s smoke run with a 212,992 B buffer - so it recovers there too. The core default
#     of 1 MB is used.)
#   Expectation (falsifiable): I4 at 5% latency median and p99 BETTER or HELD (HELD if this PC's writer reads requests
#     while it sends, so that its recoveries never spanned a send), wire bytes and writer datagrams per sample HELD or
#     BETTER, delivered HELD. T4/L4 HELD everywhere: core's p4 build has no large samples, and the only change it
#     carries - the retry ceiling at 64 x max(srtt, G) - binds only for an srtt below G / 64. A WORSE wire bytes,
#     delivered % or latency anywhere refutes "no cost".
#
# Usage: srtt_busy_writer_pc_ab.sh <repo> <base_sha> <new_sha>
#        env: REPS (5) DUR (5) LATDUR (10) LDUR (20) OUT (dir)
# Needs passwordless `sudo -n ip`; tc runs inside the private namespace through `ip netns exec`.
# Long: launch detached, read $OUT/summary.txt.
set -uo pipefail
REPO=${1:?repo}
BASE_SHA=${2:?base sha}
NEW_SHA=${3:?new sha}
REPS=${REPS:-5}
DUR=${DUR:-5}
LDUR=${LDUR:-20}
LATDUR=${LATDUR:-10}
OUTD=${OUT:-$HOME/srtt_busy_writer_pc_ab/$(date +%Y%m%d-%H%M%S)}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUTD/runs" "$OUTD/trees" || exit 1
RUNS="$OUTD/runs.txt"
: >"$RUNS"
NS1=sbw-pub-$$
NS2=sbw-sub-$$

# --- build: each arm's core and harness from its own commit ---------------------------------------------------------
build_tree() { # <sha> <dir>
    local sha=$1 d=$2 arr bytes
    rm -rf "$d" && mkdir -p "$d" || return 1
    git -C "$REPO" archive "$sha" src include examples/perf_hil | tar -x -C "$d" || return 1
    local core="$d/src/tickle.c $d/src/encoding.c $d/src/log.c $d/src/hal_linux.c"
    # shellcheck disable=SC2086 # core is a list of files
    cc -O2 -DNDEBUG -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -I"$d/include" -I"$d/src" -o "$d/large_sample_bench" \
        "$d/examples/perf_hil/experiments/large_sample_bench.c" $core -lpthread -lm || return 1
    local p4="$d/examples/perf_hil/tickle/common/p4"
    arr=$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$p4/Bench.msg")
    bytes=$((12 + arr))
    for scen in reliable_throughput reliable_latency; do
        for role in client server; do
            # shellcheck disable=SC2086
            cc -O2 -DNDEBUG -DBENCH_CORE_BUILD=release -DBENCH_SAMPLE_PATH=frag -DBENCH_DATAGRAM_BYTES=1472 \
                -Dtt_MAX_SAMPLE_LENGTH=4096 -DBENCH_SAMPLE_BYTES=$bytes -I"$p4" \
                -I"$d/examples/perf_hil/tickle/common" -I"$d/include" -I"$d/src" -o "$d/${scen}_$role" \
                "$d/examples/perf_hil/tickle/$scen/$role.c" "$p4/Bench.c" $core -lm -lpthread || return 1
        done
    done
}
build_tree "$BASE_SHA" "$OUTD/trees/base" || { echo "BUILD FAILED base"; exit 1; }
build_tree "$NEW_SHA" "$OUTD/trees/new" || { echo "BUILD FAILED new"; exit 1; }
# IDENTITY: the change's core has the busy mark, the parent's has not - checked in the source each arm was built from.
if grep -q probe_busy "$OUTD/trees/base/src/tickle.c" || ! grep -q probe_busy "$OUTD/trees/new/src/tickle.c"; then
    echo "IDENTITY VOID: base must lack probe_busy and new must have it" | tee "$OUTD/header.txt"
    exit 1
fi
tree_of() { case $1 in new) echo "$OUTD/trees/new" ;; *) echo "$OUTD/trees/base" ;; esac; }

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add sbw1 netns "$NS1" type veth peer name sbw2 netns "$NS2" || exit 1
# The addresses the benches broadcast for (192.168.10.255), as on the rig.
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev sbw1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev sbw2
for ns in "$NS1" "$NS2"; do
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NS1" link set sbw1 up
sudo -n ip -n "$NS2" link set sbw2 up
txb() { sudo -n ip netns exec "$1" cat "/sys/class/net/$2/statistics/tx_bytes"; }

{
    echo "=== srtt_busy_writer_pc_ab $(date -Is) base=$BASE_SHA new=$NEW_SHA reps=$REPS dur=$DUR ldur=$LDUR latdur=$LATDUR ==="
    for arm in base new; do
        echo "arm $arm large_md5=$(md5sum "$(tree_of $arm)/large_sample_bench" | cut -c1-12)" \
            "t4_client_md5=$(md5sum "$(tree_of $arm)/reliable_throughput_client" | cut -c1-12)"
    done
    echo "identity: base lacks probe_busy, new has it"
    echo "uptime: $(uptime)"
} | tee "$OUTD/header.txt"

set_loss() {
    sudo -n ip netns exec "$NS1" tc qdisc del dev sbw1 root 2>/dev/null
    if [ "$1" != 0 ]; then
        sudo -n ip netns exec "$NS1" tc qdisc add dev sbw1 root netem loss "$1%" || return 1
    fi
}

# one ARM CELL LOSS REP: runs it and appends one line to runs.txt
one() {
    local arm=$1 cell=$2 loss=$3 rep=$4 t sublog publog sub_pid b1 b2 p s
    t=$(tree_of "$arm")
    sublog="$OUTD/runs/$cell-$loss-$arm-$rep.sub"
    publog="$OUTD/runs/$cell-$loss-$arm-$rep.pub"
    b1=$(txb "$NS1" sbw1)
    b2=$(txb "$NS2" sbw2)
    case $cell in
    T4)
        # shellcheck disable=SC2024 # the logs are written by this shell, as the invoking user: intended
        sudo -n ip netns exec "$NS2" "$t/reliable_throughput_server" -d $((DUR + 6)) >"$sublog" 2>&1 &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS1" "$t/reliable_throughput_client" -d "$DUR" >"$publog" 2>&1
        wait "$sub_pid"
        ;;
    L4)
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS2" "$t/reliable_latency_server" -d $((LATDUR + 6)) -W 50 -C 50 -T 2 >"$sublog" 2>&1 &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS1" "$t/reliable_latency_client" -i 0.001 -d "$LATDUR" -W 50 -C 50 -T 2 >"$publog" 2>&1
        wait "$sub_pid"
        ;;
    I4)
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS2" "$t/large_sample_bench" sub -s 4194304 -d $((LDUR + 8)) -R >"$sublog" 2>&1 &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS1" "$t/large_sample_bench" pub -s 4194304 -r 15 -d "$LDUR" -R >"$publog" 2>&1
        wait "$sub_pid"
        ;;
    esac
    local wire=$(($(txb "$NS1" sbw1) - b1 + $(txb "$NS2" sbw2) - b2))
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
    k=0
    for spec in I4:0 I4:5 T4:1 L4:1 T4:5 L4:5; do
        cell=${spec%:*}
        loss=${spec#*:}
        set_loss "$loss" || exit 1
        for j in 0 1 2; do
            arm=${ARMS[$(((j + rep + k) % 3))]}
            one "$arm" "$cell" "$loss" "$rep"
        done
        k=$((k + 1))
    done
    echo "rep $rep done $(date -Is) uptime: $(uptime)" | tee -a "$OUTD/header.txt"
done
set_loss 0
python3 "$HERE/srtt_busy_writer_pc_ab.py" "$RUNS" | tee "$OUTD/summary.txt"
echo "AB_DONE $OUTD"

#!/usr/bin/env bash
# wifi_burst_loss_netns.sh - network profiles phase 3 (Wi-Fi, local values only), groundwork: the CURRENT build under
# Wi-Fi-like loss and delay, between private network namespaces on this PC. 2026-10-22. No rig, no default namespace:
# every node runs under `sudo -n ip netns exec`, and the one EXIT trap removes every namespace this run made.
#
# WHY netem AND NOT mac80211_hwsim. The plan (NETWORK_PROFILES_PLAN section 7) names hwsim for the PC stage. It is not
# loaded on this PC and `sudo -n` permits only ip and tc, so this is netem: Gilbert-Elliott burst loss (`loss gemodel`)
# and `slot` (frames held and released in bursts at random 1-4 ms slot boundaries, which is how netem models Wi-Fi
# aggregation: jitter WITHOUT reordering, which plain `delay X Y` jitter would add and Wi-Fi does not). What netem
# cannot model: per-receiver loss of one broadcast (here a broadcast is lost for every receiver at once), MAC
# retransmission of unicast (modelled only as unicast getting a lighter loss than broadcast in the bc arm), the basic
# rate's airtime, and power save. A burst is counted in PACKETS on one egress, not in time: a repair sent right after a
# loss continues the same burst, which is harsher on any repair scheme than a time-based burst at these rates - FEC
# parity sent in the burst included.
#
# TOPOLOGY. Three node namespaces - ping (context 1, 10.78.0.1), echo (2, .2), idle (3, .3) - each with one veth to a
# bridge in a hub namespace: one broadcast domain. All shaping is on each node's own egress (wl0), so every datagram
# crosses exactly one shaper: root prio with two bands, band 1:1 for the broadcast address (u32 on 10.78.0.255 and
# 255.255.255.255) and band 1:2 for the rest, each a netem "limit 10000 delay 2ms slot 1ms 4ms packets 42" plus the
# arm's loss. The same structure in every arm, the control included, so arms differ only in the loss and delay they
# name. The shared-memory transport is compiled out (the namespaces share /dev/shm; with it the data would bypass the
# veth entirely).
#
# CELLS (each at the same fixed offered rate in every arm):
#   be_small   100-byte BEST_EFFORT samples at 100 Hz, echoed (wifi_link_node.c ping/echo).
#   rel_small  the same, RELIABLE KEEP_LAST 10 (rmw's default profile).
#   rel_large  1 MiB RELIABLE samples at 5 Hz (large_sample_bench.c, rmw's core build: 65507-byte datagram, large
#              samples), one way; its idle node is wifi_link_node idle.
# Every cell also carries the idle node, which sends no data: its liveliness rests on broadcast summaries alone.
#
# ARMS (both directions, all three nodes; L = average loss, B = mean burst length in packets):
#   ctl      no loss: the control - the change of arm provably cannot touch it, and every lossy arm reads against it.
#   ge1b2 ge1b8 ge5b2 ge5b4 ge5b8 ge10b4   Gilbert-Elliott, 1-k = 0, 1-h = 100%: r = 1/B, p = L r / (1 - L).
#   bc       unicast ge1b2 (MAC retries make unicast nearly clean), broadcast L 30% B 3: Wi-Fi's broadcast is sent at
#            the basic rate with no ACK and no retry.
#   tv       time-varying: every PHASE_S the harness changes every node's shaping with `tc qdisc change`, cycling clean ->
#            ge5b4 -> ge10b8 with 20 ms more delay -> clean ...
#
# PRE-REGISTERED (written 2026-10-22 before the first run; the code that applies it is wifi_burst_read.py, called at the
# end, and it - not this comment - decides):
#   SAMPLE SIZE. Small cells: DUR=30 s x 100 Hz = 3000 samples a rep, 1 s of warm-up unscored, REPS=3: ~8700 scored a
#     cell. At L = 1% a run sees no loss with probability (0.99)^2900 ~ e^-29: every lossy rep is exercised; the p99
#     rests on ~87 samples pooled. rel_large: 5 Hz x 30 s = 150 a rep, 450 a cell; each 1 MiB sample is ~723 datagrams,
#     so at L = 1% (0.99)^723 = 0.07% of samples see no loss: every sample is a repair test.
#   VOID (the run says nothing): a missing RESULT line or a Publisher that never matched; the control losing anything
#     (BE < 99.9% delivered, RELIABLE < 100%, or a tc drop on any band) - the setup drops; a lossy arm whose realized
#     loss on ping's unicast band (tc counters) is outside [0.5 L, 1.5 L], or bc's broadcast band outside [15%, 45%] -
#     the arm did not apply its treatment; be_small's delivered loss disagreeing with that realized loss by more than
#     max(1 point, 0.3 L) - one of the two instruments is wrong; fewer than 2000 scored samples in a small rep or 100 in
#     a large one; the PC's 1-minute load above nproc/2 at a rep's start (latency void, delivery kept).
#   DECISION RULE (for the lead: the WIFI profile with local values, run by this harness with PROFILE=wifi, against
#     these rules; the baseline is the same rules on ETHERNET):
#     base_rtt = ctl rel_small round-trip p50; base_owd = ctl rel_small one-way p50; T = 10 ms (one sample period).
#     R1 rel_small delivers >= 99.5% of offered at ge5b2 and ge5b4.
#     R2 rel_small one-way p99 <= base_owd + T + K x 1.5 x base_rtt, K = 2 at ge5b2 and 3 at ge5b4 - the repair
#        rounds an ideal ARQ needs at that arm's p99 (wifi_burst_read.py derives K: a lost sample needs another round
#        while the burst lasts, ~(1 - 1/B)^2 a round at these rates, and K is the least k with L x that^k <= 1%), each
#        round a round trip and half a round trip of retry timer. (Since 2026-10-23 plus a detection term D: AMENDED
#        below.)
#     R3 rel_large delivers >= 95% of offered at ge5b4 and ge10b4.
#     R4 rel_large one-way p50 <= ctl p50 + 4 x base_rtt at ge5b4 and ge10b4 (one bitmap ACKNACK names every unit of
#        a 1 MiB sample - 723 bits, inside the 8192-bit window - so an ideal repair takes a few rounds).
#     R5 no false peer death (a departure seen before that peer's own end) in any arm, the idle node included.
#     R6 the ping node's join (create to matched) p99 <= 2 s (two summary intervals) in every arm, bc included.
#     R7 tv: duplicates at the receiver <= 1% of delivered (a retry timer that keeps up with the delay step).
#   READING. R1/R2 failing at ge5b2 with the WIFI profile -> local values are not enough for small-sample loss and
#     latency where FEC can act (short bursts): lift FEC. Failing only at ge5b4/ge5b8 -> XOR FEC loses its parity in the
#     same burst; that is interleaving or RS, a separate decision for the user, not this lift. R3/R4 failing with the
#     reverse direction's bytes above 2% of the forward bytes at that arm -> the ACKNACK is material: lift range NACK;
#     failing with less -> a timer or pacing problem, a local value. R5/R6/R7 are local values only (discovery silence,
#     unicast summary repeat, the RTT estimator) and never lift a wire change. Run header merge is a wired-link
#     decision (wire bytes per sample in ctl, printed) and is not judged here.
#
# CORRECTED AFTER THE FIRST BASELINE (2026-10-22/23, ~/rig_results_safe/wifi_baseline/base_20261022), each in the reader
# and named there; no threshold above moved:
#   - R7's instrument was blind: a RELIABLE reader drops a copy below its acknowledged sequence before decoding, so the
#     node's own duplicate count is always 0. R7 now reads core's late_below_ack + duplicates (RSTATS).
#   - A departure answered by the same peer's re-appearance within 10 ms (a table refresh, 2 us apart) is not a death.
#   - Realized loss is counted per skb, as netem drops: tc's "Sent ... pkt" counts the segments of a multi-frame skb,
#     the veth's tx_packets the skbs. On the large cell netem therefore drops some frames in groups.
#   - Other sessions' builds loaded the PC past nproc/2 for most reps; latency comes from the unloaded ones (REP_FIRST
#     added reps 4-6), and a decision cell needs two of them (POOLED_MIN, not pre-registered). The all-reps p99 is
#     printed beside it: the control's p99 moved 6.0 -> 6.7 ms at load 30.
#   - R2's K model counts repair rounds and not the wait before a gap is seen: core's detect-to-recover was under 8 ms
#     for ~90% of gaps at ge5b2/ge5b4, so most of the excess over the limit is the burst itself, before detection.
#
# AMENDED 2026-10-23, BEFORE THE FIRST PROFILE=wifi RUN (the WIFI profile's discovery values; no threshold moved):
#   - rel_large's two large_sample_bench nodes run the profile too (-P wifi): until now only that cell's idle node did,
#     so PROFILE named one node of three there, and R5 in rel_large is the idle node's view of those two. A
#     PROFILE=ethernet run keeps the baseline's command lines exactly.
#   - The treatment check: every node's RESULT names the discovery it ran (miss_threshold, summary_copies,
#     announce_repeats sent). A wifi run's cell is VOID where any node ran the default miss threshold or the idle node
#     sent no summary copy or ping no announce repeat; an ethernet run's where any node sent either or ran another
#     threshold. The reader prints each cell's totals beside R5 and R6.
#
# AMENDED 2026-10-23, BEFORE THE WIFI REPAIR RUNS (R2 only; K, T, the 1.5 factor and every other threshold unchanged):
#   - R2's limit gains a DETECTION term: base_owd + T + D + K x 1.5 x base_rtt, D = (B - 1) / R. A lost sample cannot
#     be asked for before the reader sees the gap, and it sees it only when a later datagram of the writer gets
#     through. netem's Gilbert-Elliott burst is counted in packets on the writer's egress (1-h = 100%: the bad state
#     loses every packet, and leaves it with probability r = 1/B a packet), so after a lost datagram the burst goes
#     on, memoryless, for (1 - r) / r = B - 1 more packets on average, and those packets take (B - 1) / R seconds,
#     R being the egress's packet rate. T already holds the wait for the one next sample after the burst (an
#     ACKNACK getting through first reveals nothing). R is the arm's own realized rate: ping's unicast skbs, sent and
#     dropped (tc), over its published samples / 100 Hz, pooled over the cell's reps - the arm's repair traffic
#     included, which makes R larger and D smaller than at the offered rate (~101/s), so a profile that sends more
#     gets a tighter limit, never a looser one. D is the mean, as K's per-round continuation is: a quantile of the
#     burst would count the tail twice, K already giving a lost sample more rounds while the burst lasts.
#   - Why: on the first baseline and on WW, core's detect-to-recover was under 8 ms for ~90% of gaps at ge5b2/ge5b4,
#     yet the p99 exceeded the limit by 4-17 ms; the round count left out the time before detection (corrected
#     above). This is written AFTER those runs failed R2, so their re-reading under it is a control of the formula,
#     not a test of anything: the test is a run made after this line. The reader prints both readings, the rule as
#     registered on 2026-10-22 (R2-as-registered) and as amended (R2, which the READING uses).
#   - What would show the term wrong: in a run made after it, the reader's own detect-to-recover (RSTATS) staying
#     under 8 ms for most gaps while the p99 still exceeds the amended limit - the excess is then neither detection
#     nor repair rounds, and the amendment explains nothing.
#
# ADDED 2026-10-23, BEFORE ITS FIRST RUN - CLOCK_PPS (default 0: every command line as before). A burst is counted in
# packets on one egress, so a node that sends more packets ends its bursts sooner in time: the parent of the WIFI repair
# fix resent 2-3 times per lost datagram and its outages were that much shorter. CLOCK_PPS=N gives every node's egress a
# steady N packets a second of its own (16-byte UDP to 10.78.0.9, a static neighbour whose MAC no node has: the bridge
# floods it, every node drops it, nothing answers), through the same unicast band and its netem, so that a burst of B
# packets lasts about B / N seconds whatever the node's own traffic - Wi-Fi's bursts are in time. The reading it is for
# is in wifi_repair_ab.sh's header (the clock control). The clock packets count in R2's R; each node's sender prints
# how many it sent (clock.log), and a run whose sender sent under 90% of N x its duration is VOID for that reading.
#
# Usage: [REPS=3] [REP_FIRST=1] [DUR=30] [ARMS="ctl ge1b2 ..."] [CELLS="be_small rel_small rel_large"] [PROFILE=ethernet|wifi]
#        [OUT=dir] wifi_burst_loss_netns.sh
#   LARGE_HZ (default 5) sets rel_large's rate. OUT defaults to ~/rig_results_safe/wifi_baseline/<stamp>: raw logs per run
#   under OUT/runs/, the summary in
#   OUT/summary.txt, which always ends with "=== wifi_burst_loss_netns ended". READ_ONLY=1 re-reads an existing OUT.
#   Needs `sudo -n ip` (tc runs under `ip netns exec`). Takes ~1 min a rep of a cell: 9 arms x 3 cells x 3 reps ~ 80 min.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
REPS=${REPS:-3}
# REP_FIRST: the first rep number, so more reps can be added to an existing OUT (one whose reps were run under PC load).
REP_FIRST=${REP_FIRST:-1}
DUR=${DUR:-30}
ARMS=${ARMS:-"ctl ge1b2 ge1b8 ge5b2 ge5b4 ge5b8 ge10b4 bc tv"}
CELLS=${CELLS:-"be_small rel_small rel_large"}
PROFILE=${PROFILE:-ethernet}
PHASE_S=${PHASE_S:-5}
SMALL_HZ=100
SMALL_BYTES=100
LARGE_BYTES=1048576
LARGE_HZ=${LARGE_HZ:-5}
OUT=${OUT:-$HOME/rig_results_safe/wifi_baseline/$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT/runs" || exit 1
SUM="$OUT/summary.txt"
say() { echo "$*" | tee -a "$SUM"; }

if [ "${READ_ONLY:-0}" = 1 ]; then
    python3 "$HERE/wifi_burst_read.py" "$OUT"
    exit $?
fi
[ "$REP_FIRST" = 1 ] && : >"$SUM"

case "$PROFILE" in ethernet | wifi) ;; *) echo "PROFILE must be ethernet or wifi" >&2; exit 2 ;; esac
# The large bench takes the profile only as -P wifi (header, AMENDED 2026-10-23); ethernet runs it as the baseline did.
LARGE_PROFILE=()
[ "$PROFILE" = wifi ] && LARGE_PROFILE=(-P wifi)
WORK=$(mktemp -d "$HOME/tmp_build/wifi_burst.XXXXXX") || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
TV_PID=""
on_exit() {
    local rc=$? ns
    [ -n "$TV_PID" ] && kill "$TV_PID" 2>/dev/null
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== wifi_burst_loss_netns ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
TREE_SHA=$(git -C "$REPO" rev-parse --short HEAD)
DIRTY=$(git -C "$REPO" status --porcelain -- src include platform | wc -l)
say "=== wifi_burst_loss_netns $(date -Is): tree=$TREE_SHA dirty_paths=$DIRTY PROFILE=$PROFILE REPS=$REPS DUR=$DUR" \
    "ARMS='$ARMS' CELLS='$CELLS' small=${SMALL_BYTES}B@${SMALL_HZ}Hz large=${LARGE_BYTES}B@${LARGE_HZ}Hz ==="
{
    echo "tree=$TREE_SHA dirty_paths=$DIRTY profile=$PROFILE reps=$REPS rep_first=$REP_FIRST dur=$DUR phase_s=$PHASE_S"
    echo "netem_base='${NETEM_BASE:-limit 10000 delay 2ms slot 1ms 4ms packets 42}' node_env='${NODE_ENV:-}'"
    echo "small_bytes=$SMALL_BYTES small_hz=$SMALL_HZ large_bytes=$LARGE_BYTES large_hz=$LARGE_HZ nproc=$(nproc)"
    echo "build_small=cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS (core defaults: 1472-byte datagram)"
    echo "build_large=cc -O2 -DNDEBUG -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0" \
        "rstats=${LARGE_RSTATS:-0} large_pub_args='${LARGE_PUB_ARGS:-}' large_sub_args='${LARGE_SUB_ARGS:-}'" \
        "bands=${BANDS:-2} clock_pps=${CLOCK_PPS:-0}"
} >>"$OUT/provenance.txt"

# --- build: both binaries from this tree, as hybrid_send_netns.sh builds its bench ----------------------------------
CORE=("$REPO/src/tickle.c" "$REPO/src/encoding.c" "$REPO/src/log.c" "$REPO/src/hal_linux.c")
export TMPDIR=$HOME/tmp_build
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS -I"$REPO/include" -I"$REPO/src" \
    -I"$REPO/examples/perf_hil/tickle/common" -o "$WORK/node" "$HERE/wifi_link_node.c" "${CORE[@]}" -lpthread -lm \
    >"$WORK/build.log" 2>&1; then
    say "VOID  wifi_link_node did not build"
    tail -5 "$WORK/build.log"
    exit 1
fi
LARGE_STATS=()
[ "${LARGE_RSTATS:-0}" = 1 ] && LARGE_STATS=(-Dtt_RELIABLE_STATS)
# LARGE_TRACE=1 (the R4 investigation, 2026-10-25; a tree carrying src/tickle.c's R4_TRACE hooks): the large bench
# records core's repair events and each sample's times (r4_trace_read.py reads them); default off, nothing changes.
[ "${LARGE_TRACE:-0}" = 1 ] && LARGE_STATS+=(-DR4_TRACE)
# shellcheck disable=SC2068 # an empty array expands to nothing
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    ${LARGE_STATS[@]} -I"$REPO/include" -I"$REPO/src" -o "$WORK/large" "$HERE/large_sample_bench.c" "${CORE[@]}" -lpthread -lm \
    >>"$WORK/build.log" 2>&1; then
    say "VOID  large_sample_bench did not build"
    tail -5 "$WORK/build.log"
    exit 1
fi

# --- shaping -------------------------------------------------------------------------------------------------------
BASE=${NETEM_BASE:-"limit 10000 delay 2ms slot 1ms 4ms packets 42"}
# ge L B: netem's gemodel arguments for average loss L% and mean burst B packets.
ge() { awk -v L="$1" -v B="$2" 'BEGIN { r = 100 / B; p = L * r / (100 - L); printf "loss gemodel %.4f%% %.4f%%", p, r }'; }
# spec ARM BAND [PHASE]: the netem arguments of one band (ucast|bcast) in one arm (tv: in one phase).
spec() {
    local arm=$1 band=$2
    case "$arm" in
    ctl) echo "$BASE" ;;
    ge*b*)
        local l=${arm#ge}
        echo "$BASE $(ge "${l%b*}" "${l#*b}")"
        ;;
    bc) if [ "$band" = bcast ]; then echo "$BASE $(ge 30 3)"; else echo "$BASE $(ge 1 2)"; fi ;;
    tv0) echo "$BASE" ;;
    tv1) echo "$BASE $(ge 5 4)" ;;
    tv2) echo "limit 10000 delay 22ms slot 1ms 4ms packets 42 $(ge 10 8)" ;;
    *) return 1 ;;
    esac
}
NODES=(p e i)
# The clock sender (CLOCK_PPS, header): N 16-byte datagrams a second to 10.78.0.9 until the run's stop file appears
# (checked every 100 datagrams) or its duration ends; prints "clock sent=<n> secs=<s>" on the way out.
CLOCK_PY='
import os, socket, sys, time
pps, dur, stop = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
t0 = time.monotonic()
n = 0
while time.monotonic() - t0 < dur and (n % 100 or not os.path.exists(stop)):
    try:
        s.sendto(b"0123456789abcdef", ("10.78.0.9", 9))
    except OSError:
        pass
    n += 1
    wait = t0 + n / pps - time.monotonic()
    if wait > 0:
        time.sleep(wait)
print(f"clock sent={n} secs={time.monotonic() - t0:.3f} pps={pps:g}", flush=True)
'

IPS=(10.78.0.1 10.78.0.2 10.78.0.3)
BCAST=10.78.0.255

# topo RUN: the hub and the three node namespaces of one run, each node's egress shaped for $ARM (tv: phase 0).
topo() {
    local run=$1 k ns
    HUB="wf$$-$run-h"
    echo "$HUB" >>"$NS_LIST"
    sudo -n ip netns add "$HUB" || return 1
    sudo -n ip -n "$HUB" link add br0 type bridge || return 1
    sudo -n ip -n "$HUB" link set br0 up
    for k in 0 1 2; do
        ns="wf$$-$run-${NODES[$k]}"
        echo "$ns" >>"$NS_LIST"
        NS[k]=$ns
        sudo -n ip netns add "$ns" || return 1
        sudo -n ip -n "$HUB" link add "hp$k" type veth peer name wl0 netns "$ns" || return 1
        sudo -n ip -n "$HUB" link set "hp$k" master br0 && sudo -n ip -n "$HUB" link set "hp$k" up || return 1
        sudo -n ip -n "$ns" addr add "${IPS[$k]}/24" brd + dev wl0 || return 1
        sudo -n ip -n "$ns" link set lo up && sudo -n ip -n "$ns" link set wl0 up || return 1
        shape "$ns" add || return 1
    done
}
# shape NS add|change [ARM]: the prio root, its broadcast filters and both bands' netem.
shape() {
    local ns=$1 how=$2 arm=${3:-$SHAPE_ARM}
    local t=(sudo -n ip netns exec "$ns" tc)
    if [ "$how" = add ]; then
        "${t[@]}" qdisc add dev wl0 root handle 1: prio bands 2 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 || return 1
        # shellcheck disable=SC2046 # the netem arguments are words on purpose
        "${t[@]}" qdisc add dev wl0 parent 1:1 handle 10: netem $(spec "$arm" bcast) || return 1
        # shellcheck disable=SC2046
        "${t[@]}" qdisc add dev wl0 parent 1:2 handle 20: netem $(spec "$arm" ucast) || return 1
        # BANDS=1 (a diagnostic, 2026-10-23): no broadcast filters, so broadcast shares the unicast band and its queue.
        [ "${BANDS:-2}" = 1 ] && return 0
        "${t[@]}" filter add dev wl0 parent 1: protocol ip prio 1 u32 match ip dst "$BCAST/32" flowid 1:1 || return 1
        "${t[@]}" filter add dev wl0 parent 1: protocol ip prio 1 u32 match ip dst 255.255.255.255/32 flowid 1:1 ||
            return 1
    else
        # shellcheck disable=SC2046
        "${t[@]}" qdisc change dev wl0 parent 1:1 handle 10: netem $(spec "$arm" bcast) || return 1
        # shellcheck disable=SC2046
        "${t[@]}" qdisc change dev wl0 parent 1:2 handle 20: netem $(spec "$arm" ucast) || return 1
    fi
}
# The tv arm's phase changer, a child of this shell (its PID from $!, killed by the trap if still running).
tv_cycle() {
    local phase=0 k
    while :; do
        sleep "$PHASE_S"
        phase=$(((phase + 1) % 3))
        for k in 0 1 2; do shape "${NS[$k]}" change "tv$phase"; done
        echo "$(date +%s.%N) tv phase $phase" >>"$RUN_DIR/tv_phases.txt"
    done
}
counters() { # counters NS LABEL -> RUN_DIR/counters.txt: the veth and both bands, as the kernel counted them
    local ns=$1 label=$2
    {
        echo "== $label tx_bytes=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_bytes)" \
            "tx_packets=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_packets)" \
            "tx_dropped=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_dropped)" \
            "rx_dropped=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/rx_dropped)"
        sudo -n ip netns exec "$ns" tc -s qdisc show dev wl0
    } >>"$RUN_DIR/counters.txt"
}
# node K ARGS...: one wifi_link_node or large bench in node namespace K, memory capped, output to RUN_DIR.
inns() {
    local k=$1
    shift
    # shellcheck disable=SC2024 # the logs are this user's, written by this shell
    # NODE_ENV (e.g. TT_UDP_OFFLOAD=0) reaches the node through env, which sudo's env_reset would otherwise drop.
    # shellcheck disable=SC2086 # NODE_ENV is words on purpose
    sudo -n ip netns exec "${NS[$k]}" env ${NODE_ENV:-} bash -c 'ulimit -v 2000000; exec "$@"' inns "$@"
}

run_one() { # run_one ARM CELL REP
    local arm=$1 cell=$2 rep=$3 run="$1-$2-$3" k
    RUN_DIR="$OUT/runs/$run"
    mkdir -p "$RUN_DIR"
    SHAPE_ARM=$arm
    [ "$arm" = tv ] && SHAPE_ARM=tv0
    echo "load=$(cut -d' ' -f1 /proc/loadavg) nproc=$(nproc) start=$(date -Is)" >"$RUN_DIR/meta.txt"
    if ! topo "$run" >"$RUN_DIR/topo.log" 2>&1; then
        say "VOID  $run: no topology (see $RUN_DIR/topo.log)"
        return 1
    fi
    local stop="$WORK/stop-$run" mode=be
    rm -f "$stop"
    local clock_pids=()
    if [ "${CLOCK_PPS:-0}" -gt 0 ]; then
        for k in 0 1 2; do
            sudo -n ip -n "${NS[$k]}" neigh replace 10.78.0.9 lladdr 02:00:00:00:00:09 dev wl0 nud permanent
            inns "$k" python3 -I -c "$CLOCK_PY" "$CLOCK_PPS" $((DUR + 70)) "$stop" >>"$RUN_DIR/clock.log" 2>&1 &
            clock_pids+=($!)
        done
    fi
    [ "$cell" != be_small ] && mode=rel
    local common=(-b "$BCAST" -p "$PROFILE" -q "$stop")
    inns 2 "$WORK/node" idle -c 3 -d $((DUR + 60)) "${common[@]}" >"$RUN_DIR/idle.log" 2>&1 &
    local idle_pid=$!
    TV_PID=""
    if [ "$cell" = rel_large ]; then
        # shellcheck disable=SC2086 # LARGE_SUB_ARGS / LARGE_PUB_ARGS are words on purpose
        local node_env=${NODE_ENV:-}
        [ "${LARGE_TRACE:-0}" = 1 ] && NODE_ENV="$node_env R4_TRACE_FILE=$RUN_DIR/sub.trace"
        inns 1 "$WORK/large" sub -s "$LARGE_BYTES" -d $((DUR + 8)) -R -b "$BCAST" "${LARGE_PROFILE[@]}" ${LARGE_SUB_ARGS:-} >"$RUN_DIR/echo.log" 2>&1 &
        local echo_pid=$!
        sleep 1
        [ "$arm" = tv ] && { tv_cycle & TV_PID=$!; }
        [ "${LARGE_TRACE:-0}" = 1 ] && NODE_ENV="$node_env R4_TRACE_FILE=$RUN_DIR/pub.trace"
        # shellcheck disable=SC2086
        inns 0 "$WORK/large" pub -s "$LARGE_BYTES" -r "$LARGE_HZ" -d "$DUR" -R -b "$BCAST" "${LARGE_PROFILE[@]}" ${LARGE_PUB_ARGS:-} >"$RUN_DIR/ping.log" 2>&1
        wait "$echo_pid"
        NODE_ENV=$node_env
    else
        inns 1 "$WORK/node" echo -c 2 -d $((DUR + 60)) -m "$mode" -s "$SMALL_BYTES" -r "$SMALL_HZ" -l "$RUN_DIR/owd_us.txt" \
            "${common[@]}" >"$RUN_DIR/echo.log" 2>&1 &
        local echo_pid=$!
        sleep 1
        [ "$arm" = tv ] && { tv_cycle & TV_PID=$!; }
        inns 0 "$WORK/node" ping -c 1 -d "$DUR" -m "$mode" -s "$SMALL_BYTES" -r "$SMALL_HZ" -l "$RUN_DIR/rtt_us.txt" \
            "${common[@]}" >"$RUN_DIR/ping.log" 2>&1
        sleep 1
        touch "$stop"
        wait "$echo_pid"
    fi
    if [ -n "$TV_PID" ]; then
        kill "$TV_PID" 2>/dev/null
        wait "$TV_PID" 2>/dev/null
        TV_PID=""
    fi
    sleep 1
    touch "$stop"
    wait "$idle_pid"
    for k in "${clock_pids[@]}"; do wait "$k"; done # each ends on the stop file, at the latest after DUR + 70 s
    for k in 0 1 2; do counters "${NS[$k]}" "${NODES[$k]}"; done
    for k in 0 1 2; do sudo -n ip netns del "${NS[$k]}"; done
    sudo -n ip netns del "$HUB"
    local line
    line=$(grep -h '^RESULT' "$RUN_DIR/ping.log" "$RUN_DIR/echo.log" | tr '\n' ' ' | cut -c1-400)
    say "$run: ${line:-<no RESULT line>}"
}

declare -a NS
for rep in $(seq "$REP_FIRST" $((REP_FIRST + REPS - 1))); do
    for cell in $CELLS; do
        for arm in $ARMS; do
            run_one "$arm" "$cell" "$rep"
        done
    done
done
say "--- reading (wifi_burst_read.py)"
python3 "$HERE/wifi_burst_read.py" "$OUT" | tee -a "$SUM"

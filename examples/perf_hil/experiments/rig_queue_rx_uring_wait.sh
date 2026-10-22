#!/usr/bin/env bash
# rig_queue_rx_uring_wait.sh - the rig A/B of the wait ring (hal_linux.c, "The wait ring", branch ab/rx-uring-dt),
# written 2026-10-22 before any of its runs. PREPARED, NOT RUN: launching it is the lead's decision.
#
# The question: does the ring take the receive gain recv_wake_xhost.c measured between its prod and prod_dt arms
# (+1.28 us p50, -0.75 us CPU a datagram, Pi to Pi) into production, and cost nothing anywhere else?
#
# Three stages, in this order, each its own rig lock (the harnesses take it themselves):
#   G  core gate v2: rig_queue_core_ab.sh A=<branch base on main> B=<branch head> BRANCH=ab/rx-uring-dt, its default
#      cells (reliable and best-effort throughput p1-p4, reliable latency p1-p4, N0), mirrored blocks A P B B P A.
#      Its reader (core_ab_read.py) is the rule: PASS = no CPU witness row and no latency row WORSE.
#   R  rmw BEST_EFFORT, the wake split of RESULTS KH (rows 48c-51c' rig, rmw_holdover_rig.sh part K): rmw_crosshost_rtt.sh
#      at the branch head, RMW_LIST=rmw_tickle, TICKLE_VARIANTS="default ringoff libc libc_ringoff" (default and libc:
#      the ring on, the branch's tt_RX_URING_WAIT 1; ringoff: -Dtt_RX_URING_WAIT=0, today's path), WAITS=block
#      MSGS=bench QOSES=best_effort, CAPTURE=1 STAMPS=1 SYSSTAMP_ARMS="off on", ROTATE=1, REPS=5. Read by
#      rmw_pcap_split.py's ring summary (its RING0-RING3, the code is the rule):
#        RING0 samples: an arm with fewer than 3 reps of >= 50 matched samples is NOT READ.
#        RING1 control: the two libc arms' median wire p50s within 10 us, else NOT READ.
#        RING2 the receive return, "pong tap->recv" per-rep p50 (the ring arm's is the io_uring_enter that returned the
#              datagram, sysstamp records it): libc against libc_ringoff by the range rule and the median difference.
#              IMPROVED: FASTER and the median at least 0.5 us lower.
#        RING3 the round trip without the instrument (sysstamp off, the raw-call builds default against ringoff): by the
#              range rule, not SLOWER.
#      PASS = RING2 IMPROVED and RING3 not SLOWER.
#   M  a multi-threaded executor: the same with PONG_THREADS=2 (pong_node --executor-threads 2, rclcpp's
#      MultiThreadedExecutor: two threads take the poll role in turn, the case the ring may move between them in),
#      TICKLE_VARIANTS="default ringoff", no capture, REPS=5. PASS = RING3 not SLOWER (the RTT, default against
#      ringoff), read by the same summary.
# LAND iff G PASS, R PASS and M PASS. Anything else: the branch does not land (the ring stays off by default).
#
# ESTIMATE: G ~105 min (the rx-gap3c stage's); R: 4 variant builds on both Pis ~25 min + 5 reps x 4 arms x 2 sysstamp
# arms x ~40 s ~ 27 min; M ~15 min; ~2 h 50 min in all. Analyse an overrun at 4 h 15 min.
#
# Before it can run: the branch pushed (the Pis fetch it), and the harness changes it needs on that branch -
# rmw_crosshost_rtt.sh's ringoff / libc_ringoff variants and PONG_THREADS, pong_node --executor-threads, sysstamp's
# io_uring_enter record and rmw_pcap_split.py's ring summary (all in the branch's commit "perf_hil: the wait ring's
# rig A/B").
#
# Usage: SHA=<branch head> BASE=<main commit it is on> setsid nohup examples/perf_hil/experiments/rig_queue_rx_uring_wait.sh \
#            > ~/rig_results_safe/rx_uring_wait_rig_<date>.log 2>&1 < /dev/null &
#        STAGES="G R M" (default all). The last line is always "RX_URING_WAIT_RIG_END rc=<n> verdict=<...>".
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAFE=$HOME/rig_results_safe
STAGES=${STAGES:-G R M}
SHA=${SHA:?SHA=<the branch head, pushed>}
BASE=${BASE:?BASE=<the main commit the branch is on>}
BRANCH=${BRANCH:-ab/rx-uring-dt}
STAMP=$(date +%Y%m%d-%H%M%S)
G_RC=skip
R_VERDICT=skip
M_VERDICT=skip
finish() { echo "RX_URING_WAIT_RIG_END rc=$1 verdict=$2 G=$G_RC R=$R_VERDICT M=$M_VERDICT $(date -Is)"; }
trap 'finish $? "INCOMPLETE"' EXIT
say() { echo "$(date -Is) $*"; }
say "=== wait ring rig A/B: SHA $SHA BASE $BASE BRANCH $BRANCH stages '$STAGES' ==="
git -C "$HERE" fetch -q origin "$BRANCH" || { say "REFUSED: cannot fetch origin/$BRANCH"; exit 1; }
git -C "$HERE" merge-base --is-ancestor "$SHA" "origin/$BRANCH" || { say "REFUSED: $SHA is not on origin/$BRANCH"; exit 1; }
git -C "$HERE" merge-base --is-ancestor "$BASE" "$SHA" || { say "REFUSED: $BASE is not an ancestor of $SHA"; exit 1; }
git -C "$HERE" show "$SHA:include/tickle/hal_linux.h" | grep -q '^#define tt_RX_URING_WAIT 1$' ||
    { say "REFUSED: tt_RX_URING_WAIT is not 1 at $SHA - the default arm would not have the ring"; exit 1; }
git -C "$HERE" show "$SHA:examples/perf_hil/experiments/rmw_crosshost_rtt.sh" | grep -q 'libc_ringoff)' ||
    { say "REFUSED: rmw_crosshost_rtt.sh at $SHA has no ringoff variants"; exit 1; }
git -C "$HERE" show "$SHA:examples/perf_hil/experiments/rmw_pcap_split.py" | grep -q 'def ring_summary' ||
    { say "REFUSED: rmw_pcap_split.py at $SHA has no ring summary"; exit 1; }

# One rmw stage: rmw_crosshost_rtt.sh at SHA, then its reader. $1 name, the rest the environment.
rmw_stage() {
    local name=$1 out="$SAFE/rx_uring_wait_${1}_${STAMP}"
    shift
    say "--- stage $name: rmw_crosshost_rtt.sh $* -> $out.txt ---"
    env SHA="$SHA" RMW_LIST=rmw_tickle WAITS=block MSGS=bench QOSES=best_effort ROTATE=1 REPS=5 OUT="$out.txt" "$@" \
        "$HERE/rmw_crosshost_rtt.sh" >"$out.log" 2>&1
    say "    harness rc=$? ($out.log)"
    python3 "$HERE/rmw_pcap_split.py" "$out.txt" >"$out.split.txt" 2>&1
    grep -E '^ *RING[0-3]|^RING_VERDICT' "$out.split.txt"
}

for stage in $STAGES; do
    case $stage in
    G)
        say "--- stage G: core gate v2 ---"
        A=$BASE B=$SHA BRANCH=$BRANCH "$HERE/rig_queue_core_ab.sh" >"$SAFE/rx_uring_wait_G_${STAMP}.log" 2>&1
        G_RC=$?
        say "    rig_queue_core_ab.sh rc=$G_RC (0 PASS, 2 FAIL, 3 VOID; $SAFE/rx_uring_wait_G_${STAMP}.log)"
        ;;
    R)
        R_VERDICT=$(rmw_stage R TICKLE_VARIANTS="default ringoff libc libc_ringoff" CAPTURE=1 STAMPS=1 \
            SYSSTAMP_ARMS="off on" | tee /dev/stderr | sed -n 's/^RING_VERDICT \([A-Z_]*\).*/\1/p' | tail -1)
        R_VERDICT=${R_VERDICT:-NOT_READ}
        ;;
    M)
        M_VERDICT=$(rmw_stage M TICKLE_VARIANTS="default ringoff" PONG_THREADS=2 | tee /dev/stderr |
            sed -n 's/^RING_RTT_VERDICT \([A-Z_]*\).*/\1/p' | tail -1)
        M_VERDICT=${M_VERDICT:-NOT_READ}
        ;;
    *) say "REFUSED: unknown stage $stage"; exit 64 ;;
    esac
done
verdict=NO_LAND
[ "$G_RC" = 0 ] && [ "$R_VERDICT" = PASS ] && [ "$M_VERDICT" = PASS ] && verdict=LAND
trap - EXIT
finish 0 "$verdict"

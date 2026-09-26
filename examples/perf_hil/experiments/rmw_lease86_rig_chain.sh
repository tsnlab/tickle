#!/usr/bin/env bash
# RMW_PERF_PLAN.md 8.6's rig A/B (2026-09-27): the ping-side lease that follows the caller's cadence (NEW) against
# its parent (PARENT), rmw_tickle only, both Pis rebuilt per block. Blocks run A B B A (3 repetitions each, 6 per
# arm) so a drift over the session lands on both arms alike; each block takes the rig lock itself.
# Every row: block mode and the poll control at 100 us, bench and array1k, BEST_EFFORT and RELIABLE, the pong idle
# 10 s before the ping (IDLE_S=10: pong_idle_cpu_ns over 9 s) - the ping's log keeps its shutdown line with
# delivered_by_poll_thread= and park_wakes= (NEW only).
# Reading, pre-registered in RMW_PERF_PLAN 8.6: block RTT median falls beyond 2 x SE in every cell; idle CPU, pong
# whole-run CPU and every poll row do not move beyond 2 x SE; any criterion failing, the change is reverted.
cd /home/semih/tickle || exit 1
PARENT=${PARENT:?PARENT=<sha>}; NEW=${NEW:?NEW=<sha>}
export RIG_LOCK_WAIT=36000
X=./examples/perf_hil/experiments
n=0
for sha in "$PARENT" "$NEW" "$NEW" "$PARENT"; do
    n=$((n + 1))
    SHA=$sha RMW_LIST=rmw_tickle WAITS='block poll' POLL_SLEEPS=100 MSGS='bench array1k' REPS=3 IDLE_S=10 \
        OUT=/tmp/rmw86_b${n}_${sha:0:8}.txt $X/rmw_crosshost_rtt.sh > "/tmp/rmw86_b${n}_${sha:0:8}.log" 2>&1
done
echo "=== chain done $(date -Is) ===" > /tmp/rmw86_rig_chain.done

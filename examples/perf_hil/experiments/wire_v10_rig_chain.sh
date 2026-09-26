#!/usr/bin/env bash
# WIRE_PLAN.md section 6's no-regression campaign on the rig (2026-09-27): the v10 bundle against its parent, TickLE
# only (the vendors do not change), each session taking the rig lock itself.
#   1. the native campaign cells (A/T rows), 5 repetitions, parent then v10
#   2. the rmw rows, block and the poll sweep (V/P rows), 3 repetitions, parent then v10
#   3. an rmw capture per build, for the rmw bytes per sample
# Reading, pre-registered in WIRE_PLAN 6 / 6.1: nothing moves beyond 2 x SE in the wrong direction; the report lists
# every row that moved and every row that only held.
cd /home/semih/tickle || exit 1
PARENT=${PARENT:?PARENT=<sha>}; V10=${V10:?V10=<sha>}
export RIG_LOCK_WAIT=36000
X=./examples/perf_hil/experiments
for sha in "$PARENT" "$V10"; do
    SHA=$sha FWS=tickle REPS=5 OUT=/tmp/wire_v10_campaign_${sha:0:8}.txt $X/campaign_sweep.sh > "/tmp/wire_v10_campaign_${sha:0:8}.log" 2>&1
done
for sha in "$PARENT" "$V10"; do
    SHA=$sha RMW_LIST=rmw_tickle WAITS='poll block' POLL_SLEEPS='0 50 100 200' MSGS='bench array1k' REPS=3 \
        OUT=/tmp/wire_v10_rmw_${sha:0:8}.txt $X/rmw_crosshost_rtt.sh > "/tmp/wire_v10_rmw_${sha:0:8}.log" 2>&1
done
for sha in "$PARENT" "$V10"; do
    SHA=$sha RMW_LIST=rmw_tickle WAITS=block CAPTURE=1 MSGS=bench REPS=1 \
        OUT=/tmp/wire_v10_cap_${sha:0:8}.txt $X/rmw_crosshost_rtt.sh > "/tmp/wire_v10_cap_${sha:0:8}.log" 2>&1
done
echo "=== chain done $(date -Is) ===" > /tmp/wire_v10_rig_chain.done

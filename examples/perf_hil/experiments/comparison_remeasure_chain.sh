#!/usr/bin/env bash
# COMPARISON.md re-measured on one build (2026-09-27, after v10, executor-driven receive, D1-D5 and the liveliness
# work): the three frameworks in the same sessions, each step taking the rig lock itself.
#   1. the native campaign, all 12 cells (the A rows), FastDDS as shipped
#   2. the P3/P4 cells again with FastDDS's maxMessageSize 1472 (the T rows, the user's fair-evaluation setting)
#   3. the rmw block rows with RSS and CPU (the V rows, 48-51 and 68-71), with both sides' /proc maps checked
# Reading: campaign_summary.py's verdict rules for 1-2 (unchanged since the A/T rows), the V rows as before
# (lowest mean, 2 x SE for a win). Poll rows 52-67 were re-measured the same day (J) and are not repeated.
cd /home/semih/tickle || exit 1
export SHA=${SHA:?SHA=<commit>}; TAG=${TAG:-cmp}
export RIG_LOCK_WAIT=10800
X=./examples/perf_hil/experiments
REPS=3 OUT=/tmp/${TAG}_A_${SHA:0:8}.txt $X/campaign_sweep.sh > "/tmp/${TAG}_A_${SHA:0:8}.log" 2>&1
REPS=3 CELLS="3 4 6" FASTDDS_PROFILE=fastdds_eth0_only_mms1472.xml OUT=/tmp/${TAG}_T_${SHA:0:8}.txt \
    $X/campaign_sweep.sh > "/tmp/${TAG}_T_${SHA:0:8}.log" 2>&1
WAITS=block MSGS="bench array1k" REPS=3 OUT=/tmp/${TAG}_V_${SHA:0:8}.txt \
    $X/rmw_crosshost_rtt.sh > "/tmp/${TAG}_V_${SHA:0:8}.log" 2>&1
echo "=== chain done $(date -Is) ===" > "/tmp/${TAG}_chain.done"

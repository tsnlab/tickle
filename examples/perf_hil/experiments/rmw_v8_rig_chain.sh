#!/usr/bin/env bash
# The rig after the discovery summary change (2026-09-26): scored rmw rows on a v8+ core, then DISCOVERY_PLAN M6 -
# rmw non-data traffic from captures, to compare with the v7 capture session (~390 B/s for rmw_tickle).
# Both sessions take the rig lock themselves (rmw_crosshost_rtt.sh).
cd /home/semih/tickle || exit 1
export RIG_LOCK_WAIT=36000
SHA=${SHA:-ec82de1fcbae4bad3c52946af8eaf1ab8c2b883c}
SHA=$SHA WAITS='poll block' MSGS='bench array1k' REPS=3 OUT=/tmp/rmw_scored3_2026-09-26.txt \
    ./examples/perf_hil/experiments/rmw_crosshost_rtt.sh > /tmp/rmw_scored3.log 2>&1
SHA=$SHA WAITS=block CAPTURE=1 MSGS=bench REPS=1 OUT=/tmp/rmw_m6_2026-09-26.txt \
    ./examples/perf_hil/experiments/rmw_crosshost_rtt.sh > /tmp/rmw_m6.log 2>&1
echo "=== chain done ===" > /tmp/rmw_v8_rig_chain.done

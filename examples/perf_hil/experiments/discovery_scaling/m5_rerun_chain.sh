#!/usr/bin/env bash
# M5 re-run (DISCOVERY_PLAN section 7 follow-up), 2026-09-26: v7, the first v8, and v8 with the request retry and
# the liveliness silence fix, same tool (completeness sampled 2 s before exit), 5 repetitions of 8 nodes each.
# Pre-registered (Dev's expectation, written before the run): after b90a047d no dips, and every node complete by
# ~1.0 s plus a few x 10 ms; the first v8 is expected to reproduce the 2 s straggler/dips at some rate.
cd /home/semih/tickle || exit 1
S=examples/perf_hil/experiments/discovery_scaling/discovery_join.sh
for sha in 41fa005f063ac765ca99fece566a0e602e9c1eea d732139b030810286d8a38c30ae6bf56879911bc "$(git rev-parse b90a047d)"; do
    SHA=$sha OUT=/tmp/discovery_M5r_${sha:0:8}_2026-09-26.txt $S loss 8 32 5
done
echo "=== chain done ===" > /tmp/discovery_m5r_chain.done

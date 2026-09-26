#!/usr/bin/env bash
cd /home/semih/tickle || exit 1
S=examples/perf_hil/experiments/discovery_scaling/discovery_join.sh
for sha in 41fa005f063ac765ca99fece566a0e602e9c1eea d732139b030810286d8a38c30ae6bf56879911bc; do
    for e in 4 32; do
        SHA=$sha OUT=/tmp/discovery_M3_${sha:0:8}_E${e}_2026-09-26.txt $S join 8 $e 10
    done
    SHA=$sha OUT=/tmp/discovery_M5_${sha:0:8}_2026-09-26.txt $S loss 8 32 3
done
echo "=== chain done ===" > /tmp/discovery_m3m5_chain.done

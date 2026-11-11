#!/usr/bin/env bash
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
D=${RESULTS:-$HOME/rig_results_safe}/t1_seed
mkdir -p "$D"
for r in 1 2; do
  mkdir -p "$D/r$r"
  SEED=7 TRACE="$D/r$r/wm.trace" PROFILES="ug:0.36:733,2.2,0.01,2.1,1.3,30.5" CELL=be REPS=1 DUR=60 OUTDIR="t1_seed/r$r" "$S/t1_ge2.sh"
done
echo T1_SEED_END

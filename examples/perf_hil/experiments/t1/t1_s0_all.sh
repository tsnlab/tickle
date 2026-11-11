#!/usr/bin/env bash
# Stage 0 of RANGE_NACK_DESIGN_2026-11-11, all three sweeps in sequence (calls t1_s0.sh). Simulation only.
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CTL="ctl:0.0:1000000,1,0.0"
P3="ug:0.36:733,2.2,0.01,2.1,1.3,30.5 psu_cs:0.44:504,28,0.01,9.7,1.0,46.5 o60:0.9:1140,60,0.01"
MODE=small CELLS=rel PROFILES="$CTL $P3" OUTDIR=range_nack_s0_rel "$S/t1_s0.sh"
MODE=large CELLS=rel REPS=2 DUR=60 PROFILES="psu_cs:0.44:504,28,0.01,9.7,1.0,46.5 o60:0.9:1140,60,0.01" OUTDIR=range_nack_s0_large "$S/t1_s0.sh"
MODE=small CELLS=be PROFILES="$P3" OUTDIR=range_nack_s0_be "$S/t1_s0.sh"
echo S0_ALL_DONE

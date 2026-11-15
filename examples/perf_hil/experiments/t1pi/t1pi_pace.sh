#!/usr/bin/env bash
# Drives one block of the T1-Pi pacing plan (PACE_PLAN.md): pace.sh <name> <size-bytes> "<cfgs>" [cells] [fw-extra]. Detached use:
#   setsid nohup ./t1pi_pace.sh ... > log 2>&1 < /dev/null &
# Results in $BASE/<name>; resumable (a finished run has a .done file and is skipped).
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BASE=${BASE:-$HOME/rig_results_safe/t1pi_pace_20261115}
name=$1; size=$2; cfgs=$3; cells=${4:-rel_large}
OUT=$BASE/$name CELLS="$cells" ARMS="ctl o60 o200" ROUNDS=3 COPIES="A A2" ROTATE=5 FWS="$cfgs" LARGE_SZ=$size DUR=60 \
    "$S/t1pi_fw.sh"

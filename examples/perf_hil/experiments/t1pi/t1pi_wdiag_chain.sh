#!/usr/bin/env bash
# Chain of the Wi-Fi diagnosis (WIFI_DIAG_PLAN.md): (c) drip entry, (b) timeline against DDS, (a) capacity curve. Detached use:
#   setsid nohup ./t1pi_wdiag_chain.sh > $BASE/chain.log 2>&1 < /dev/null &
# Each step takes the rig lock itself (flock /tmp/rig.lock + rig_lock.sh, inside the step) and is resumable (finished runs are skipped).
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BASE=${BASE:-$HOME/rig_results_safe/wifi_diag_20261117}
mkdir -p "$BASE"
step() { echo "$(date -Is) CHAIN_STEP $*"; }
export PCAP=1 SAMPLE=1
step c_drip
OUT=$BASE/c_drip CELLS=rel_large ARMS="ctl o200" ROUNDS=${C_ROUNDS:-12} COPIES=A FWS=tickle LARGE_SZ=1048576 DUR=60 ROTATE=1 PCAP_SNAP=96 "$S/t1pi_fw.sh"
step b_timeline_1m
OUT=$BASE/b_1m CELLS=rel_large ARMS="ctl o60 o200" ROUNDS=2 COPIES=A FWS="tickle fastdds cyclonedds" LARGE_SZ=1048576 DUR=60 ROTATE=1 PCAP_SNAP=160 "$S/t1pi_fw.sh"
step b_timeline_128k
OUT=$BASE/b_128k CELLS=rel_large ARMS="ctl o60 o200" ROUNDS=2 COPIES=A FWS="tickle fastdds cyclonedds" LARGE_SZ=131072 DUR=60 ROTATE=1 PCAP_SNAP=160 "$S/t1pi_fw.sh"
unset PCAP SAMPLE
step a_grid
MODE=grid REPS=5 OUT=$BASE/a_grid "$S/t1pi_cap.sh"
step a_fresh
MODE=fresh REPS=5 OUT=$BASE/a_fresh "$S/t1pi_cap.sh"
step a_ctrl_wire
MODE=ctrl VARIANT=wire ARMS=ctl REPS=5 OUT=$BASE/a_ctrl_wire "$S/t1pi_cap.sh"
step a_ctrl_fix54
MODE=ctrl VARIANT=fix54 ARMS=ctl REPS=5 OUT=$BASE/a_ctrl_fix54 "$S/t1pi_cap.sh"
step CHAIN_DONE

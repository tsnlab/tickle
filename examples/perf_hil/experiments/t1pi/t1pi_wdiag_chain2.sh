#!/usr/bin/env bash
# Extras after the first chain (WIFI_DIAG_PLAN.md amendment 1): queue-limit bursts and the multicast-join priming test.
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BASE=${BASE:-$HOME/rig_results_safe/wifi_diag_20261117}
echo "$(date -Is) CHAIN2_STEP big"
MODE=big DUR=6 REPS=5 OUT=$BASE/a_big "$S/t1pi_cap.sh"
echo "$(date -Is) CHAIN2_STEP prime"
MODE=ctrl VARIANT=prime ARMS=ctl REPS=12 OUT=$BASE/a_prime "$S/t1pi_cap.sh"
echo "$(date -Is) CHAIN2_DONE"

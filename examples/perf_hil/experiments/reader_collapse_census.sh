#!/usr/bin/env bash
# reader_collapse_census.sh - does the reader's own retry estimate still collapse on main (RESULTS `RLC`, 2026-10-26)?
#
# BACKGROUND. In the tv storm work (`TVS`) some reps' readers sent 1,300-25,558 timer ACKNACKs against a median ~80,
# a loop separate from the writer storm 8d0d2eed fixed. R4K's K5 found, under candidate K (Karn's rule for large
# writers), that a probe on the watermark is timed from the first request naming its unit, so a repair already on its
# way gives a ~0.2 ms seed that locks the estimate. Main keeps the default rule after the seed (retry <= 3 timed).
# The census of what is on disk (main-core reps, 8d0d2eed / 06d23f21, untraced and traced): COLLAPSE (> 5 x the
# arm's median) in ge5b4 4/130 (up to 3,364), ge5b2 1/140 (1,019, traced: a 35 us seed), tv 0/80, bc 0/30, ge10b4
# 0/34; before the storm fix tv 8/185 (up to 25,558), bc 2/20. ETHERNET amp ge0.1b1 (30 Hz, 1 Gbit/s) 4/15 at 360-637
# against a median ~7.
#
# THIS RUN, on main 81daa880 (core = 8d0d2eed's): WIFI rel_large, arms ge5b4 ge5b2 tv bc, REPS a tree (default 30),
#   T1..T4 = main + r4_trace.patch + k5_add_r_event.py (TRACED, one tree run four times), U = main untraced (this
#   checkout); all five at once (wifi_repair_ab.sh). With them, E1 = the traced tree on ETHERNET's amp cell (ge0.1b1,
#   LARGE_HZ=30, BANDS=1, the followups harness's NETEM_BASE), ETH_REPS reps (default 60).
# SAMPLE SIZE (before the run): ge5b4 at the census's 3.1% over 120 traced reps: 3.7 collapse reps expected,
#   P(none) = 0.023; tv: 0 of 150 here excludes rates over 2.0% ((1-p)^150 = 0.05), 0 of 230 with the census 1.3% -
#   against the pre-fix 4.3%. ETHERNET ge0.1b1 at 4/15: ~16 of 60.
# READING: reader_collapse_read.py (its docstring is the rule set: COLLAPSE / SEVERE, the trace control, hypothesis H
#   "short-sample lock under the default rule" with P1-P3 and its verdict, ETHERNET descriptive only).
# Usage: TRACED=/path/tree [UNTRACED=this checkout] [TAG=rlc1] [REPS=30] [ETH_REPS=60] reader_collapse_census.sh
#   Output: $BASE_OUT (default ~/rlc_results)/<TAG>_<label>/, <TAG>.ab.txt, <TAG>.read.txt; this script's own log ends
#   with "=== reader_collapse_census ended".
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TRACED=${TRACED:?TRACED=tree with the trace hooks}
UNTRACED=${UNTRACED:-$(cd "$HERE/../../.." && pwd)}
TAG=${TAG:-rlc1}
REPS=${REPS:-30}
ETH_REPS=${ETH_REPS:-60}
BASE_OUT=${BASE_OUT:-$HOME/rlc_results}
mkdir -p "$BASE_OUT" || exit 1
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== reader_collapse_census ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
echo "=== reader_collapse_census $(date -Is) tag=$TAG REPS=$REPS ETH_REPS=$ETH_REPS traced=$TRACED" \
    "($(git -C "$TRACED" rev-parse --short HEAD), dirty $(git -C "$TRACED" status --porcelain -- src | wc -l))" \
    "untraced=$UNTRACED ($(git -C "$UNTRACED" rev-parse --short HEAD), dirty $(git -C "$UNTRACED" status --porcelain -- src | wc -l))" \
    "load $(cut -d' ' -f1-3 /proc/loadavg) ==="
env OUT="$BASE_OUT/${TAG}_eth_E1" REPS="$ETH_REPS" DUR=30 PROFILE=ethernet NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1 \
    LARGE_TRACE=1 ARMS="ge0.1b1" CELLS="rel_large" LARGE_HZ=30 BANDS=1 NETEM_BASE="limit 100000 delay 100us rate 1gbit" \
    "$TRACED/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$BASE_OUT/${TAG}_eth_E1.log" 2>&1 &
eth=$!
BASE_OUT=$BASE_OUT TAG=$TAG TREES="T1=$TRACED T2=$TRACED T3=$TRACED T4=$TRACED U=$UNTRACED" PROFILE=wifi REPS=$REPS \
    ARMS="ge5b4 ge5b2 tv bc" CELLS=rel_large LARGE_RSTATS=1 LARGE_TRACE=1 NODE_ENV=TT_UDP_OFFLOAD=0 \
    "$HERE/wifi_repair_ab.sh" >/dev/null 2>&1
echo "wifi done $(date -Is)"
wait "$eth"
echo "ethernet done rc=$? $(date -Is)"
python3 "$HERE/reader_collapse_read.py" "$BASE_OUT" "$TAG" >"$BASE_OUT/$TAG.read.txt" 2>&1
echo "read rc=$?"
grep -E '^(OCCURRENCE|  (wifi|ethernet) |TRACE CONTROL|CONTROL|VERDICT)' "$BASE_OUT/$TAG.read.txt"

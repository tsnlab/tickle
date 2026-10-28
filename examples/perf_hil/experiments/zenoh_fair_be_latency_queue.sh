#!/usr/bin/env bash
# zenoh_fair_be_latency_queue.sh - the two best_effort_latency cells of zenoh_fair_queue.sh, re-run on their own after
# the TickLE, CycloneDDS and Fast DDS best_effort_latency benches became reliable_latency's with BEST_EFFORT
# (tickle/common/LatencyReliability.h, 2026-10-28). In the fair campaign those rows came back
# "VOID(no instrument= field)" for the three, because their best_effort_latency clients were the old uninstrumented
# ones (100 pings, no warm-up); zenoh-pico's already printed the instrumented fields and read ok.
#
# CELLS: L|p1|Q1|N0|best_effort_latency and L|p4|Q1|N0|best_effort_latency, x [tickle cyclonedds fastdds zenohpico]
# x 3 reps, in campaign_sweep.sh's own loop, which rotates the framework order by one every cell and every repetition.
# Everything else is zenoh_fair_queue.sh's: the same argv (-i 0.1 -d 10 between 4096-round-trip edges 1 ms apart,
# each bounded at 20 s), the same instruments, the same gates.
#
# HOW A ROW IS READ, before the run (campaign_sweep.sh's cell verdict implements each):
#   - instrument=ok and window=ok on every role, warm-up and cool-down as asked (or ended by time), core_build=release
#     and sample_path= for TickLE, as for every latency cell;
#   - reliability=best_effort on every line that carries it (each TickLE line, each DDS client), read back from the
#     created endpoints; a row that says reliable, mixed or unreadable, or says nothing, is VOID(qos:...) - it measured
#     something other than BEST_EFFORT under a best-effort label. zenoh-pico's row must say transport=udp_multicast.
#   - Scoring is campaign_summary.py's and zenoh_fair_read.py's, unchanged: zenohpico is a REFERENCE column.
#
# RIG TIME (estimate): 2 cells x 4 frameworks x 3 reps = 24 runs at ~35 s (N0 latency, zenoh_fair_queue.sh's figure)
# = 14 min, + builds ~10 min on the Pis, + zenoh_fair_preflight.sh and rig_preflight.sh ~5 min on this PC first. About
# 30 min (+-25%); 1.5x deadline 45 min, plus however long the rig lock is held by someone else (RIG_LOCK_WAIT=36000,
# up to 10 h, so a queued run waits rather than silently giving up after the lock's 30 min default).
#
# Usage:  DRY_RUN=1 zenoh_fair_be_latency_queue.sh   prints the plan; touches neither the rig nor the lock.
#         SHA=<landed commit on origin> zenoh_fair_be_latency_queue.sh
#           1. zenoh_fair_preflight.sh (PC netns, low rate, the zenoh-pico arm)          - stops the queue on a FAIL
#           2. campaign_sweep.sh (rig_preflight.sh for the other three on this PC, then the rig lock, held throughout)
#           3. campaign_summary.py and zenoh_fair_read.py on the output
#         The last line is always "ZENOH BE LAT QUEUE DONE rc=<n>" (a watcher's end marker), success or not.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TMPDIR=${TMPDIR:-/home/semih/tmp_build} RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
DRY_RUN=${DRY_RUN:-0}
R=/home/semih/zenoh_fair_results
T=$(date +%Y%m%d-%H%M%S)
OUT=${OUT:-$R/zenoh_fair_be_latency_$T.txt}
mkdir -p "$R"
rc=0
trap 'echo "ZENOH BE LAT QUEUE DONE rc=$rc $(date -Is) out=$OUT"' EXIT

specs=("L|p1|Q1|N0|best_effort_latency|" "L|p4|Q1|N0|best_effort_latency|")
MATRIX_SPECS=$(
    IFS=';'
    echo "${specs[*]}"
)
export MATRIX_SPECS
FWS="tickle cyclonedds fastdds zenohpico"
echo "zenoh_fair_be_latency_queue $(date -Is): ${#specs[@]} cells x [$FWS] x 3 reps, DRY_RUN=$DRY_RUN, out $OUT"

if [ "$DRY_RUN" = 1 ]; then
    FWS="$FWS" REPS=3 DRY_RUN=1 OUT="$OUT.dryrun" "$HERE/campaign_sweep.sh"
    rc=$?
    echo "--- the specs rig_preflight.sh would get (PREFLIGHT_FWS: the three it knows) ---"
    FWS="$FWS" PREFLIGHT_CELLS_ONLY=1 "$HERE/campaign_sweep.sh"
    exit "$rc"
fi
[ -n "${SHA:-}" ] || {
    echo "SHA= (the landed commit carrying the instrumented best_effort_latency benches) is required"
    rc=2
    exit 2
}
# The zenoh-pico arm has no rig_preflight.sh path; zenoh_fair_preflight.sh is its preflight, as in zenoh_fair_queue.sh.
if ! OUT="$R/preflight_be_latency_$T" "$HERE/zenoh_fair_preflight.sh"; then
    echo "zenoh_fair_preflight.sh FAILED - not taking the rig"
    rc=3
    exit 3
fi
FWS="$FWS" PREFLIGHT_FWS="tickle cyclonedds fastdds" PF_JOBS=${PF_JOBS:-1} SHA="$SHA" REPS=3 OUT="$OUT" \
    "$HERE/campaign_sweep.sh"
rc=$?
echo "campaign rc=$rc $(date -Is)"
python3 "$HERE/campaign_summary.py" "$OUT" >"$OUT.summary" 2>&1
python3 "$HERE/zenoh_fair_read.py" "$OUT" >"$OUT.zenoh_fair" 2>&1
tail -3 "$OUT.summary"
exit "$rc"

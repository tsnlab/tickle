#!/usr/bin/env bash
# zenoh_fair_queue.sh - zenoh-pico against TickLE core, CycloneDDS and Fast DDS, as fairly as the four allow, on the
# rig's two Pis in ONE campaign_sweep.sh session (the user, 2026-10-28: "Zenoh를 최대한 공정하게 비교해보자").
#
# WHAT IS THE SAME FOR ALL FOUR. The same two Pis and link; the same netem on the client Pi's eth0 egress (loss 0,
# 0.1%, 1%, 5% = N0, N5, N6, N1); the same payloads p1-p4 (12 + 64/1280/1412/2788 B); the same argv to every harness
# (-d 5 measured window between a 2 s warm-up and cool-down; latency -i 0.1 -d 10 between 4096-round-trip edges bounded
# at 20 s); the same instruments (BenchStats.h CPU and eth0 counters, BenchWindow.h windows, RttQuantiles.h p50/p99);
# the framework order rotated by one every cell and every repetition (campaign_sweep.sh's run loop). zenoh-pico is the
# pinned release 1.10.2 (6a881c21, zenohpico/build.sh), shipped CMake defaults (DDS-default-first rule; no tuned arm).
#
# WHAT IS NOT THE SAME, AND IS LABELLED ON EVERY ROW. zenoh-pico's RELIABLE is TCP peer-to-peer: over UDP its
# Z_RELIABILITY_RELIABLE is monotonic sequence numbers only (1.10.2 src/transport/unicast/rx.c _z_unicast_handle_frame
# and src/transport/multicast/rx.c: "@TODO: amend once reliability is in place"), so a UDP reliable arm would be a label
# and not a behaviour (the user, 2026-10-28: BE on UDP, RELIABLE on TCP, no UDP-reliable arm). So BEST_EFFORT rows
# compare like with like (UDP multicast vs TickLE broadcast vs DDS UDP) and RELIABLE rows compare OUTCOMES of different
# repair mechanisms: TCP's stream repair vs per-sample repair over UDP. zenoh-pico has no history/KEEP_ALL/
# max_blocking settings; it ignores -N/-K/-B and the campaign gate checks its transport= instead (qos_identity_void).
# Expected and pre-registered: zenoh-pico's TCP link drops its peer at max rate and under loss (PC reproduction,
# zenoh_tcp_disconnect_repro.sh, 2026-10-28), so its reliable max-rate and lossy cells are expected to read DIED.
#
# CELLS (MATRIX_SPECS below). Kept within ~5 h of rig time by measuring p2/p3 at N0 only: under loss the two loss
# behaviours are p1 (one datagram) and p4 (fragmented by DDS and TickLE alike), and p2/p3 add a size, not a behaviour.
#   T best_effort_throughput max rate   N0 p1-p4, N5/N6/N1 p1,p4      (capacity and delivered fraction)
#   T reliable_throughput  -i 0.0001    N0 p1-p4, N5/N6/N1 p1,p4      (offered ~10 kHz to every framework: goodput of
#                                                                      complete samples at an offered load all can carry)
#   T reliable_throughput max rate      N0 p1,p4                      (capacity; zenoh-pico's TCP drop at max rate)
#   L reliable_latency                  N0 p1-p4, N5/N6/N1 p1,p4
#   L best_effort_latency               N0 p1,p4
#   control: the first cell (T BE p1 N0) is run again as the last (R8 drift, zenoh_fair_read.py).
# Known limit of the fixed-rate rows: -i is a sleep after each write, so a framework whose write costs more offers
# slightly fewer samples; delivered% (win_recv / win_sent) is read beside the absolute goodput for that reason.
# Not in this campaign: Array1m. zenoh-pico's default FRAG_MAX_SIZE is 4096 B (1.10.2 cmake/feature_config.cmake), its
# defragmentation buffer (src/transport/*/rx.c), so a 1 MiB sample needs a tuned build; and Array1m is an rmw-layer cell
# here (rmw_keepall_rig.sh), which is out of scope (the user, 2026-10-28: zenoh-pico core comparison only).
#
# READING RULES: zenoh_fair_read.py's header (R1-R8), pre-registered before the first run; TickLE vs DDS scoring is
# campaign_summary.py's, unchanged. zenohpico is a REFERENCE column: no WIN/LOSE for or against it.
#
# RIG TIME (estimate): 36 cells x 4 frameworks x 3 reps = 432 runs. Throughput runs ~29 s each (9 s window + ~20 s
# ssh/start/settle, from the 2026-09-26 campaign's 25 s at 5 s), 24 cells -> 2.3 h. Latency runs ~35 s at N0, ~40 s at
# 0.1% and up to ~65 s at 1-5% loss (each lost ping costs the 500 ms give-up until the 20 s edge bound), 12 cells ->
# 1.8 h. Builds ~10 min, rig_preflight ~10 min on the PC first. About 4.4 h (+-25%); 1.5x deadline 6.6 h.
# Usage:  DRY_RUN=1 zenoh_fair_queue.sh   prints the plan and the cell list; touches neither the rig nor the lock.
#         SHA=<landed commit on origin> zenoh_fair_queue.sh
#           1. zenoh_fair_preflight.sh (PC netns, low rate, zenoh-pico arm)       - stops the queue on a FAIL
#           2. campaign_sweep.sh (rig_preflight.sh for the other three, then the rig lock, held for the whole campaign)
#           3. campaign_summary.py and zenoh_fair_read.py on the output
#         The last line is always "ZENOH FAIR QUEUE DONE rc=<n>" (a watcher's end marker), success or not.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TMPDIR=${TMPDIR:-/home/semih/tmp_build} RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
DRY_RUN=${DRY_RUN:-0}
R=/home/semih/zenoh_fair_results
T=$(date +%Y%m%d-%H%M%S)
OUT=${OUT:-$R/zenoh_fair_$T.txt}
mkdir -p "$R"
rc=0
trap 'echo "ZENOH FAIR QUEUE DONE rc=$rc $(date -Is) out=$OUT"' EXIT

specs=("T|p1|Q1|N0|best_effort_throughput|")
for net in N0 N5 N6 N1; do
    sizes="p1 p4"
    [ "$net" = N0 ] && sizes="p1 p2 p3 p4"
    for p in $sizes; do specs+=("T|$p|Q1|$net|best_effort_throughput|"); done
    for p in $sizes; do specs+=("T|$p|Q0|$net|reliable_throughput|-Q|-i 0.0001"); done
    for p in $sizes; do specs+=("L|$p|Q0|$net|reliable_latency|"); done
    if [ "$net" = N0 ]; then
        for p in p1 p4; do specs+=("T|$p|Q0|N0|reliable_throughput|-Q"); done
        for p in p1 p4; do specs+=("L|$p|Q1|N0|best_effort_latency|"); done
    fi
done
specs+=("T|p1|Q1|N0|best_effort_throughput|")
MATRIX_SPECS=$(IFS=';'; echo "${specs[*]}")
export MATRIX_SPECS
FWS="tickle cyclonedds fastdds zenohpico"
echo "zenoh_fair_queue $(date -Is): ${#specs[@]} cells x [$FWS] x 3 reps, DRY_RUN=$DRY_RUN, out $OUT"

if [ "$DRY_RUN" = 1 ]; then
    FWS="$FWS" REPS=3 DUR=5 DRY_RUN=1 OUT="$OUT.dryrun" "$HERE/campaign_sweep.sh"
    rc=$?
    echo "--- the specs rig_preflight.sh would get (PREFLIGHT_FWS: the three it knows) ---"
    FWS="$FWS" PREFLIGHT_CELLS_ONLY=1 "$HERE/campaign_sweep.sh" | head -50
    exit "$rc"
fi
[ -n "${SHA:-}" ] || { echo "SHA= (the landed commit carrying zenoh_fair_queue.sh) is required"; rc=2; exit 2; }
if ! OUT="$R/preflight_$T" "$HERE/zenoh_fair_preflight.sh"; then
    echo "zenoh_fair_preflight.sh FAILED - not taking the rig"; rc=3; exit 3
fi
FWS="$FWS" PREFLIGHT_FWS="tickle cyclonedds fastdds" PF_JOBS=${PF_JOBS:-1} SHA="$SHA" REPS=3 DUR=5 OUT="$OUT" \
    "$HERE/campaign_sweep.sh"
rc=$?
echo "campaign rc=$rc $(date -Is)"
python3 "$HERE/campaign_summary.py" "$OUT" >"$OUT.summary" 2>&1
python3 "$HERE/zenoh_fair_read.py" "$OUT" >"$OUT.zenoh_fair" 2>&1
tail -3 "$OUT.summary"
exit "$rc"

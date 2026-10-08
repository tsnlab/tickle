#!/usr/bin/env bash
# lending_pc.sh - receive-buffer lending (docs/DESIGN.md section 10) on THIS PC, in private network namespaces, before
# the rig A/B. A PC figure for steering, never for publication.
#
# Usage: ARMS=<dir> lending_pc.sh      ARMS holds the arm directories (each a client and server, built per commit with
#   examples/perf_hil/tickle/build.sh): base_<scen>_<size> (the parent commit), head_<scen>_<size> (lending compiled in,
#   unused), keepcopy_<size> / keeplend_<size> (head's best_effort_throughput server with --keep copy / --keep lend).
# Env: REPS (5), DUR (5), OUT (results directory, outside any session directory). Each cell's summary (MEDIAN, VOID
#   and per-run rows) is the be_wake_cost.sh / reader_wake_cost.sh stdout in $OUT/<cell>.log; their raw runs in .txt.
#
# PRE-REGISTERED READING (printed with the results; the rig A/B in ~/rig_queue_lending.sh decides, not this):
#   unused (base vs head, same cells as the rig queue): msps / srv_recv_mps and usr_us + sys_us per sample for the
#     throughput cells, rtt_p50 for the latency cells. HELD = within 2 x SE of base; a head figure worse beyond 2 x SE is
#     a candidate regression to look at, not a finding (the PC's vCPUs make single cells noisy).
#   the copy saved (keepcopy vs keeplend, p1 and p3): srv_usr_us is where the copy is; keeplend must show
#     lent_per_sample >= 0.99 (else the arm did not get its treatment: VOID), keepcopy 0. The saving read is
#     keepcopy - keeplend in srv_usr_us, beside head (no keep) as the control both share.
set -uo pipefail
X="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARMS=${ARMS:?ARMS=<directory of arm dirs>}
REPS=${REPS:-5}
DUR=${DUR:-5}
OUT=${OUT:-$HOME/lend_results/pc_$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
say() { echo "$*" | tee -a "$OUT/driver.log"; }
trap 'say "=== lending_pc ended $(date -Is) ==="' EXIT
say "=== lending_pc $(date -Is) REPS=$REPS DUR=$DUR ARMS=$ARMS ==="
for cell in best_effort_throughput_p1 best_effort_throughput_p3 best_effort_throughput_p4 reliable_throughput_p3; do
    say "--- $cell base vs head ($(date +%T))"
    REPS=$REPS DUR=$DUR OUT=$OUT/$cell.txt "$X/be_wake_cost.sh" "$ARMS/base_$cell" "$ARMS/head_$cell" \
        >"$OUT/$cell.log" 2>&1
    grep '^MEDIAN\|^VOID' "$OUT/$cell.log" | tee -a "$OUT/driver.log"
done
for size in p1 p3; do
    say "--- keep $size: head (nothing kept) vs keepcopy vs keeplend ($(date +%T))"
    REPS=$REPS DUR=$DUR OUT=$OUT/keep_$size.txt "$X/be_wake_cost.sh" "$ARMS/head_best_effort_throughput_$size" \
        "$ARMS/keepcopy_$size" "$ARMS/keeplend_$size" >"$OUT/keep_$size.log" 2>&1
    grep '^MEDIAN\|^VOID' "$OUT/keep_$size.log" | tee -a "$OUT/driver.log"
done
for cell in reliable_latency_p2 reliable_latency_p4; do
    say "--- $cell base vs head, -i 0.005 ($(date +%T))"
    REPS=$REPS DUR=$DUR SPACING=0.005 OUT=$OUT/$cell.txt "$X/reader_wake_cost.sh" "$ARMS/base_$cell" "$ARMS/head_$cell" \
        >"$OUT/$cell.log" 2>&1
    grep -E '^MEDIAN|^VOID|^arm=' "$OUT/$cell.log" | tail -12 | tee -a "$OUT/driver.log"
done
say "=== ALL_DONE $(date -Is) ==="

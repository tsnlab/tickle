#!/usr/bin/env bash
# s6_latency_driver.sh - the RTT half of S6, which §2.2c records as the one metric still unmeasured.
#
# The throughput cells (s6_transport_cells.sh, 2026-10-01) established that all three frameworks really do carry
# their data over their own shared-memory transport on this host, and gave CPU, memory and throughput. They gave no
# latency, because reliable_throughput does not measure one. This runs the SAME harness with SCEN=reliable_latency
# so the transport witness and the identity checks are unchanged and only the scenario differs.
#
# HOW TO READ IT, written before the run:
#   Each framework's ON arm must pass its own witness band (ratio <= 0.25 against its kernel arm) before its RTT is
#   quoted at all - an RTT from an arm that silently used the kernel is a network figure wearing a segment's name.
#   Then, per framework, rtt_avg_ms and rtt_max_ms, median of the repetitions, ON against OFF:
#     ON clearly below OFF   -> the segment shortens the round trip, and by how much is the cell
#     ON within OFF's spread -> no latency gain visible at this payload; say that, do not round it to a win
#     ON above OFF           -> the segment costs latency here; report it first, as the p4 throughput loss was
#   A framework whose ON arm fails the witness is VOID for this cell, named, and its number is not published.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SIZES=${SIZES:-"p2"}
SHA=${SHA:-$(git -C "$REPO" rev-parse --short origin/main)}
# Exported rather than passed as an env prefix on the call: with the prefix, shellcheck cannot tell that the
# $SHA inside OUT is this shell's and not the one being assigned on the same command (SC2097/SC2098), and
# lint-shell is a gate. The values are identical either way; this just says so unambiguously.
export SCEN=reliable_latency SHA REPS="${REPS:-3}" DUR="${DUR:-5}"
for sz in $SIZES; do
    export SIZE="$sz" OUT="$HOME/rig_results_safe/s6_latency_${sz}_${SHA}.txt"
    "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh"
    echo "=== $sz finished $(date -Is) ===" >> "$HOME/rig_results_safe/s6_latency_driver.log"
done
echo "ALL_DONE $(date -Is)" >> "$HOME/rig_results_safe/s6_latency_driver.log"

# PRE-REGISTERED for the 2026-10-02 re-run, after the first attempt produced no number:
#   The first run used the throughput cells' DUR=5 with the latency client's default 1 s ping interval, so each arm
#   sent FIVE samples. TickLE's tx_shm was 0 and its witness 1.000 - the segment never attached. The reading is that
#   tt_SEGMENT_ATTACH_RETRY_SENDS is 256: a first attach attempt that fails is re-asked only every 256 sends, and
#   five sends never reach it. So this run uses -i 0.005 over DUR=10, about 2,000 round trips per rep, which passes
#   that threshold roughly eight times.
#     TickLE's ON arm attaches (tx_shm > 0, witness <= 0.25)  -> the reading was right, and the RTT cell is valid
#     TickLE's ON arm still does not attach                   -> the threshold is NOT the explanation and the cell
#         stays VOID; that is then a finding about the attach heuristic on low-volume traffic, not about latency,
#         and it is worth more than the RTT row was.

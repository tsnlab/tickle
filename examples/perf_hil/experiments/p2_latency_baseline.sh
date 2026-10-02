#!/usr/bin/env bash
# The p2 latency baseline on today's HEAD, which is the control any FIFO doorbell work would be judged
# against and does not currently exist on this code.
#
# WHY NOW. COMPARISON 2.2c's latency table (TickLE kernel 0.050 ms, segment 0.058, separable, 16%) was
# measured on fcc4ddb4. Tonight added record_size_limit() to the publish path of every build. That was
# measured inert on p4 THROUGHPUT against a control, which says nothing about p2 LATENCY - a different
# scenario, a different payload and a metric that a few hundred nanoseconds can move. Open question 1
# proposes a FIFO doorbell worth ~5.61 us of the ~8 us gap, and that proposal rests on a gap figure
# measured before tonight. Re-establish the gap on this code before building anything to close it.
#
# tickle only: the vendors are unchanged by anything tonight, and FRAMEWORKS=tickle keeps the build on
# the rig rather than on this PC, so it does not load the box a peer's netns suite is using.
#
# HOW TO READ IT, written before the run:
#   segment arm separably SLOWER than the kernel arm   -> the gap stands on this code. The FIFO premise
#        survives and open question 1's arithmetic can be re-applied to THIS gap rather than to fcc4ddb4's.
#   ranges OVERLAP                                      -> the gap is no longer separable on this code, and
#        the FIFO proposal needs re-deriving before any of it is built; do not implement against a gap that
#        is not currently measurable.
#   segment arm separably FASTER                        -> report it first and look for a harness fault
#        before believing it; nothing tonight should have made the segment path faster at p2.
#   CONTROL: the kernel (OFF) arm against its own previous value, 0.050 ms. record_size_limit() cannot
#        change that arm - with segments compiled out it resolves to the constants it replaced - so a move
#        there is the machine or the harness, and it is what tells a real change from a drifting rig.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-a55d9cf9}
OUT=${OUT:-$HOME/rig_results_safe/p2_latency_baseline.txt}
: >"$OUT"
echo "=== p2 latency baseline $(date -Is) sha=$SHA ===" | tee -a "$OUT"
# -i 0.005 is not optional and the first run of this script omitted it. The published figure is ~1,950
# round trips a rep (COMPARISON 2.2c, fcc4ddb4); without the interval the client falls back to one ping a
# second and DUR=10 gives TEN. Ten round trips produce an rtt_min that is one sample's luck, and comparing
# that to a 1,950-sample figure is comparing two different measurements with the same name.
timeout 1800 env FRAMEWORKS=tickle REPS=3 DUR=10 SCEN=reliable_latency SIZE=p2 SHA="$SHA" \
    CLI_ARGS="-i 0.005" \
    OUT=/tmp/p2lat_cells.txt "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1

# Refuse to report at all on too few samples, rather than reporting a number nobody can use. Pre-registered
# because it already happened once: the sample count is the first thing to check and the last thing anyone
# looks at, and a thin run does not announce itself - it produces a plausible figure.
MIN_SAMPLES=${MIN_SAMPLES:-1000}
thin=$(grep -hoE '(^| )sent=[0-9]+' /tmp/p2lat_cells.txt.tickle 2>/dev/null | cut -d= -f2 | sort -n | head -1)
if [ -z "$thin" ] || [ "$thin" -lt "$MIN_SAMPLES" ]; then
    echo "VOID: the thinnest rep carried ${thin:-no} round trips, under the $MIN_SAMPLES this comparison needs." | tee -a "$OUT"
    echo "  The published 0.050/0.058 came from ~1,950 a rep. Nothing here is comparable to it." | tee -a "$OUT"
    echo "=== done $(date -Is) ===" | tee -a "$OUT"
    exit 1
fi
grep -hE 'arm=|built:|FATAL' /tmp/p2lat_cells.txt.tickle 2>/dev/null | tee -a "$OUT" >/dev/null
echo "--- arms ---" | tee -a "$OUT"
grep -hoE 'arm=(ON|OFF)|rtt_p50_ms=[0-9.]+|rtt_us_p50=[0-9.]+|p50_us=[0-9.]+' "$OUT" | paste - - 2>/dev/null | tee -a "$OUT"
echo "=== done $(date -Is) ===" | tee -a "$OUT"

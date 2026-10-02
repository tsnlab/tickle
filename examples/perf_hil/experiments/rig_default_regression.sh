#!/usr/bin/env bash
# Did record_size_limit() change anything at the DEFAULT slot, where 6e(a) cannot grant?
#
# The argument says no: with every peer's slot at one datagram, whole_record_limit_for() returns that, and
# record_size_limit(floor=tt_MAX_BUFFER_LENGTH) and record_size_limit(floor=FRAG_WHOLE_DATA_LIMIT) both
# resolve to exactly the constants they replaced. But "should be inert" is an argument, and two arguments of
# mine were wrong tonight - the hoist comment claimed the bound could not come apart while sitting on the code
# that parted it. This is the measurement.
#
# Against OUR OWN previous value, not against a vendor: the vendors control the rig, not our regressions. The
# figure to beat is the default-slot ON arm, 2745 Mbps, measured before record_size_limit() existed.
#
# HOW TO READ IT, written before the run:
#   ON arm within the run-to-run spread of 2745          -> inert at the default, as argued. Safe to keep
#        quoting 2745, and the 6e(a) hold is the only behavioural change in 73073336.
#   ON arm separably BELOW 2745                          -> record_size_limit() costs something on the path
#        every build takes. That is a regression in the hot path and outranks all of the 6e work.
#   ON arm separably ABOVE                               -> report it, do not celebrate it; the likeliest
#        cause is a difference in the arms rather than a gain, and it needs the same scrutiny as a loss.
#   datagrams per sample must stay ~2.01 on the ON arm. At the default slot the record cannot go whole, so a
#        ratio near 1.0 would mean the ceiling is not holding and the reading above is void.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-73073336}
OUT=${OUT:-$HOME/rig_results_safe/rig_default_regression.txt}
: >"$OUT"
echo "=== default-slot regression $(date -Is) sha=$SHA (no BUILD_FLAGS: the shipped geometry) ===" | tee -a "$OUT"
timeout 1500 env FRAMEWORKS=tickle REPS=3 DUR=3 SCEN=reliable_throughput SIZE=p4 SHA="$SHA" \
    OUT=/tmp/rigdef_cells.txt "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
grep -hE 'arm=|built:|FATAL' /tmp/rigdef_cells.txt.tickle 2>/dev/null | tee -a "$OUT" >/dev/null
python3 "$REPO/examples/perf_hil/experiments/rig_default_regression_analyse.py" "$OUT" >"$OUT.verdict" 2>&1
# The verdict goes to its own file and is checked for, because on 2026-10-03 a bad edit left this
# heredoc malformed: python died on a syntax error, the error went to the console, "=== done" printed,
# and the script exited 0 with NO VERDICT AT ALL. A run that completes without concluding anything must
# not look like a run that concluded. Same rule as everywhere else tonight - an absent answer needs its
# own answer.
if grep -qE '^  (INERT|REGRESSION|ABOVE THE CONTROL|VOID)' "$OUT.verdict"; then
    tee -a "$OUT" <"$OUT.verdict"
else
    {
        echo "NO VERDICT: the analysis produced no reading. The arms above may be fine; this says only"
        echo "  that nothing judged them. Do not record anything from this run. Analysis output was:"
        sed 's/^/    /' "$OUT.verdict"
    } | tee -a "$OUT"
    exit 1
fi
echo "=== done $(date -Is) ===" | tee -a "$OUT"

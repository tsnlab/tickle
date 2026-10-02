#!/usr/bin/env bash
# Why does the rig never grant a whole-record send, when the same code grants locally?
#
# Locally (whole_record_refusal.sh, 2026-10-03) the predicate refuses ZERO times at slot 4096 and the cell
# collapses because it grants. On the rig the same flag gave 2.01 datagrams per sample across four campaigns,
# which is the signature of splitting - so something there refuses that does not refuse here. a5e9c553 makes
# the publisher name the cause and keeps the client's log, which earlier runs discarded.
#
# Outside the repo on purpose: the repo tree is under `make check-gates` while this runs, and a new .sh inside
# it would be linted by the gate that is running. Results outside /tmp and outside the session scratchpad, so
# they survive this session (CLAUDE.md section 1).
#
# HOW TO READ IT, written before the run:
#   "Whole-record send refused: <cause>"  -> that is the answer; the rig refuses for that reason and the
#        local/rig difference is explained by it.
#   no refusal line, and the control line present -> the rig GRANTS too, and 2.01 datagrams/sample must come
#        from somewhere other than the predicate. That would make the rig and local behaviour the same and
#        the four campaigns' reading wrong for a second, different reason.
#   control line absent -> this captured nothing; say so and read nothing else.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-a5e9c553}
OUT=${OUT:-$HOME/rig_results_safe/rig_whole_refusal.txt}
: >"$OUT"
echo "=== rig whole-record refusal $(date -Is) sha=$SHA ===" | tee -a "$OUT"
timeout 900 env FRAMEWORKS=tickle REPS=1 DUR=3 SCEN=reliable_throughput SIZE=p4 SHA="$SHA" \
    BUILD_FLAGS="-Dtt_SEGMENT_SLOT_BYTES=4096" OUT=/tmp/rigwr_cells.txt \
    "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
echo "--- cells ---" | tee -a "$OUT"
grep -hE 'arm=|built:|FATAL' /tmp/rigwr_cells.txt.tickle 2>/dev/null | tee -a "$OUT"
K=$HOME/.ssh/tickle_ci_ed25519
# Read from the driver rather than written down twice: the fallback below is a guess, and a probe that
# ssh'd to the WRONG host would report no refusal line, which is indistinguishable from no refusal.
# shellcheck disable=SC2016  # a sed script, which must not expand here
HOST=$(sed -n 's/^HOST=\${HOST:-\([^}]*\)}.*/\1/p' "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" | head -1)
HOST=${HOST:-192.168.10.11}
echo "--- control: did the publisher's log reach us? (host $HOST) ---" | tee -a "$OUT"
ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" \
    "grep -c 'Node open' /tmp/s6wit_client.log 2>/dev/null || echo 0" </dev/null | tee -a "$OUT"
echo "--- refusal causes named by the publisher ---" | tee -a "$OUT"
ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" \
    "grep 'Whole-record send refused' /tmp/s6wit_client.log 2>/dev/null | sort -u || true" </dev/null | tee -a "$OUT"
echo "=== done $(date -Is) ===" | tee -a "$OUT"

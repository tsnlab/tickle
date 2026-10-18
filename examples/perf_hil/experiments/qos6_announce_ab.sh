#!/usr/bin/env bash
# qos6_announce_ab.sh - QoS plan step 6 (D1, D8, D9: wire v14, +12 B per announce record) against its parent, on the
# three things the plan's A/B names: announce bytes, discovery join time, and the rmw receive path. The reading is
# qos6_announce_ab.py's docstring, written before any run and copied to $OUTB.prereg.txt (its sha256 in the log)
# before the first measurement; each part's verdict is computed by that reader, not by eye.
#
#   1. bytes  (PC, static, seconds) announce_bytes.c built against A and B.
#   2. rmw    (rig, under its lock, ~2 h with tput) ab_rmw_regress.sh A B, TPUT_TOPICS="Array1k Array4k" by default:
#             same-host rmw RTT and CPU, RELIABLE and BEST_EFFORT, phases A B B A B A A B, cyclonedds as the control -
#             the receive path is where the change adds a byte read and a branch per sample. It needs its PC preflight
#             build at B first (pc_preflight_build.sh, as for the step-3 A/B); RMW_OUTB=<its OUTB> reuses a run.
#   3. join   (PC, private netns, under the box lock, ~10 min) discovery_join.sh join JOIN_N JOIN_E JOINS for A, B, B,
#             A: a node joins JOIN_N-1 running ones, each announcing JOIN_E endpoints (more than a datagram holds, so the
#             fragmented announce is what joins).
#   final: qos6_announce_ab.py final, one VERDICT line in $OUTB.verdict.txt and on the ALL_DONE line.
#
# Usage (detached; ~/rig_results_safe/qos6_announce_ab_<stamp>.*, the driver log ends "=== ALL_DONE ... ==="):
#   A=461d6558 B=<sha on an origin branch> setsid nohup examples/perf_hil/experiments/qos6_announce_ab.sh \
#       > ~/rig_results_safe/qos6_announce_ab.log 2>&1 < /dev/null &
#   env: JOIN_N(6) JOIN_E(32) JOINS(10) TPUT_TOPICS("Array1k Array4k") RMW_OUTB(run ab_rmw_regress.sh) SKIP_RMW(0)
#        SKIP_JOIN(0) OUT_TAG (in the file names, e.g. _dryrun); PHASES REPS RTT_DUR TPUT_DUR PREFLIGHT PF_BUILD_DIR
#        RIG_LOCK_WAIT pass to ab_rmw_regress.sh.
# ESTIMATE: bytes < 1 min; rmw ~90 min + ~60 min of tput (ab_rmw_regress.sh prints its own); join ~10 min.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
A=${A:-461d6558}
B=${B:?B=<sha>}
JOIN_N=${JOIN_N:-6}
JOIN_E=${JOIN_E:-32}
JOINS=${JOINS:-10}
export TPUT_TOPICS=${TPUT_TOPICS-"Array1k Array4k"}
OUTB=""
LOG=/dev/null
TMP=$(mktemp -d "${TMPDIR:-/tmp}/qos6_announce_ab.XXXXXX")
on_exit() { # the one EXIT trap
    local rc=$? v=""
    rm -rf "$TMP"
    [ -n "$OUTB" ] && [ -f "$OUTB.verdict.txt" ] && v=" | $(head -n 1 "$OUTB.verdict.txt")"
    echo "=== ALL_DONE $(date -Is) rc=$rc${v} ===" | tee -a "$LOG"
}
trap on_exit EXIT
trap 'exit 143' TERM INT HUP
say() { echo "$*" | tee -a "$LOG"; }

for v in A B; do
    full=$(git -C "$REPO" rev-parse --verify -q "${!v}^{commit}") || { echo "REFUSED: $v=${!v} does not resolve" >&2; exit 2; }
    printf -v "$v" '%s' "$full"
done
mkdir -p "$HOME/rig_results_safe"
OUTB=$HOME/rig_results_safe/qos6_announce_ab${OUT_TAG:-}_$(date +%Y%m%d-%H%M%S)
LOG=$OUTB.driver.log
say "=== qos6_announce_ab $(date -Is) on $(hostname) ==="
say "    A=$A B=$B JOIN_N=$JOIN_N JOIN_E=$JOIN_E JOINS=$JOINS TPUT_TOPICS='$TPUT_TOPICS'"
python3 "$X/qos6_announce_ab.py" --prereg >"$OUTB.prereg.txt" || exit 2
say "    prereg sha256 $(sha256sum "$OUTB.prereg.txt" | cut -c1-64)"

# 1. announce bytes, each tree's own encoder.
say "--- 1. announce bytes $(date +%T)"
for v in A B; do
    mkdir -p "$TMP/$v"
    git -C "$REPO" archive "${!v}" src include tests examples/perf_hil/experiments/announce_bytes.c | tar -x -C "$TMP/$v" ||
        exit 1
    (cd "$TMP/$v" && gcc -I include -I src -o "$TMP/announce_bytes_$v" examples/perf_hil/experiments/announce_bytes.c \
        src/encoding.c src/log.c -lm) >"$OUTB.bytes_build_$v.log" 2>&1 || { say "    build $v failed"; exit 1; }
    "$TMP/announce_bytes_$v" >"$OUTB.bytes_$v.txt" || exit 1
done
python3 "$X/qos6_announce_ab.py" bytes "$OUTB.bytes_A.txt" "$OUTB.bytes_B.txt" >"$OUTB.bytes_verdict.txt"
say "    $(head -n 1 "$OUTB.bytes_verdict.txt")"

# 2. the rmw receive path on the rig: ab_rmw_regress.sh, found afterwards by the exact A=/B= line its driver log opens
# with (not by recency), or named by RMW_OUTB.
if [ -n "${RMW_OUTB:-}" ]; then
    say "--- 2. rmw: reusing $RMW_OUTB"
elif [ "${SKIP_RMW:-0}" = 1 ]; then
    say "--- 2. rmw: SKIPPED (SKIP_RMW=1) - the final verdict will be VOID"
else
    say "--- 2. rmw: ab_rmw_regress.sh $(date -Is)"
    touch "$TMP/rmw_start"
    A="$A" B="$B" "$X/ab_rmw_regress.sh" >"$OUTB.rmw.log" 2>&1
    say "    ab_rmw_regress.sh exit $? ($(tail -n 1 "$OUTB.rmw.log"))"
    want="    A=$A B=$B "
    for f in "$HOME"/rig_results_safe/ab_rmw_regress_*.driver.log; do
        [ "$f" -nt "$TMP/rmw_start" ] || continue
        if grep -qF -- "$want" "$f"; then
            RMW_OUTB=${f%.driver.log}
        fi
    done
    say "    its OUTB: ${RMW_OUTB:-NOT FOUND}"
fi
printf '%s\n' "${RMW_OUTB:-}" >"$OUTB.rmw_outb"

# 3. discovery join, on the PC under the box lock, A B B A.
if [ "${SKIP_JOIN:-0}" = 1 ]; then
    say "--- 3. join: SKIPPED (SKIP_JOIN=1) - the final verdict will be VOID"
else
    say "--- 3. join $(date -Is)"
    i=0
    for ph in A B B A; do
        i=$((i + 1))
        n=$(((i + 1) / 2))
        [ "$i" -eq 4 ] && n=2
        [ "$i" -eq 1 ] && n=1
        out="$OUTB.join_${ph}_$n.txt"
        RIG_LOCK_SCOPE=box RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-1800} SHA="${!ph}" OUT="$out" \
            "$REPO/examples/perf_hil/rig_lock.sh" "$X/discovery_scaling/discovery_join.sh" join "$JOIN_N" "$JOIN_E" \
            "$JOINS" >"$out.log" 2>&1
        say "    $ph ($n): exit $?; $(grep -c '| ok |' "$out" 2>/dev/null || echo 0) of $JOINS joins ok"
    done
    python3 "$X/qos6_announce_ab.py" join "$OUTB.join_A_1.txt" "$OUTB.join_B_1.txt" "$OUTB.join_B_2.txt" \
        "$OUTB.join_A_2.txt" >"$OUTB.join_verdict.txt"
    say "    $(head -n 1 "$OUTB.join_verdict.txt")"
fi

python3 "$X/qos6_announce_ab.py" final "$OUTB" >"$OUTB.verdict.txt"
cat "$OUTB.verdict.txt" >>"$LOG"

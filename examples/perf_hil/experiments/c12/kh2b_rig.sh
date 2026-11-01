#!/usr/bin/env bash
# kh2b_rig.sh - KH2 warm, step B: the two wait-ring candidates (ab/ring-fixed-file, ab/ring-poll-first) A/B against today's
# ring. Written 2026-11-01 before any of its runs; the arms, the readback and PASS / FAIL / VOID are in kh2b_read.py (code,
# with --selftest).
#
# One call of rmw_crosshost_rtt.sh, unpinned, at main with PI_PATCH=c12/kh2b.patch (both candidates behind their
# switches, applied on both Pis, so every arm is one source): TICKLE_VARIANTS="libc_ringbase libc_ringff libc_ringpf", no
# vendor, INJECT_ARMS="none 50", 13 reps, STAMPS=1 (the pong's callback).
# Locks, inside this detached process, in this order, held for every cell: wait while /home/semih/tmp_build/PC_QUIET
# exists; flock /home/semih/rig_jobs/pc_ab.lock; examples/perf_hil/rig_lock.sh (hil, RIG_LOCK_WAIT=36000).
# Usage: setsid nohup env SHA=<sha on origin> TS=$TS examples/perf_hil/experiments/c12/kh2b_rig.sh \
#          > ~/rig_results_safe/kh2b_$TS.log 2>&1 < /dev/null &
# ESTIMATE: build ~6 min (three variants) + 13 reps x 6 rows x ~65 s = ~90 min under the lock.
# Results: ~/rig_results_safe/kh2b_<sha8>_<TS>.txt (+ .pcaps .sysstamp .inject .logs .stamps), the reading in
# ~/rig_results_safe/kh2b_<TS>.reading.txt. Last line always "KH2B_END rc=<n>".
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
EXP=$(cd "$HERE/.." && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SAFE=$HOME/rig_results_safe
SHA=${SHA:?SHA=<commit on origin>}
TS=${TS:-$(date +%Y%m%d-%H%M%S)}
rc=1
if [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    trap 'echo "KH2B_END rc=$rc $(date -Is)"' EXIT
    python3 "$HERE/kh2b_read.py" --selftest | tail -1 | grep -q "SELFTEST OK" || { echo "REFUSED: kh2b_read.py --selftest"; exit 1; }
    git -C "$REPO" fetch -q origin || { echo "REFUSED: git fetch"; exit 1; }
    full=$(git -C "$REPO" rev-parse --verify -q "$SHA^{commit}") || { echo "REFUSED: $SHA is not a commit here"; exit 1; }
    [ -n "$(git -C "$REPO" branch -r --contains "$full" 2>/dev/null)" ] || { echo "REFUSED: $SHA is on no origin branch"; exit 1; }
    SHA=$full
    if [ "${DRY_RUN:-0}" = 1 ]; then
        echo "DRY_RUN clean: SHA ${SHA:0:8}, reps $(python3 "$HERE/kh2b_read.py" --reps-needed)"
        rc=0
        exit 0
    fi
    waited=0
    while [ -e /home/semih/tmp_build/PC_QUIET ]; do
        [ $((waited % 300)) = 0 ] && echo "waiting: /home/semih/tmp_build/PC_QUIET exists ($(date -Is))"
        sleep 10
        waited=$((waited + 10))
    done
    exec 8>>/home/semih/rig_jobs/pc_ab.lock
    echo "taking pc_ab.lock ($(date -Is))"
    flock -w 36000 8 || { echo "REFUSED: pc_ab.lock not free within 10 h"; exit 75; }
    echo "pc_ab.lock held ($(date -Is)); taking the rig lock"
    export SHA TS RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=36000
    "$REPO/examples/perf_hil/rig_lock.sh" "${BASH_SOURCE[0]}"
    rc=$?
    exit "$rc"
fi
# ---- under both locks ----
echo "rig lock held ($(date -Is))"
o=$SAFE/kh2b_${SHA:0:8}_$TS.txt
reps=$(python3 "$HERE/kh2b_read.py" --reps-needed)
echo "--- ${SHA:0:8}, $reps reps -> $o ($(date -Is))"
SHA=$SHA ROTATE=1 WAITS=block REPS=$reps MSGS=bench QOSES=best_effort PI_PATCH=$HERE/kh2b.patch TICKLE_VARIANTS="libc_ringbase libc_ringff libc_ringpf" VARIANT_VENDORS='' STAMPS=1 \
    SYSSTAMP_ARMS=on CAPTURE=1 INJECT_ARMS="none 50" OUT=$o "$EXP/rmw_crosshost_rtt.sh" >"$o.launch.log" 2>&1
echo "  rc=$? rows ok: $(grep -c ' | ok | ' "$o" 2>/dev/null) of $(grep -c ' inject=' "$o" 2>/dev/null) ($(date -Is))"
R=$SAFE/kh2b_$TS.reading.txt
timeout 900 python3 "$HERE/kh2b_read.py" "$o" >"$R" 2>&1
rc=$?
cat "$R"
exit "$rc"

#!/usr/bin/env bash
# step3_rig.sh - C12 idle wake, step 3: the sockets' options (UDP checksum 0, the UDP_GRO probe) as TickLE's idle cost.
# Written 2026-10-31 before any of its runs; the arms, n, the control rule (the excess E over CycloneDDS's own
# difference), the treatment readback and every reading rule are in step3_read.py (code, with --selftest).
#
# One call of rmw_crosshost_rtt.sh as step2_rig.sh, with TICKLE_VARIANTS="libc_rb libc_rb_csum libc_rb_nogro" (wait ring
# as shipped) beside rmw_cyclonedds_cpp, INJECT_ARMS="none 50", 13 reps, PI_PATCH=c12/step3.patch (TT_EXP_C12_READBACK,
# TT_EXP_NO_GRO_SETUP: experiment only, inert unless defined; every build of the session is of the patched tree).
# Locks, inside this detached process, in this order, held for every cell: wait while /home/semih/tmp_build/PC_QUIET
# exists; flock /home/semih/rig_jobs/pc_ab.lock; examples/perf_hil/rig_lock.sh (hil, RIG_LOCK_WAIT=36000).
# Usage: setsid nohup env SHA=<sha on origin> TS=$TS examples/perf_hil/experiments/c12/step3_rig.sh \
#          > ~/rig_results_safe/c12s3_$TS.log 2>&1 < /dev/null &
# ESTIMATE: build ~8 min (three variants) + 13 reps x 8 rows x ~65 s = ~2 h under the lock.
# Results: ~/rig_results_safe/c12s3_<sha8>_<TS>.txt (+ .pcaps .sysstamp .inject .logs), the reading in
# ~/rig_results_safe/c12s3_<TS>.reading.txt. Last line always "STEP3_END rc=<n>".
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
EXP=$(cd "$HERE/.." && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SAFE=$HOME/rig_results_safe
SHA=${SHA:?SHA=<commit on origin>}
TS=${TS:-$(date +%Y%m%d-%H%M%S)}
rc=1
if [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    trap 'echo "STEP3_END rc=$rc $(date -Is)"' EXIT
    python3 "$HERE/step3_read.py" --selftest | tail -1 | grep -q "SELFTEST OK" || { echo "REFUSED: step3_read.py --selftest"; exit 1; }
    git -C "$REPO" fetch -q origin || { echo "REFUSED: git fetch"; exit 1; }
    full=$(git -C "$REPO" rev-parse --verify -q "$SHA^{commit}") || { echo "REFUSED: $SHA is not a commit here"; exit 1; }
    [ -n "$(git -C "$REPO" branch -r --contains "$full" 2>/dev/null)" ] || { echo "REFUSED: $SHA is on no origin branch"; exit 1; }
    SHA=$full
    if [ "${DRY_RUN:-0}" = 1 ]; then
        echo "DRY_RUN clean: SHA ${SHA:0:8}, reps $(python3 "$HERE/step3_read.py" --reps-needed)"
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
o=$SAFE/c12s3_${SHA:0:8}_$TS.txt
reps=$(python3 "$HERE/step3_read.py" --reps-needed)
echo "--- ${SHA:0:8}, $reps reps -> $o ($(date -Is))"
SHA=$SHA ROTATE=1 WAITS=block REPS=$reps MSGS=bench QOSES=best_effort PI_PATCH=$HERE/step3.patch TICKLE_VARIANTS="libc_rb libc_rb_csum libc_rb_nogro" VARIANT_VENDORS=rmw_cyclonedds_cpp \
    SYSSTAMP_ARMS=on CAPTURE=1 INJECT_ARMS="none 50" OUT=$o "$EXP/rmw_crosshost_rtt.sh" >"$o.launch.log" 2>&1
echo "  rc=$? rows ok: $(grep -c ' | ok | ' "$o" 2>/dev/null) of $(grep -c ' inject=' "$o" 2>/dev/null) ($(date -Is))"
R=$SAFE/c12s3_$TS.reading.txt
timeout 900 python3 "$HERE/step3_read.py" "$o" >"$R" 2>&1
rc=$?
cat "$R"
exit "$rc"

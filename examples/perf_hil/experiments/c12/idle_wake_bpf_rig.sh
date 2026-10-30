#!/usr/bin/env bash
# idle_wake_bpf_rig.sh - C12 idle wake, step 5: the pong's kernel receive path split by pong_rx_ring_split.bt, arms none
# and 50 ms, rmw_tickle@libc and CycloneDDS, read by idle_wake_bpf_read.py (its reading rule is written there). Written
# 2026-10-31 after idle_wake_rig.sh's run, before this one's. bpftrace's probes cost time in the path they time, so the
# figures are compared only between the arms of this run, never with idle_wake_rig.sh's.
#
# Locks as idle_wake_rig.sh: wait while /home/semih/tmp_build/PC_QUIET exists, flock /home/semih/rig_jobs/pc_ab.lock,
# then examples/perf_hil/rig_lock.sh (hil, RIG_LOCK_WAIT=36000), all inside this detached process.
# Usage: setsid nohup env SHA=<sha on origin> TS=$TS examples/perf_hil/experiments/c12/idle_wake_bpf_rig.sh \
#          > ~/rig_results_safe/c12iwb_$TS.log 2>&1 < /dev/null &
# ESTIMATE: build ~2 min + REPS (4) x 4 rows x ~60 s = ~18 min. Last line always "IDLE_WAKE_BPF_END rc=<n>".
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
EXP=$(cd "$HERE/.." && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SAFE=$HOME/rig_results_safe
SHA=${SHA:?SHA=<commit on origin>}
TS=${TS:-$(date +%Y%m%d-%H%M%S)}
rc=1
if [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    trap 'echo "IDLE_WAKE_BPF_END rc=$rc $(date -Is)"' EXIT
    git -C "$REPO" fetch -q origin || { echo "REFUSED: git fetch"; exit 1; }
    full=$(git -C "$REPO" rev-parse --verify -q "$SHA^{commit}") || { echo "REFUSED: $SHA is not a commit here"; exit 1; }
    [ -n "$(git -C "$REPO" branch -r --contains "$full" 2>/dev/null)" ] || { echo "REFUSED: $SHA is on no origin branch"; exit 1; }
    SHA=$full
    while [ -e /home/semih/tmp_build/PC_QUIET ]; do sleep 10; done
    exec 8>>/home/semih/rig_jobs/pc_ab.lock
    echo "taking pc_ab.lock ($(date -Is))"
    flock -w 36000 8 || { echo "REFUSED: pc_ab.lock not free within 10 h"; exit 75; }
    echo "pc_ab.lock held ($(date -Is)); taking the rig lock"
    export SHA TS RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=36000
    "$REPO/examples/perf_hil/rig_lock.sh" "${BASH_SOURCE[0]}"
    rc=$?
    exit "$rc"
fi
echo "rig lock held ($(date -Is))"
o=$SAFE/c12iwb_${SHA:0:8}_$TS.txt
SHA=$SHA ROTATE=1 WAITS=block REPS=${REPS:-4} MSGS=bench QOSES=best_effort TICKLE_VARIANTS=libc VARIANT_VENDORS=rmw_cyclonedds_cpp \
    SYSSTAMP_ARMS=off BPF_ARMS=on BPF_SCRIPT=$HERE/pong_rx_ring_split.bt CAPTURE=1 INJECT_ARMS="none 50" OUT=$o \
    "$EXP/rmw_crosshost_rtt.sh" >"$o.launch.log" 2>&1
echo "  rc=$? rows ok: $(grep -c ' | ok | ' "$o" 2>/dev/null) of $(grep -c ' inject=' "$o" 2>/dev/null) ($(date -Is))"
R=$SAFE/c12iwb_$TS.reading.txt
timeout 300 python3 "$HERE/idle_wake_bpf_read.py" "$o" >"$R" 2>&1
rc=$?
cat "$R"
exit "$rc"

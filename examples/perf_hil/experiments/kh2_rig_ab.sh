#!/usr/bin/env bash
# kh2_rig_ab.sh - RESULTS KH2 re-measured for a parent and a branch under ONE hil lock (ab/kh2-receive-return: the wait
# ring's first move at 2 waits, not 64). Written 2026-10-24 before any of its runs. Prepared for the lead to queue;
# nothing here was run on the rig by its author.
#
# Each arm is exactly KH2's call (rig_queue_s1_kh.sh part KH) of rmw_crosshost_rtt.sh - ROTATE=1 WAITS=block REPS=3
# MSGS=bench TICKLE_VARIANTS="default libc" SYSSTAMP_ARMS="off on" CAPTURE=1 STAMPS=1, the three rmws - with
# QOSES=best_effort (KH2's deciding cell; its RELIABLE cell was NOT READ, the wire control failed), then
# rmw_pcap_split.py. Order P, B (the vendors, which neither SHA changes, are the control between the two). Read by:
#   s1_kh_rig_read.py --kh B.split --prev P.split    the vendors' own tap->recv must hold within 1.0 us (else NOT
#                                                     COMPARABLE); CLOSED / NARROWED (>= 1.0 us less gap) / NOT CLOSED
#   kh2_pong_calls.py on each arm's rmw_tickle@libc rows   THE TREATMENT: P's first ~20 measured round trips received
#                                                     with epoll_pwait2 + recvfrom, B's by the ring (io_uring_enter)
#                                                     from the first; B with epoll round trips beyond 1 per rep -> the
#                                                     change did not act on the rig: do not read the gap.
# What is expected (written before the run, so the reading cannot be bent to it): the change removes the epoll round
# trips at the start of each run, ~20% of KH2's samples at ~3-4 us more each, so rmw_tickle@libc's p50 moves by a few
# tenths of a microsecond and its mean by ~0.7 us; NARROWED (>= 1.0 us at p50) is NOT expected. The rest of the gap to
# the vendors is the wait ring's kernel path against a blocking recvmsg (recv_wake_xhost on the rig: 9.30 against
# 8.43 us at p50) - no rmw_tickle user code runs between the tap and the receive's return.
#
# Usage (from a durable checkout at a commit holding this script, both SHAs pushed to origin):
#   TS=$(date +%Y%m%d-%H%M%S)
#   setsid nohup env P_SHA=<parent> B_SHA=<branch> examples/perf_hil/experiments/kh2_rig_ab.sh \
#     > ~/rig_results_safe/kh2ab_$TS.log 2>&1 < /dev/null &
#   DRY_RUN=1 checks the SHAs and the readers without the lock.
# ESTIMATE: KH2's whole call took ~52 min for both QoS; best_effort only ~26 min an arm, ~55 min under the lock.
# Results: ~/rig_results_safe/kh2ab_<arm>_<sha8>_<stamp>.* and kh2ab_<stamp>.reading.txt. Last line "KH2AB_END rc=<n>".
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
SAFE=$HOME/rig_results_safe
P_SHA=${P_SHA:?P_SHA=<parent sha>}
B_SHA=${B_SHA:?B_SHA=<branch sha>}
TS=${TS:-$(date +%Y%m%d-%H%M%S)}
rc=1
if [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    trap 'echo "KH2AB_END rc=$rc $(date -Is)"' EXIT
    git -C "$REPO" fetch -q origin || { echo "REFUSED: git fetch"; exit 1; }
    for s in "$P_SHA" "$B_SHA"; do
        full=$(git -C "$REPO" rev-parse --verify -q "$s^{commit}") || { echo "REFUSED: $s is not a commit here"; exit 1; }
        [ -n "$(git -C "$REPO" branch -r --contains "$full" 2>/dev/null)" ] || { echo "REFUSED: $s is on no origin branch (the Pis fetch it)"; exit 1; }
    done
    python3 "$HERE/s1_kh_rig_read.py" --selftest | tail -1 | grep -q "SELFTEST OK" || { echo "REFUSED: s1_kh_rig_read.py --selftest"; exit 1; }
    if [ "${DRY_RUN:-0}" = 1 ]; then
        echo "DRY_RUN clean: P ${P_SHA:0:8}, B ${B_SHA:0:8}"
        rc=0
        exit 0
    fi
    export P_SHA B_SHA TS RIG_LOCK_SCOPE=hil
    "$REPO/examples/perf_hil/rig_lock.sh" "${BASH_SOURCE[0]}"
    rc=$?
    exit "$rc"
fi
# ---- under the lock ----
for arm in P B; do
    sha=$P_SHA
    [ "$arm" = B ] && sha=$B_SHA
    o=$SAFE/kh2ab_${arm}_${sha:0:8}_$TS.txt
    echo "--- $arm ${sha:0:8} -> $o ($(date -Is))"
    SHA=$sha ROTATE=1 WAITS=block REPS=3 MSGS=bench QOSES=best_effort TICKLE_VARIANTS="default libc" \
        SYSSTAMP_ARMS="off on" CAPTURE=1 STAMPS=1 OUT=$o "$HERE/rmw_crosshost_rtt.sh" >"$o.launch.log" 2>&1
    echo "  rc=$? rows ok: $(grep -c ' | ok | ' "$o" 2>/dev/null)"
    timeout 600 python3 "$HERE/rmw_pcap_split.py" "$o" >"${o%.txt}.split.txt" 2>&1
    stems=$(grep -o 'rmw_tickle@libc_bench_best_effort_rep[0-9]*_block_sst' "${o%.txt}.split.txt" | sort -u)
    # shellcheck disable=SC2086 # one stem a word
    timeout 300 python3 "$HERE/kh2_pong_calls.py" "$o" $stems >"${o%.txt}.calls.txt" 2>&1
    sed 's/^/  /' "${o%.txt}.calls.txt"
done
R=$SAFE/kh2ab_$TS.reading.txt
timeout 120 python3 "$HERE/s1_kh_rig_read.py" --kh "$SAFE/kh2ab_B_${B_SHA:0:8}_$TS.split.txt" \
    --prev "$SAFE/kh2ab_P_${P_SHA:0:8}_$TS.split.txt" >"$R" 2>&1
rc=$?
cat "$R"
exit "$rc"

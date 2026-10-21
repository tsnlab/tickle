#!/usr/bin/env bash
# rmw_holdover_rig.sh - the rmw cross-host block-wait rows and the Pi's kernel wake split, re-measured after the
# executor hold-over (58bd6b1a: an rmw_wait() that found something keeps the poll role). Written 2026-10-21 before
# any of its runs. Two parts, one rig lock, the three rmws in an order rotated every repetition (ROTATE=1):
#
#   W  docs/RESULTS.md rows 48c-51c (block wait, Bench and Array1k, BEST_EFFORT and RELIABLE; src Wc at 29826912):
#      rmw_crosshost_rtt.sh WAITS=block REPS=3 MSGS="bench array1k", rmw_tickle / rmw_fastrtps_cpp /
#      rmw_cyclonedds_cpp. Read by rmw_crosshost_creation.py (its C1-C3: samples first, the R4 control, the range
#      rule the rows were published with). The hold-over removed a timerfd_settime() between the datagram and its
#      callback (0.61 -> 0.06 us on the PC), so the expectation is "within noise of 48c-51c"; the PC did not resolve
#      the round trip, and this is where the rig decides.
#   K  the ROADMAP item "rmw kernel wake (~6 us)": in September the pong's tap -> receive return read 18.2 us for
#      rmw_tickle against CycloneDDS's 12.6. rmw_crosshost_rtt.sh WAITS=block REPS=3 MSGS=bench, CAPTURE=1 STAMPS=1,
#      SYSSTAMP_ARMS="off on", TICKLE_VARIANTS="default libc": rmw_tickle as built for W and again with
#      -DTT_HAL_LIBC_CALLS (its raw tt_sys6 system calls bypass sysstamp's LD_PRELOAD on aarch64; the libc build makes
#      them visible), beside both vendors. Read by rmw_pcap_split.py's wake summary (its K0-K3: samples first, the
#      wire control, the range rule on per-rep "pong tap->recv" and "pong tap->wake", sysstamp's own cost). The
#      default build's sst rows are expected NOT READ (K0) - that is the check that the variant is the treatment.
#
# Guards, enforced here before the lock: SHA (default origin/main) must be on origin/main (fetched), contain the
# hold-over 58bd6b1a, give ping_node --dues and keep TT_HAL_LIBC_CALLS in hal_linux.c; the harness and readers come
# from RUNNER, a detached worktree at SHA (so the scripts that run are the ones on origin/main); the readers' selftest
# passes. Under the lock: refuses if either Pi's eth0 carries a netem or tbf qdisc (this script shapes nothing).
# The rig lock (scope hil) is taken ONCE, by this process, for both parts (RIG_LOCK_WAIT 36000 s).
# One EXIT trap: reports any ping_node/pong_node/rmw_zenohd left on the Pis (by /proc/PID/exe) and prints the last
# line, always "RMW_HOLDOVER_END rc=<n> ...", after "RMW_HOLDOVER_DONE ..." on completion.
#
# ESTIMATE: W: the Pi builds ~15 min + 36 rows x ~30 s = ~35 min; K: rebuild + the libc variant ~10 min + 48 rows x
# ~40 s (capture) = ~42 min; ~1 h 20 min in all. Analyse an overrun at 2 h.
#
# Usage (from any checkout of this repository; nothing here needs it to be clean):
#   setsid nohup examples/perf_hil/experiments/rmw_holdover_rig.sh > ~/rig_results_safe/rmw_holdover_<date>.log 2>&1 < /dev/null &
#   env: SHA (origin/main), PARTS ("W K"), RUNNER (/home/semih/tickle_runner_holdover), W_REPS (3), K_REPS (3)
set -u
SELF_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PARTS=${PARTS:-W K}
RUNNER=${RUNNER:-/home/semih/tickle_runner_holdover}
W_REPS=${W_REPS:-3}
K_REPS=${K_REPS:-3}
HOLDOVER=58bd6b1a
SAFE=$HOME/rig_results_safe
RMW3="rmw_tickle rmw_fastrtps_cpp rmw_cyclonedds_cpp"
say() { echo "$(date -Is) $*"; }
case "$W_REPS$K_REPS" in *[!0-9]*) say "REFUSED: W_REPS and K_REPS are whole numbers"; exit 64 ;; esac
for p in $PARTS; do case "$p" in W | K) ;; *) say "REFUSED: PARTS takes W and/or K, not '$p'"; exit 64 ;; esac; done

if [ "${HOLDOVER_LOCKED:-0}" != 1 ]; then
    trap 'echo "RMW_HOLDOVER_END rc=$? (before the lock) $(date -Is)"' EXIT
    git -C "$SELF_REPO" fetch -q origin main || { say "REFUSED: git fetch failed"; exit 1; }
    SHA=$(git -C "$SELF_REPO" rev-parse --verify --quiet "${SHA:-origin/main}^{commit}") ||
        { say "REFUSED: unknown SHA '${SHA:-}'"; exit 1; }
    git -C "$SELF_REPO" merge-base --is-ancestor "$SHA" origin/main ||
        { say "REFUSED: $SHA is not on origin/main (the Pis fetch it)"; exit 1; }
    git -C "$SELF_REPO" merge-base --is-ancestor "$HOLDOVER" "$SHA" ||
        { say "REFUSED: $SHA does not contain the hold-over $HOLDOVER"; exit 1; }
    git -C "$SELF_REPO" show "$SHA:rmw_tickle/rmw_perf_pingpong/src/ping_node.cpp" | grep -q -- '"--dues"' ||
        { say "REFUSED: ping_node at $SHA has no --dues (no creation time)"; exit 1; }
    git -C "$SELF_REPO" show "$SHA:src/hal_linux.c" | grep -q 'TT_HAL_LIBC_CALLS' ||
        { say "REFUSED: hal_linux.c at $SHA has no TT_HAL_LIBC_CALLS (the libc variant would change nothing)"; exit 1; }
    if [ ! -d "$RUNNER" ]; then
        git -C "$SELF_REPO" worktree add -q --detach "$RUNNER" "$SHA" || { say "REFUSED: worktree $RUNNER"; exit 1; }
    fi
    git -C "$RUNNER" checkout -q --detach "$SHA" || { say "REFUSED: checkout of $SHA in $RUNNER"; exit 1; }
    X=$RUNNER/examples/perf_hil/experiments
    if ! grep -q 'ROTATE' "$X/rmw_crosshost_rtt.sh" || ! grep -q 'libc)' "$X/rmw_crosshost_rtt.sh"; then
        say "REFUSED: rmw_crosshost_rtt.sh at $SHA has no ROTATE or no libc variant"; exit 1
    fi
    if ! grep -q 'def wake_summary' "$X/rmw_pcap_split.py" || ! grep -q '"epoll_pwait2"' "$X/rmw_pcap_split.py"; then
        say "REFUSED: rmw_pcap_split.py at $SHA has no wake summary or no epoll_pwait2"; exit 1
    fi
    grep -q 'CALL_EPOLL_PWAIT2' "$X/sysstamp/sysstamp.c" || { say "REFUSED: sysstamp.c at $SHA records no epoll_pwait2"; exit 1; }
    python3 "$X/creation_latency_summary.py" --selftest | tail -1 | grep -qx 'selftest: PASS' ||
        { say "REFUSED: creation_latency_summary.py's selftest does not pass"; exit 1; }
    say "=== rmw hold-over re-measure: parts '$PARTS', SHA ${SHA:0:8}, harness in $RUNNER, W reps $W_REPS, K reps $K_REPS ==="
    say "uptime: $(uptime)"
    say "--- checks passed; taking the rig lock (hil), once for both parts ---"
    trap - EXIT
    export SHA PARTS RUNNER W_REPS K_REPS HOLDOVER_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
    exec "$RUNNER/examples/perf_hil/rig_lock.sh" "$RUNNER/examples/perf_hil/experiments/rmw_holdover_rig.sh"
fi

# ---- under the lock ----------------------------------------------------------------------------------------------
cd "$RUNNER" || exit 1
X=$RUNNER/examples/perf_hil/experiments
K=$HOME/.ssh/tickle_ci_ed25519
PIS="10.1.1.214 10.1.1.213"
pi() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "$2" </dev/null; }
# shellcheck disable=SC2329 # invoked by the EXIT trap below
finish() { # the one EXIT trap: this script applied nothing to the Pis; it reports what bench process is left there
    local rc=$? h
    for h in $PIS; do
        # shellcheck disable=SC2016 # the $ are for the Pi's shell
        say "leftovers on $h: $(pi "$h" 'for q in /proc/[0-9]*; do e=$(readlink $q/exe 2>/dev/null); case "$e" in
            */ping_node|*/pong_node|*/rmw_zenohd) echo "${q#/proc/}=$e";; esac; done' 2>&1 | tr '\n' ' ')"
    done
    echo "RMW_HOLDOVER_END rc=$rc $(date -Is)"
}
trap finish EXIT
say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
for h in $PIS; do
    q=$(pi "$h" "tc qdisc show dev eth0") || { say "REFUSED: cannot read $h eth0's qdisc"; exit 1; }
    case "$q" in *netem* | *tbf*) say "REFUSED: $h eth0 is shaped ($q)"; exit 1 ;; esac
    say "qdisc $h eth0: $(echo "$q" | head -1)"
done
TS=$(date +%Y%m%d-%H%M%S)
rc_all=0

case " $PARTS " in *" W "*)
    W=$SAFE/holdover_rmwW_${SHA:0:8}_$TS.txt
    say "--- W: rmw cross-host block wait (rows 48c-51c) at ${SHA:0:8}, rotated -> $W ---"
    SHA=$SHA ROTATE=1 WAITS=block REPS=$W_REPS MSGS="bench array1k" RMW_LIST="$RMW3" OUT=$W \
        "$X/rmw_crosshost_rtt.sh" >"$W.launch.log" 2>&1
    rc=$?; [ "$rc" = 0 ] || rc_all=1
    say "  W rc=$rc rows ok: $(grep -c ' | ok | ' "$W") of $(grep -cE '^rmw_[a-z_]+ (bench|array1k) ' "$W")"
    python3 "$X/rmw_crosshost_creation.py" "$W" >"${W%.txt}.creation.txt" 2>&1 || rc_all=1
    say "  W reading -> ${W%.txt}.creation.txt: $(grep -c '^  CONTROL PASS' "${W%.txt}.creation.txt") cells CONTROL" \
        "PASS, $(grep -c '^  CONTROL FAIL' "${W%.txt}.creation.txt") FAIL"
    ;;
esac

case " $PARTS " in *" K "*)
    KO=$SAFE/holdover_rmwK_${SHA:0:8}_$TS.txt
    say "--- K: the Pi's kernel wake split (sysstamp off/on, rmw_tickle default + libc, capture) -> $KO ---"
    SHA=$SHA ROTATE=1 WAITS=block REPS=$K_REPS MSGS=bench TICKLE_VARIANTS="default libc" SYSSTAMP_ARMS="off on" \
        CAPTURE=1 STAMPS=1 OUT=$KO "$X/rmw_crosshost_rtt.sh" >"$KO.launch.log" 2>&1
    rc=$?; [ "$rc" = 0 ] || rc_all=1
    say "  K rc=$rc rows ok: $(grep -c ' | ok | ' "$KO") of $(grep -cE '^rmw_[a-z_@]+ bench ' "$KO")"
    python3 "$X/rmw_pcap_split.py" "$KO" >"${KO%.txt}.split.txt" 2>&1 || rc_all=1
    say "  K reading -> ${KO%.txt}.split.txt:"
    sed -n '/^=== kernel wake summary/,$p' "${KO%.txt}.split.txt"
    ;;
esac
say "RMW_HOLDOVER_DONE rc=$rc_all"
exit "$rc_all"

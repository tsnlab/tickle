#!/usr/bin/env bash
# The round-trip rows still read from the send stamp, re-measured from creation time (the user's decision of
# 2026-10-17: latency = arrival - the time the data was due; the send stamp stays as a diagnostic; TESTING.md
# section 4). Written 2026-10-17 before any of its runs. Two parts, so the one that needs nothing pushed can run
# before the one that does:
#
#   PARTS=samehost  RESULTS S7-S10, S12, S18 (same host, the core reliable_latency clients, which print crt_* beside
#                   rtt_* since 4fca013b). fair_samehost_remeasure.sh, free arm only (the headline; pinned is a note),
#                   3 reps per framework: reliable_latency p2 / p3 / p4 at 200 pings/s for 10 s (S7, S9, S10, S18)
#                   and p2 at 20 pings/s for 60 s (S8, S12). The Pis build CORE_SHA (default origin/main): no push.
#                   Read by fair_samehost_summary.py (its C1-C3; C2 is creation_latency_summary.py's R4).
#   PARTS=rmw       rmw rows 48-67 (cross-host) and R1-R12 (same host): rmw_perf_pingpong's ping_node records each
#                   ping's due time since this branch (RESULT crt_* fields, --dues file), so the Pis must build a
#                   commit that has it: RMW_SHA, which must be on origin/main (the Pis fetch it).
#                     W  rmw_crosshost_rtt.sh WAITS=block, bench + array1k, both QoS, 4 rmws, 3 reps   (rows 48-51)
#                     J  rmw_crosshost_rtt.sh WAITS=poll POLL_SLEEPS="0 50 100 200" PHASE=jitter RTT_AT=callback,
#                        J_REPS reps (7, as published), SKIP_BUILD=1 after W                       (rows 52-67)
#                     R  rmw_samehost.sh, rtt cells only (TPUT_TOPICS empty: R13-R21 are throughput rows and are not
#                        latency), its rig dry run first, 3 reps                                     (rows R1-R12)
#                   Read by rmw_crosshost_creation.py (W, J; its C1-C3) and rmw_samehost_summary.py (R; its C1-C3).
#
# Every reader scores rmw_tickle / TickLE against each vendor on the creation figure the row publishes (a mean in
# every one of these rows) by the row's own rule, prints the send-stamp verdict beside it, and gates the creation
# verdicts on the R4 control (nothing waits at a ping-pong sender, so creation and stamp differ by the sender's timer
# wake alone). Nothing is written into docs/RESULTS.md by this script.
#
# THE RIG. The lock is taken ONCE, inside this detached process, for every cell (rig_lock.sh, RIG_LOCK_WAIT 36000).
# This script applies nothing to the Pis itself: no tc, no source patch, no setting. The harnesses it runs build on the
# Pis (a checkout of a commit, as every rig run does) and clean up their own processes in their own traps. Its one
# EXIT trap reports what is left on the Pis (any ping_node/pong_node/server/client/perf_test/rmw_zenohd process, by
# /proc/PID/exe) and stops those, then prints the last line.
#
# ESTIMATE. samehost: the PC preflight ~5 min, then (s6 runs each framework's ON and OFF arm, the OFF arm being the
# transport witness's denominator) 3 cells x 18 runs x ~25 s (23 min) + the 20/s cell 18 x ~75 s (23 min) + the Pi
# builds ~5 min = ~55 min; analyse an overrun at 80 min. rmw: the PC preflight build ~5 min + its
# netns run ~3 min; W: the Pi builds ~15 min + 48 rows x ~30 s (24 min); J: 4 x 16 x J_REPS rows x ~30 s (7 reps:
# 3 h 45 min); R: build + dry run ~15 min + 48 runs x ~45 s (36 min) = ~5 h with J_REPS=7, ~3 h with J_REPS=3; analyse
# an overrun at 1.5x.
#
# Usage (from a checkout that has this script; RUNNER is a detached worktree it runs from, at HARNESS_SHA):
#   PARTS=samehost setsid nohup examples/perf_hil/experiments/rig_queue_creation_rtt.sh \
#     > ~/rig_results_safe/creation_rtt_queue.log 2>&1 < /dev/null &
#   PARTS=rmw RMW_SHA=<pushed sha with ping_node --dues> setsid nohup ... &
# HARNESS_SHA defaults to RMW_SHA when set, else this checkout's HEAD (the PC-side scripts and readers; it need not be
# pushed for PARTS=samehost). The last line is always "CREATION_RTT_QUEUE rc=<n> ...".
set -u
SELF_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PARTS=${PARTS:-samehost}
RMW_SHA=${RMW_SHA:-}
HARNESS_SHA=${HARNESS_SHA:-${RMW_SHA:-$(git -C "$SELF_REPO" rev-parse HEAD)}}
RUNNER=${RUNNER:-/home/semih/tickle_runner_crtrtt}
J_REPS=${J_REPS:-7}
SAFE=$HOME/rig_results_safe
X=$RUNNER/examples/perf_hil/experiments
say() { echo "$(date -Is) $*"; }
SAMEHOST_CELLS="reliable_latency:p2 reliable_latency:p3 reliable_latency:p4 reliable_latency:p2:0.05:60"
mkdir -p "$SAFE/preflight"

if [ "${CRTRTT_LOCKED:-0}" != 1 ]; then
    trap 'echo "CREATION_RTT_QUEUE rc=$? (before the lock) $(date -Is)"' EXIT
    for p in $PARTS; do
        case "$p" in samehost | rmw) ;; *) say "REFUSED: PARTS takes samehost and/or rmw, not '$p'"; exit 2 ;; esac
    done
    git -C /home/semih/tickle fetch -q origin || { say "REFUSED: git fetch failed"; exit 1; }
    CORE_SHA=${CORE_SHA:-$(git -C /home/semih/tickle rev-parse origin/main)}
    git -C /home/semih/tickle merge-base --is-ancestor "$CORE_SHA" origin/main ||
        { say "REFUSED: CORE_SHA $CORE_SHA is not on origin/main (the Pis fetch it)"; exit 1; }
    git -C "$SELF_REPO" cat-file -e "$HARNESS_SHA^{commit}" 2>/dev/null ||
        { say "REFUSED: HARNESS_SHA $HARNESS_SHA is not in $SELF_REPO"; exit 1; }
    case " $PARTS " in *" rmw "*)
        [ -n "$RMW_SHA" ] || { say "REFUSED: PARTS=rmw needs RMW_SHA, a pushed commit whose ping_node has --dues"; exit 1; }
        git -C /home/semih/tickle merge-base --is-ancestor "$RMW_SHA" origin/main ||
            { say "REFUSED: RMW_SHA $RMW_SHA is not on origin/main (the Pis fetch it)"; exit 1; }
        git -C /home/semih/tickle show "$RMW_SHA:rmw_tickle/rmw_perf_pingpong/src/ping_node.cpp" | grep -q -- '"--dues"' ||
            { say "REFUSED: ping_node at $RMW_SHA has no --dues (it would record no creation time)"; exit 1; } ;;
    esac
    if [ ! -d "$RUNNER" ]; then
        git -C "$SELF_REPO" worktree add -q --detach "$RUNNER" "$HARNESS_SHA" || { say "REFUSED: worktree"; exit 1; }
    fi
    git -C "$RUNNER" checkout -q --detach "$HARNESS_SHA" || { say "REFUSED: checkout of $HARNESS_SHA in $RUNNER"; exit 1; }
    for f in rmw_crosshost_creation.py fair_samehost_summary.py rmw_samehost_summary.py creation_latency_summary.py; do
        [ -f "$X/$f" ] || { say "REFUSED: $HARNESS_SHA has no $f"; exit 1; }
    done
    python3 "$X/creation_latency_summary.py" --selftest | tail -1 | grep -qx 'selftest: PASS' ||
        { say "REFUSED: creation_latency_summary.py's selftest does not pass"; exit 1; }
    say "=== creation-time RTT queue: parts '$PARTS', core ${CORE_SHA:0:8}, rmw ${RMW_SHA:0:8}, harness" \
        "${HARNESS_SHA:0:8} in $RUNNER, J reps $J_REPS ==="
    say "uptime: $(uptime)"
    if [ "${PREFLIGHT:-1}" = 1 ]; then
        case " $PARTS " in *" samehost "*)
            specs=()
            for cell in $SAMEHOST_CELLS; do
                specs+=("$(echo "$cell" | cut -d: -f1-2):N0:-i $(echo "$cell:0.005" | cut -d: -f3):-Q")
            done
            pfd=$SAFE/preflight/creation_rtt_samehost_$(date +%Y%m%d-%H%M%S)
            say "--- PC preflight, same-host latency cells at ${CORE_SHA:0:8} -> $pfd ---"
            PREFLIGHT_OUT=$pfd SHA=$CORE_SHA PREFLIGHT_TOPO=samens FWS="tickle fastdds cyclonedds" \
                "$X/rig_preflight.sh" "${specs[@]}" > "$pfd.log" 2>&1 || { say "PREFLIGHT FAILED (same host): $pfd.log"; exit 1; }
            # The treatment must be in the clients before the rig is spent: every preflight client line has crt_*.
            n_res=$(cat "$pfd"/jobs/*reliable_latency*/client.log 2>/dev/null | grep -c '^RESULT:.*scenario=reliable_latency')
            n_crt=$(cat "$pfd"/jobs/*reliable_latency*/client.log 2>/dev/null | grep -c '^RESULT:.*crt_avg_ms=')
            if [ "$n_res" = 0 ] || [ "$n_crt" != "$n_res" ]; then
                say "STOPPED: $n_crt of $n_res preflight latency client RESULT lines carry crt_* ($pfd)"
                exit 1
            fi
            say "--- PC preflight (same host) PASS: $n_crt of $n_res client lines with crt_* ---" ;;
        esac
        case " $PARTS " in *" rmw "*)
            say "--- PC preflight build of the rmw stack at ${HARNESS_SHA:0:8} (-j4) ---"
            MAKEFLAGS=-j4 TMPDIR=/home/semih/tmp_build "$X/pc_preflight_build.sh" \
                > "$SAFE/preflight/creation_rtt_pfbuild_$(date +%Y%m%d-%H%M%S).log" 2>&1 ||
                { say "PREFLIGHT BUILD FAILED"; exit 1; }
            pfo=$SAFE/preflight/creation_rtt_rmwsh_$(date +%Y%m%d-%H%M%S)
            PREFLIGHT_ONLY=1 PREFLIGHT_OUT=$pfo ARMS="tickle fastdds cyclonedds zenoh" TPUT_TOPICS="" \
                PF_CELLS="rtt:bench:reliable:block rtt:array1k:best_effort:poll" "$X/rmw_samehost.sh" \
                > "$pfo.log" 2>&1 || { say "PREFLIGHT FAILED (rmw same host): $pfo.log"; exit 1; }
            # The treatment must be in the binary before hours are spent: every preflight rtt run left a dues file.
            pfr=$pfo.runs
            n_rtt=$(find "$pfr" -maxdepth 1 -type d -name 'rtt_*' | wc -l)
            n_dues=$(find "$pfr" -mindepth 2 -maxdepth 2 -name dues.txt | wc -l)
            if [ "$n_rtt" = 0 ] || [ "$n_dues" != "$n_rtt" ]; then
                say "STOPPED: $n_dues of $n_rtt preflight rtt runs in $pfr have a dues file - ping_node without --dues?"
                exit 1
            fi
            say "--- PC preflight (rmw same host) PASS: $n_dues of $n_rtt rtt runs with a dues file ($pfr) ---" ;;
        esac
    fi
    say "--- taking the rig lock (hil), once for every cell ---"
    trap - EXIT
    export CTL_PARTS="$PARTS" CORE_SHA RMW_SHA HARNESS_SHA RUNNER J_REPS CRTRTT_LOCKED=1 RIG_LOCK_SCOPE=hil \
        RIG_LOCK_WAIT=36000 TMPDIR=/home/semih/tmp_build
    exec "$RUNNER/examples/perf_hil/rig_lock.sh" "$RUNNER/examples/perf_hil/experiments/rig_queue_creation_rtt.sh" "$@"
fi

# ---- under the lock ----------------------------------------------------------------------------------------------
cd "$RUNNER" || exit 1
K=$HOME/.ssh/tickle_ci_ed25519
PIS="10.1.1.214 10.1.1.213"
pi() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "$2" < /dev/null; }
finish() { # the one EXIT trap. It applied nothing to the Pis, so it reverts nothing; it reports any bench process
    # still alive there (by /proc/PID/exe; the harnesses stop their own, and the next run's leftover check VOIDs on one)
    local rc=$? h
    for h in $PIS; do
        # shellcheck disable=SC2016 # the $ are for the Pi's shell
        say "leftovers on $h: $(pi "$h" 'for q in /proc/[0-9]*; do e=$(readlink $q/exe 2>/dev/null); case "$e" in
            */ping_node|*/pong_node|*/perf_test|*/rmw_zenohd|/home/ci/*/client|/home/ci/*/server) echo "${q#/proc/}=$e";;
            esac; done' 2>&1 | tr '\n' ' ')"
    done
    echo "CREATION_RTT_QUEUE rc=$rc $(date -Is)"
}
trap finish EXIT
say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
TS=$(date +%Y%m%d-%H%M%S)

case " $CTL_PARTS " in *" samehost "*)
    OUTB=$SAFE/creation_rtt_samehost_${CORE_SHA:0:8}_$TS
    say "--- same host (S7-S10, S12, S18) at ${CORE_SHA:0:8} -> $OUTB.* ---"
    SHA=$CORE_SHA CELLS="$SAMEHOST_CELLS" ARMS=free REPS=3 PREFLIGHT=0 OUTB=$OUTB \
        "$X/fair_samehost_remeasure.sh" > "$OUTB.launch.log" 2>&1
    say "  same host rc=$?"
    python3 "$X/fair_samehost_summary.py" "$OUTB" > "$OUTB.summary.txt" 2>&1
    say "  same host reading -> $OUTB.summary.txt: $(grep -cE '^  CONTROL PASS' "$OUTB.summary.txt") of 4 cells" \
        "CONTROL PASS"
    ;;
esac

case " $CTL_PARTS " in *" rmw "*)
    RMW4="rmw_tickle rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_zenoh_cpp"
    W=$SAFE/creation_rtt_rmwW_${RMW_SHA:0:8}_$TS.txt
    say "--- rmw cross-host block wait (rows 48-51) at ${RMW_SHA:0:8} -> $W ---"
    SHA=$RMW_SHA WAITS=block REPS=3 MSGS="bench array1k" RMW_LIST="$RMW4" OUT=$W \
        "$X/rmw_crosshost_rtt.sh" > "$W.launch.log" 2>&1
    say "  W rc=$? rows ok: $(grep -c ' | ok | ' "$W")"
    J=$SAFE/creation_rtt_rmwJ_${RMW_SHA:0:8}_$TS.txt
    say "--- rmw cross-host poll wait (rows 52-67), $J_REPS reps -> $J ---"
    SHA=$RMW_SHA SKIP_BUILD=1 WAITS=poll POLL_SLEEPS="0 50 100 200" PHASE=jitter RTT_AT=callback REPS=$J_REPS \
        MSGS="bench array1k" RMW_LIST="$RMW4" OUT=$J "$X/rmw_crosshost_rtt.sh" > "$J.launch.log" 2>&1
    say "  J rc=$? rows ok: $(grep -c ' | ok | ' "$J")"
    python3 "$X/rmw_crosshost_creation.py" "$W" "$J" > "${W%.txt}.creation.txt" 2>&1
    say "  cross-host reading -> ${W%.txt}.creation.txt: $(grep -c '^  CONTROL PASS' "${W%.txt}.creation.txt") cells" \
        "CONTROL PASS, $(grep -c '^  CONTROL FAIL' "${W%.txt}.creation.txt") FAIL"
    R=$SAFE/creation_rtt_rmwsh_${RMW_SHA:0:8}_$TS
    say "--- rmw same host (R1-R12) -> $R.* ---"
    SHA=$RMW_SHA TPUT_TOPICS="" REPS=3 ARMS="tickle fastdds cyclonedds zenoh" PREFLIGHT=0 OUT=$R \
        "$X/rmw_samehost.sh" > "$R.launch.log" 2>&1
    say "  R rc=$? -> $R.txt (its summary, creation rows included, is at the end of it)"
    ;;
esac
say "=== creation-time RTT queue done ==="

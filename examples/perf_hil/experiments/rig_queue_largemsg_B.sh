#!/usr/bin/env bash
# Large-message step B (docs/DESIGN.md section 8, "Step B pre-registration", B2): the rig task. Written 2026-10-11
# before any of its runs; the readings are enforced by largemsg_stepb_compare.py (its docstring is the rule). Not run
# by the session that wrote it: it needs pushed SHAs, which it refuses to start without.
#
#   MAIN_SHA  = origin/main with step A and without step B (the control arm, tickle@pre)
#   STEPB_SHA = the step B branch head, pushed (tickle@head); MAIN_SHA must be its ancestor
#   HARNESS_REF: a checkout that differs from STEPB_SHA only under examples/perf_hil/experiments/ (checked)
#
# CELLS, all same-host on the client Pi (SAMEHOST=1), 30 Hz, 60 s, 5 reps, arms interleaved and rotated per rep:
#   b2a  RELIABLE KEEP_LAST 10:            Array1m:0 Array4m:0 Array16k:0
#   b2b  sensor_data (BEST_EFFORT KEEP_LAST 5): Array1m:0 Array4m:0 Array16k:0
# Arms: tickle@pre (MAIN_SHA, step A), tickle@head (STEPB_SHA), fastdds and cyclonedds at their shipped same-host
# defaults (Fast DDS data-sharing, CycloneDDS as shipped). Array16k is the control: below 64 KB, no step B code on its
# path (DESIGN.md B2 named Array60k; perf_test's 64 KB type is itself a large sample and the summary knows no 60k).
# The vendor-favouring labelled arms (CycloneDDS iceoryx) are a second queue, after this one is read.
#
# READ BY largemsg_stepb_compare.py reliable=<b2a>.txt best_effort=<b2b>.txt (the selftest runs first and must PASS):
# CONTROL, B2.1 (BE delivered >= 99.9%), B2.2 (RELIABLE 100%), B2.3 (sub+pub CPU/MB below step A beyond 2 x SE in the
# RELIABLE cells - failing it, L3 drops step B), B2.4 (latency median not worse), B2.5 (vs each DDS), B2.6 (RSS).
#
# ESTIMATE (A4 stage 4: a 60 s run ~73 s, a build ~3 min): 2 x (4 arms x 3 cells x 5 reps) = 120 runs + 4 builds
# = ~2 h 40 min; analyse an overrun at 4 h. PC preflight (PC_PREFLIGHT=1, REPS=1 DUR=6, both QoS) ~10 min first.
#
# Usage (when the rig is free; the lock is taken once, inside, for both stages):
#   MAIN_SHA=<sha> STEPB_SHA=<sha> HARNESS_REF=<ref> \
#     setsid nohup examples/perf_hil/experiments/rig_queue_largemsg_B.sh > ~/rig_results_safe/queue_largemsgB.log 2>&1 \
#     < /dev/null &
#   PREFLIGHT_ONLY=1 runs the checks and the PC preflight and stops before the lock.
set -u
MAIN_SHA=${MAIN_SHA:?set MAIN_SHA: origin/main with step A, without step B}
STEPB_SHA=${STEPB_SHA:?set STEPB_SHA: the pushed step B head}
HARNESS_REF=${HARNESS_REF:?set HARNESS_REF: the harness checkout ref}
RUNNER=${RUNNER:-/home/semih/tickle_runner_b}
SAFE=$HOME/rig_results_safe
PF_MAIN=${PF_MAIN:-/home/semih/pc_preflight_builds/${MAIN_SHA:0:12}}
PF_STEPB=${PF_STEPB:-/home/semih/pc_preflight_builds/${STEPB_SHA:0:12}}
X=$RUNNER/examples/perf_hil/experiments
TAG=largemsgB
CELLS_B="Array1m:0 Array4m:0 Array16k:0"
say() { echo "$(date -Is) $*"; }
mkdir -p "$SAFE"

if [ "${B_LOCKED:-0}" != 1 ]; then
    git -C /home/semih/tickle fetch -q origin || { say "REFUSED: git fetch failed"; exit 1; }
    if [ ! -d "$RUNNER" ]; then
        git -C /home/semih/tickle worktree add -q --detach "$RUNNER" "$HARNESS_REF" || { say "REFUSED: worktree"; exit 1; }
    fi
    git -C "$RUNNER" checkout -q --detach "$HARNESS_REF" || { say "REFUSED: checkout"; exit 1; }
    H=$(git -C "$RUNNER" rev-parse HEAD)
    say "=== largemsg B queue: main=${MAIN_SHA:0:8} stepB=${STEPB_SHA:0:8} harness=${H:0:8} ==="
    git -C "$RUNNER" merge-base --is-ancestor "$MAIN_SHA" "$STEPB_SHA" || { say "REFUSED: main is not under step B"; exit 1; }
    git -C "$RUNNER" merge-base --is-ancestor "$STEPB_SHA" "$H" || { say "REFUSED: the harness is not on step B"; exit 1; }
    other=$(git -C "$RUNNER" diff --name-only "$STEPB_SHA" "$H" | grep -v '^examples/perf_hil/experiments/')
    [ -z "$other" ] || { say "REFUSED: the harness changes more than experiments/: $other"; exit 1; }
    [ -n "$(git -C "$RUNNER" diff --name-only "$MAIN_SHA" "$STEPB_SHA" -- src/tickle.c)" ] ||
        { say "REFUSED: main and step B have the same core (the arms would be one build)"; exit 1; }
    python3 "$X/largemsg_stepb_compare.py" --selftest | tail -1 | grep -qx 'SELFTEST PASS' ||
        { say "REFUSED: the reader's selftest does not pass"; exit 1; }
    if [ "${PREFLIGHT:-1}" = 1 ]; then
        say "--- PC preflight ---"
        PFD=$SAFE/preflight/${TAG}_$(date +%Y%m%d-%H%M%S)
        mkdir -p "$PFD"
        for b in "$PF_MAIN" "$PF_STEPB"; do
            [ -f "$b/BUILD_OK" ] || { say "PREFLIGHT FAILED: no PC build $b (run pc_preflight_build.sh at its commit)"; exit 1; }
        done
        if ! grep -qx "sha=$MAIN_SHA" "$PF_MAIN/BUILD_OK" || ! grep -qx "sha=$STEPB_SHA" "$PF_STEPB/BUILD_OK"; then
            say "PREFLIGHT FAILED: the PC builds are not of MAIN_SHA and STEPB_SHA"
            exit 1
        fi
        for spec in "b2a|--reliable --keep_last --history_depth 10" "b2b|--keep_last --history_depth 5"; do
            IFS='|' read -r name qa <<<"$spec"
            env SAMEHOST=1 PC_PREFLIGHT=1 PF_TICKLE_INSTALL="$PF_STEPB/install" PF_PERF_WS="$PF_STEPB/perf/install" \
                PF_VARIANT_pre="$PF_MAIN/install" PRE_SHA="$MAIN_SHA" HEAD_SHA="$STEPB_SHA" REPS=1 DUR=6 \
                ARMS="tickle@pre tickle@head fastdds cyclonedds" CELLS="$CELLS_B" QOS_ARGS="$qa" PUB_RATE=30 \
                SUMMARY="$X/largemsg_rig_summary.py" OUT="$PFD/$name" "$X/rmw_keepall_rig.sh" > "$PFD/$name.launch.log" 2>&1
            python3 "$X/largemsg_rig_summary.py" "$PFD/$name.runs" 6 --preflight > "$PFD/$name.check.txt" 2>&1
            rc=$?
            say "  $name: $(grep -E '^PREFLIGHT ' "$PFD/$name.check.txt" | head -1) (rc $rc)"
            [ "$rc" = 0 ] || { say "PREFLIGHT FAILED: $PFD/$name.check.txt"; exit 1; }
            # The treatment, seen: step B's same-host publisher must have sent descriptors, step A's none.
            grep -q 'large_ref: sent=[1-9]' "$PFD/$name.runs"/tickle@head_Array4m_*_pub.log ||
                { say "PREFLIGHT FAILED: $name tickle@head sent no descriptor (step B not applied)"; exit 1; }
            ! grep -q 'large_ref: sent=[1-9]' "$PFD/$name.runs"/tickle@pre_Array4m_*_pub.log 2>/dev/null ||
                { say "PREFLIGHT FAILED: $name tickle@pre sent descriptors (it is not step A)"; exit 1; }
        done
        say "--- PC preflight clean ($PFD) ---"
    fi
    if [ "${PREFLIGHT_ONLY:-0}" = 1 ]; then say "=== PREFLIGHT_ONLY: stopping before the rig ==="; exit 0; fi
    say "--- taking the rig lock (hil), once for both stages ---"
    export B_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=36000
    exec "$RUNNER/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi

cd "$RUNNER" || exit 1
say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
FILES=()
for spec in "b2a|reliable|--reliable --keep_last --history_depth 10" "b2b|best_effort|--keep_last --history_depth 5"; do
    IFS='|' read -r name qos qa <<<"$spec"
    OUT=$SAFE/${TAG}_${name}_samehost_$(date +%Y%m%d-%H%M%S)
    SAMEHOST=1 PRE_SHA=$MAIN_SHA HEAD_SHA=$STEPB_SHA ARMS="tickle@pre tickle@head fastdds cyclonedds" REPS=5 DUR=60 \
        CELLS="$CELLS_B" QOS_ARGS="$qa" PUB_RATE=30 SUMMARY=$X/largemsg_rig_summary.py OUT=$OUT \
        "$X/rmw_keepall_rig.sh" > "$OUT.launch.log" 2>&1
    say "  $name [$qa] same-host rc=$? -> $OUT.txt"
    FILES+=("$qos=$OUT.txt")
done
python3 "$X/largemsg_stepb_compare.py" "${FILES[@]}" > "$SAFE/${TAG}_compare.txt" 2>&1
say "  B2: rc=$? $(grep -E '^SESSION' "$SAFE/${TAG}_compare.txt") -> $SAFE/${TAG}_compare.txt"
say "=== largemsg B queue done ==="

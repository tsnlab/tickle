#!/usr/bin/env bash
# samehost_bundle_rig.sh - the same-host re-measure bundled after the QoS work (ROADMAP "Now" item 0): every RESULTS
# section 1 row whose frameworks were measured back to back, again at one commit of origin/main, with each cell's
# frameworks interleaved and their order rotated per rep (s6_transport_cells.sh since b83e2513). Written 2026-10-19,
# before its run. It adds no metric and changes no reading rule: each row is re-run through the harness and cells
# that produced it, and read by the reader that published it.
#
#   row(s)                    code  harness (as published)                    cell(s) here                  reps
#   S1, S2                    SF    fair_samehost_remeasure.sh, free arm      best_effort_throughput p2, p3    3
#   S3, S11, S13              SF/SP/SE  the same (S11: s11_publisher_cpu.py)  best_effort_throughput p4        5
#   S4, S5, S6 (+S17, S19)    SF    the same                                  reliable_throughput p2, p3, p4   3
#   S7c, S9c, S10c, S18       SC/SC0  the same, from creation time            reliable_latency p2, p3, p4      3
#                                                                             (200 pings/s, 10 s)
#   S8c, S12                  SC    the same                                  reliable_latency p2 0.05 60 s    3
#                                                                             (20 pings/s)
# S17 and S19 are read from reliable_throughput p3; S18 from reliable_latency p3; S13 (publisher peak RSS) from
# best_effort_throughput p4's cost block. Every cell: all three frameworks, each on its own shared memory, the free
# (unpinned) arm only - nothing published is pinned any more - BEST_EFFORT at -K 1 (KEEP_LAST 1 on every framework,
# the user's 2026-10-06 rule), one Pi (10.1.1.214), so every latency here is a round trip on one clock.
# SAMPLE COUNTS kept from the published runs: 3 reps per cell (SF, SC, SC0), and 5 for best_effort_throughput p4,
# which is what SP measured S11 on. S3 and S13 are read from that same 5-rep cell, so their range rule runs over 5
# reps rather than 3 - stricter, never looser; the readers' minimum of 2 counted reps is unchanged.
#
# READERS, unchanged, after the last cell:
#   fair_samehost_summary.py <OUTB>  S1-S6 goodput, S17 CPU, S13/S19 RSS (cost block), S7c-S10c, S12, S18 creation
#                                    RTT with its C1 sample counts and the R4 control (C2) per latency cell
#   s11_publisher_cpu.py <OUTB> <CONTROL_OUTB>  S11, with its vendor control against the e521bbba campaign as SP had
#                                    (CONTROL_OUTB, default ~/rig_results_safe/fair_samehost_e521bbba_20261009-033254)
# Nothing is written into docs/RESULTS.md by this script.
#
# WHAT IT REFUSES, before the rig is touched:
#   - a SHA not on origin/main (fetched here: the Pis build it), or one without the QoS work (47eb05e8);
#   - a harness (HARNESS_SHA, run from the detached worktree RUNNER) whose s6_transport_cells.sh does not interleave
#     (b83e2513 not an ancestor) or that lacks any reader or this driver;
#   - a missing control run for S11; a reader self-test that fails;
#   - either Pi with a netem or tbf qdisc on eth0 or lo (someone's shaping is still on) - checked again under the lock;
#   - a PC preflight (rig_preflight.sh, every cell below, every framework this PC can build, both processes in one
#     private namespace with one private /dev/shm) that fails, or whose latency clients do not print crt_*.
# PC preflight builds use the layout flags for THIS PC's architecture (gate_v2_lib.sh with GATE_BUILD_ARCH=$(uname
# -m)): the rig's aarch64-only -mno-fix-cortex-a53-843419 is rejected by x86 gcc. On the rig TICKLE_EXTRA_CFLAGS is
# left unset, so s6 picks the layout-stable aarch64 build every published same-host row used.
#
# THE RIG. The lock (rig_lock.sh, hil) is taken ONCE, inside this detached process, for every cell; the inner
# fair_samehost_remeasure.sh / s6_transport_cells.sh calls see RIG_LOCK_HELD_HIL=1 and take nothing. This script
# applies nothing to the Pis: no tc, no setting. s6 stops its own processes; the one EXIT trap here reports (by
# /proc/PID/exe) any bench process left on the Pis and the last line, always:
#   SAMEHOST_BUNDLE_DONE rc=<n> ...      rc 0: every cell ran and recorded its reps in the rotated order
# Per cell it prints the realized framework order of every rep and each framework's recorded reps and samples
# (samehost_bundle_cells.py), before any value is read.
#
# ESTIMATE (from the published runs' own driver logs, three frameworks, s6 builds per cell): a throughput cell ~5.5
# min at 3 reps (e521bbba), best_effort_throughput p4 at 5 reps ~9 min (228da18d), a 200/s latency cell ~8.8 min, the
# 20/s 60 s cell ~24 min (2579e822): 11 + 9 + 16.5 + 26.5 + 24 = ~87 min under the lock, plus the PC preflight ~10 min
# before it. ~1 h 30 min in all (cells ~87 min, preflight ~1 min); analyse an overrun at 2 h 15 min.
#
# Usage (detached, from a checkout that has this script):
#   setsid nohup examples/perf_hil/experiments/samehost_bundle_rig.sh \
#     > ~/rig_results_safe/samehost_bundle_$(date +%Y%m%d-%H%M%S).log 2>&1 < /dev/null &
# env: SHA (default origin/main), HARNESS_SHA (default this checkout's HEAD), RUNNER (/home/semih/tickle_runner_samehost),
#      CONTROL_OUTB, PREFLIGHT=0 (skip the PC preflight), PREFLIGHT_ONLY=1 (checks and PC preflight, then stop: no lock),
#      ONLY_CELLS="<n> ..." (1-based indices into the cell list; for a dry run - a real bundle runs them all).
set -u
SELF_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
MAIN=/home/semih/tickle
RUNNER=${RUNNER:-/home/semih/tickle_runner_samehost}
HARNESS_SHA=${HARNESS_SHA:-$(git -C "$SELF_REPO" rev-parse HEAD)}
CONTROL_OUTB=${CONTROL_OUTB:-$HOME/rig_results_safe/fair_samehost_e521bbba_20261009-033254}
QOS_DONE=47eb05e8      # the last commit of the QoS work (ROADMAP "QoS reshaped like rmw: done")
INTERLEAVE=b83e2513    # s6_transport_cells.sh: frameworks interleaved, order rotated per rep
SAFE=${BUNDLE_OUT_DIR:-$HOME/rig_results_safe} # BUNDLE_OUT_DIR: a dry run's results stay out of the rig's
# BUNDLE_FRAMEWORKS: for a dry run on this PC (rig_dryrun.sh), which has no FastDDS 2.14 and no iox-roudi. A real
# bundle runs all three: the phase that takes the lock refuses it (a dry run enters at the lock phase, SHBUNDLE_LOCKED=1).
FRAMEWORKS=${BUNDLE_FRAMEWORKS:-"tickle fastdds cyclonedds"}
PIS="10.1.1.214 10.1.1.213"
K=$HOME/.ssh/tickle_ci_ed25519
# cell (fair_samehost_remeasure.sh's CELLS syntax) | reps | rows it feeds
CELLS=(
    "best_effort_throughput:p2|3|S1"
    "best_effort_throughput:p3|3|S2"
    "best_effort_throughput:p4|5|S3 S11 S13"
    "reliable_throughput:p2|3|S4"
    "reliable_throughput:p3|3|S5 S17 S19"
    "reliable_throughput:p4|3|S6"
    "reliable_latency:p2|3|S7c"
    "reliable_latency:p3|3|S9c S18"
    "reliable_latency:p4|3|S10c"
    "reliable_latency:p2:0.05:60|3|S8c S12"
)
if [ -n "${ONLY_CELLS:-}" ]; then
    sel=()
    for i in $ONLY_CELLS; do sel+=("${CELLS[$((i - 1))]}"); done
    CELLS=("${sel[@]}")
fi
say() { echo "$(date -Is) $*"; }
pi() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "$2" </dev/null; }
X=$RUNNER/examples/perf_hil/experiments

# shaped_refusal: 0 when neither Pi's eth0 or lo carries netem or tbf; prints and returns 1 otherwise (and when a
# Pi cannot be asked: "could not look" is not "not shaped").
shaped_refusal() {
    local h d q bad=0
    for h in $PIS; do
        for d in eth0 lo; do
            if ! q=$(pi "$h" "tc qdisc show dev $d" 2>&1); then
                say "REFUSED: could not read the qdisc of $h $d ($q)"
                bad=1
                continue
            fi
            case "$q" in
            *netem* | *tbf*) say "REFUSED: $h $d is shaped: $(echo "$q" | tr '\n' ' ')"; bad=1 ;;
            *) say "  $h $d not shaped: $(echo "$q" | head -1)" ;;
            esac
        done
    done
    return $bad
}

if [ "${SHBUNDLE_LOCKED:-0}" != 1 ]; then
    trap 'echo "SAMEHOST_BUNDLE_DONE rc=$? (before the lock) $(date -Is)"' EXIT
    [ -z "${BUNDLE_FRAMEWORKS:-}" ] || { say "REFUSED: BUNDLE_FRAMEWORKS is for dry runs; the bundle runs all three"; exit 1; }
    git -C "$MAIN" fetch -q origin || { say "REFUSED: git fetch failed"; exit 1; }
    SHA=$(git -C "$MAIN" rev-parse --verify -q "${SHA:-origin/main}^{commit}") ||
        { say "REFUSED: SHA '${SHA:-origin/main}' is not a commit in $MAIN"; exit 1; }
    git -C "$MAIN" merge-base --is-ancestor "$SHA" origin/main ||
        { say "REFUSED: SHA $SHA is not on origin/main (the Pis fetch it)"; exit 1; }
    git -C "$MAIN" merge-base --is-ancestor "$QOS_DONE" "$SHA" ||
        { say "REFUSED: SHA $SHA predates the QoS work ($QOS_DONE); this is the re-measure after it"; exit 1; }
    git -C "$SELF_REPO" cat-file -e "$HARNESS_SHA^{commit}" 2>/dev/null ||
        { say "REFUSED: HARNESS_SHA $HARNESS_SHA is not in $SELF_REPO"; exit 1; }
    git -C "$SELF_REPO" merge-base --is-ancestor "$INTERLEAVE" "$HARNESS_SHA" ||
        { say "REFUSED: HARNESS_SHA $HARNESS_SHA lacks $INTERLEAVE (s6 would not rotate the framework order)"; exit 1; }
    if [ ! -d "$RUNNER" ]; then
        git -C "$SELF_REPO" worktree add -q --detach "$RUNNER" "$HARNESS_SHA" || { say "REFUSED: worktree $RUNNER"; exit 1; }
    fi
    git -C "$RUNNER" checkout -q --detach "$HARNESS_SHA" || { say "REFUSED: checkout of $HARNESS_SHA in $RUNNER"; exit 1; }
    for f in samehost_bundle_rig.sh samehost_bundle_cells.py fair_samehost_remeasure.sh s6_transport_cells.sh \
        fair_samehost_summary.py s11_publisher_cpu.py creation_latency_summary.py s6_reps.py rig_preflight.sh; do
        [ -f "$X/$f" ] || { say "REFUSED: $HARNESS_SHA has no $f"; exit 1; }
    done
    grep -q 'FSR_LOG_APPEND' "$X/fair_samehost_remeasure.sh" ||
        { say "REFUSED: fair_samehost_remeasure.sh at $HARNESS_SHA would truncate the driver log per cell"; exit 1; }
    [ -s "$CONTROL_OUTB.best_effort_throughput_p4_free.txt" ] ||
        { say "REFUSED: no S11 control run at $CONTROL_OUTB.best_effort_throughput_p4_free.txt"; exit 1; }
    python3 "$X/creation_latency_summary.py" --selftest | tail -1 | grep -qx 'selftest: PASS' ||
        { say "REFUSED: creation_latency_summary.py's selftest does not pass"; exit 1; }
    python3 "$X/samehost_bundle_cells.py" --selftest | tail -1 | grep -qx 'selftest: PASS' ||
        { say "REFUSED: samehost_bundle_cells.py's selftest does not pass"; exit 1; }
    say "=== same-host bundle: core ${SHA:0:8}, harness ${HARNESS_SHA:0:8} in $RUNNER, ${#CELLS[@]} cells ==="
    for c in "${CELLS[@]}"; do say "    cell ${c%%|*}  reps $(echo "$c" | cut -d'|' -f2)  rows $(echo "$c" | cut -d'|' -f3)"; done
    say "uptime: $(uptime)"
    say "--- shaping check (refused on netem/tbf on either Pi's eth0 or lo) ---"
    shaped_refusal || exit 1
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        specs=()
        for c in "${CELLS[@]}"; do
            cell=${c%%|*}
            scen=${cell%%:*}
            cli=""
            [ "$scen" = reliable_latency ] && cli="-i $(echo "$cell:0.005" | cut -d: -f3)"
            [ "$scen" = best_effort_throughput ] && cli="-K 1"
            specs+=("$(echo "$cell" | cut -d: -f1-2):N0:$cli:-Q")
        done
        # This PC's layout flags: gate_v2_lib.sh for this architecture, in a subshell so nothing else is sourced here.
        # shellcheck disable=SC1091,SC2016 # the library is the runner's; $TICKLE_LAYOUT_CFLAGS is the subshell's
        pcflags=$(GATE_BUILD_ARCH=$(uname -m) bash -c '. "$1" && printf "%s" "$TICKLE_LAYOUT_CFLAGS"' _ "$X/gate_v2_lib.sh")
        case "$pcflags" in
        *cortex-a53* | '') say "REFUSED: PC preflight flags '$pcflags' (empty, or the rig's aarch64-only option)"; exit 1 ;;
        esac
        pfd=$SAFE/preflight/samehost_bundle_${SHA:0:8}_$(date +%Y%m%d-%H%M%S)
        say "--- PC preflight, ${#specs[@]} cells x {$FRAMEWORKS} at ${SHA:0:8}, TICKLE_EXTRA_CFLAGS='$pcflags' -> $pfd ---"
        PREFLIGHT_OUT=$pfd SHA=$SHA PREFLIGHT_TOPO=samens FWS="$FRAMEWORKS" TICKLE_EXTRA_CFLAGS="$pcflags" \
            TMPDIR=/home/semih/tmp_build nice -n 10 "$X/rig_preflight.sh" "${specs[@]}" >"$pfd.log" 2>&1 ||
            { say "PREFLIGHT FAILED: $pfd.log"; tail -15 "$pfd.log"; exit 1; }
        grep -E '^(ok|SKIP|FAIL) ' "$pfd.log" | sed 's/^/    /'
        # The flags reached the tickle core (an arm must report its treatment): every core compile line echoed in the
        # tickle build logs carries them (the core is built once per prefix and reused, so most variants' logs show
        # none; the bench's own compile is not echoed), and the latency clients carry crt_*.
        n_tb=$(cat "$pfd"/build_tickle_*.log 2>/dev/null | grep -cE '^cc .* -c -o obj/')
        n_tf=$(cat "$pfd"/build_tickle_*.log 2>/dev/null | grep -E '^cc .* -c -o obj/' | grep -cF -e " $pcflags ")
        if [ "$n_tb" = 0 ] || [ "$n_tf" != "$n_tb" ]; then
            say "STOPPED: $n_tf of $n_tb tickle core compile lines in the PC preflight carry '$pcflags' ($pfd)"
            exit 1
        fi
        n_res=$(cat "$pfd"/jobs/*reliable_latency*/client.log 2>/dev/null | grep -c '^RESULT:.*scenario=reliable_latency')
        n_crt=$(cat "$pfd"/jobs/*reliable_latency*/client.log 2>/dev/null | grep -c '^RESULT:.*crt_avg_ms=')
        if [ "$n_res" = 0 ] || [ "$n_crt" != "$n_res" ]; then
            say "STOPPED: $n_crt of $n_res PC preflight latency client lines carry crt_* ($pfd)"
            exit 1
        fi
        say "--- PC preflight PASS: $(grep -c '^ok ' "$pfd.log") jobs ok, $n_tf/$n_tb tickle core compiles with the" \
            "layout flags, $n_crt/$n_res latency client lines with crt_* ($(grep -c '^SKIP ' "$pfd.log") SKIP lines) ---"
    fi
    if [ "${PREFLIGHT_ONLY:-0}" = 1 ]; then
        say "PREFLIGHT_ONLY=1: stopping before the lock"
        exit 0
    fi
    say "--- taking the rig lock (hil), once for every cell ---"
    trap - EXIT
    export SHA HARNESS_SHA RUNNER CONTROL_OUTB SHBUNDLE_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=36000 \
        TMPDIR=/home/semih/tmp_build ONLY_CELLS="${ONLY_CELLS:-}"
    unset TICKLE_EXTRA_CFLAGS # s6 sets the layout-stable aarch64 build itself (gate_v2_lib.sh layout_cflags_default)
    exec "$RUNNER/examples/perf_hil/rig_lock.sh" "$X/samehost_bundle_rig.sh" "$@"
fi

# ---- under the lock ----------------------------------------------------------------------------------------------
cd "$RUNNER" || exit 1
RC=0
finish() { # the one EXIT trap: it applied nothing to the Pis, so it reverts nothing; it reports what is left there
    local rc=$? h
    [ "$RC" = 0 ] && RC=$rc
    for h in $PIS; do
        # shellcheck disable=SC2016 # the $ are for the Pi's shell
        say "leftovers on $h: $(pi "$h" 'for q in /proc/[0-9]*; do e=$(readlink $q/exe 2>/dev/null); case "$e" in
            */iox-roudi|/home/ci/*/client|/home/ci/*/server) echo "${q#/proc/}=$e";; esac; done' 2>&1 | tr '\n' ' ')"
    done
    echo "SAMEHOST_BUNDLE_DONE rc=$RC sha=${SHA:0:8} outb=${OUTB:-none} $(date -Is)"
}
trap finish EXIT
say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
say "--- shaping check under the lock ---"
if ! shaped_refusal; then RC=1; exit 1; fi
OUTB=$SAFE/samehost_bundle_${SHA:0:8}_$(date +%Y%m%d-%H%M%S)
say "=== cells -> $OUTB.* ==="
for c in "${CELLS[@]}"; do
    IFS='|' read -r cell reps rows <<<"$c"
    say "--- cell $cell, $reps reps, rows $rows ($(date +%T)) ---"
    SHA=$SHA FRAMEWORKS="$FRAMEWORKS" CELLS="$cell" ARMS=free REPS=$reps BE_HISTORY=1 PREFLIGHT=0 OUTB=$OUTB FSR_LOG_APPEND=1 \
        "$X/fair_samehost_remeasure.sh" >>"$OUTB.launch.log" 2>&1
    crc=$?
    IFS=: read -r scen size lat_i _ <<<"$cell"
    label=$size
    [ -n "$lat_i" ] && label=${size}_i$lat_i
    f=$OUTB.${scen}_${label}_free.txt
    say "  fair_samehost_remeasure.sh exit $crc; s6 exit: $(grep -A1 -F -- "--- $scen $size free -> $f" "$OUTB.driver.log" |
        grep -oE 'exit [0-9]+' | tail -1)"
    if [ ! -s "$f" ]; then
        say "  NO OUTPUT at $f - this cell's rows cannot be read"
        RC=1
        continue
    fi
    python3 "$X/samehost_bundle_cells.py" "$f" "$reps" "$FRAMEWORKS" 2>&1 | sed 's/^/  /'
    [ "${PIPESTATUS[0]}" = 0 ] || RC=1
done
say "--- readers ---"
python3 "$X/fair_samehost_summary.py" "$OUTB" >"$OUTB.summary.txt" 2>&1
say "  fair_samehost_summary.py exit $? -> $OUTB.summary.txt: $(grep -cE '^  CONTROL PASS' "$OUTB.summary.txt") latency" \
    "cells CONTROL PASS, $(grep -cE '^  CONTROL FAIL' "$OUTB.summary.txt") FAIL"
python3 "$X/s11_publisher_cpu.py" "$OUTB" "$CONTROL_OUTB" >"$OUTB.s11.txt" 2>&1
say "  s11_publisher_cpu.py exit $? -> $OUTB.s11.txt: $(grep -E '^S11 (verdict|PUBLISHABLE|NOT)' "$OUTB.s11.txt" | tr '\n' ' ')"
say "=== same-host bundle done ==="

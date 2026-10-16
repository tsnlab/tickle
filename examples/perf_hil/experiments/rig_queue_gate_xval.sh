#!/usr/bin/env bash
# rig_queue_gate_xval.sh - gate v2's re-validation on the rig after A1'' (db24e860 added -mno-fix-cortex-a53-843419 to
# the layout flags; TESTING.md 6.1, "Validating the gate itself": "Expected on the rig after A1'' (not yet run)").
# Written 2026-10-17 from rig_queue_core_ab.sh (its checks, preflight, harness runner, wait, lock and EXIT trap), before
# any run of it.
#
#   A        the build validated against itself: a pushed main commit (reachable from origin/main), required.
#   HARNESS  the commit whose examples/perf_hil runs the queue (default: HEAD of this script's checkout), checked out
#            detached in RUNNER (default ~/tickle_runner_gate_xval_<harness12>) and never moved there. This script must
#            be byte-identical to the harness commit's copy, which is the one that runs under the lock.
#   TAG      the chains' file prefix (default gxval_<a8>_<stamp>; the chains write /tmp/<TAG>_X1_* and /tmp/<TAG>_X2_*).
#   WAIT_LOG / WAIT_LINE / WAIT_MAX_S   start only after WAIT_LINE appears in WAIT_LOG (as rig_queue_core_ab.sh).
#   RIG_LOCK_WAIT  seconds rig_lock.sh waits for the hil lock (default 36000).
#   DRY_RUN=1      every check, and the exact commands it would run, printed; builds, preflights, waits, locks nothing.
#   PREFLIGHT_ONLY=1  the PC preflight, then stop before the wait.
#   XVAL_STANDIN_CELLS / XVAL_STANDIN_REPS   fewer cells / reps, for rig_dryrun.sh only: honoured only when the launch
#            already holds a stand-in lock (RIG_LOCK_HELD_HIL=1, which rig_dryrun.sh sets and a real launch never has);
#            otherwise refused. Every reading of such a run is labelled STAND-IN.
#
# THE STAGE: two campaign_ab_chain.sh GATE=v2 chains, one after the other under ONE hil lock, each mirrored blocks
# A P B B P A (P = A's commit at TICKLE_PLACEMENT_CFLAGS, -falign-functions=128 + no padding), REPS=3 per block, cells
# CORE_CELLS="1 2 3 4 8 13 14 15 10 11 16 17" (reliable and best-effort throughput p1-p4, reliable latency p1-p4, N0) -
# 88 deciding comparisons each (48 instruction rows + 40 witness rows), as rig_queue_core_ab.sh. B = A's own commit:
#   X1  B with A's flags (TICKLE_LAYOUT_CFLAGS): A/A. A vs P is the chain's own placement arm (PLACEMENT / NOTE lines).
#   X2  B at -falign-functions=256 with the same no-padding flags: TICKLE_PLACEMENT_CFLAGS with 128 -> 256, so it carries
#       TICKLE_NOPAD_CFLAGS and, building on the Pis (aarch64), -mno-fix-cortex-a53-843419. "Same commit, different
#       flags" is campaign_ab_chain.sh's own B_EXTRA_CFLAGS (B = A's commit under another alignment); no new option.
#
# PRE-REGISTRATION (the code is gate_xval_read.py; its docstring is the rule; nothing is judged by hand):
#   X PASS   no PRIMARY (instruction) row WORSE, and every latency cell's insn_u_per_sample row (client and server)
#            within its threshold (|t| <= the Bonferroni Student t, either direction, floor or no floor).
#   X FAIL   either broken; the rows are listed. A latency-cell instruction move = a layout path A1'' did not cover.
#   X VOID   the chain's VOID/REFUSED, gate v2 NO VERDICT, k not matching the rows, a cell or a latency insn_u row
#            missing, a non-numeric t, or the chain log not showing B = A and the flags given (the treatment).
#   reported WITNESS WORSE rows (a throughput CPU/rate move must sit inside P's floor - expected none); PRIMARY rows
#            beyond the threshold inside P's floor; every A-vs-P NOTE, marked in latency cells (expected none there).
#   QUEUE    VALIDATED (rc 0) only if X1 and X2 both PASS; NOT VALIDATED (rc 2) if either FAILs; NOT ANSWERED (rc 3)
#            otherwise.
#
# BEFORE THE WAIT (nothing of the rig is taken): the checks; both readers' --selftest at the harness commit; the PC
# preflight build of A (pc_preflight_build.sh, PF_BUILDS/<sha12>, built when missing, without ulimit -v); the twelve
# cell shapes through rig_preflight.sh for A (private namespaces on this PC). Any failure refuses.
#
# RIG TIME: one core chain took 105 min (rx-gap3c, 2026-10-15 01:58-03:44, Pi builds included); two chains ~210 min.
# The 1.5x deadline (315 min after the lock) is printed when the lock is taken; schedule a wakeup at it.
#
# Usage (from a checkout at the harness commit; launch detached, results outside the session):
#   A=<main sha> [WAIT_LOG=<log> WAIT_LINE='^... done'] \
#     setsid nohup examples/perf_hil/experiments/rig_queue_gate_xval.sh > ~/rig_results_safe/queue_gate_xval.log 2>&1 < /dev/null &
#   DRY_RUN=1 A=<sha> examples/perf_hil/experiments/rig_queue_gate_xval.sh      (seconds; no rig, no lock)
# Results: ~/rig_results_safe/gate_xval/<TAG>/ (reading_X1.txt, reading_X2.txt, X1.chain.log, X2.chain.log, the chains'
# files, preflight logs). Exit status: 0 VALIDATED (or a clean DRY_RUN / PREFLIGHT_ONLY), 1 REFUSED or preflight failed,
# 2 NOT VALIDATED, 3 NOT ANSWERED, 4 the wait timed out, 5 the rig lock was not obtained.
# The last line is always "gate xval queue done rc=<n> <date>".
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
SELF_REL=examples/perf_hil/experiments/rig_queue_gate_xval.sh
XREL=examples/perf_hil/experiments
CORE_CELLS="1 2 3 4 8 13 14 15 10 11 16 17"
CORE_REPS=3
MEM_KB=2097152
SAFE=$HOME/rig_results_safe
PF_BUILDS=${PF_BUILDS:-/home/semih/pc_preflight_builds}
DRY_RUN=${DRY_RUN:-0}
PREFLIGHT_ONLY=${PREFLIGHT_ONLY:-0}
WAIT_LOG=${WAIT_LOG:-}
WAIT_LINE=${WAIT_LINE:-}
WAIT_MAX_S=${WAIT_MAX_S:-86400}
RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
EST_MIN=210
export TMPDIR=/home/semih/tmp_build
rc=1
say() { echo "$(date -Is) $*"; }
lim() { (ulimit -v "$MEM_KB" && "$@"); } # one PC command under the 2 GiB cap (the PC hosts another project's VMs)
would() { printf 'WOULD RUN:'; printf ' %q' "$@"; echo; }
refuse() { say "REFUSED: $*"; rc=1; exit 1; }

# xval_flags <gate_v2_lib.sh>: sets AFL (arm A's and X1's B flags), PFL (P's) and X2FL (X2's B flags) from that copy of
# the library, in this environment (GATE_BUILD_ARCH decides the aarch64-only flag). Fails when the 128 -> 256
# substitution matched nothing or the result lost the no-padding flags.
xval_flags() {
    local TICKLE_EXTRA_CFLAGS
    unset TICKLE_EXTRA_CFLAGS
    # shellcheck disable=SC1090 # the harness commit's copy, chosen at run time
    . "$1" || return 1
    layout_cflags_default
    AFL=$TICKLE_EXTRA_CFLAGS
    PFL=$TICKLE_PLACEMENT_CFLAGS
    X2FL=${PFL/-falign-functions=128/-falign-functions=256}
    [ "$X2FL" != "$PFL" ] || { echo "TICKLE_PLACEMENT_CFLAGS '$PFL' has no -falign-functions=128 to move to 256"; return 1; }
    case " $X2FL " in *" $TICKLE_NOPAD_CFLAGS "*) ;; *) echo "X2 flags '$X2FL' lost '$TICKLE_NOPAD_CFLAGS'"; return 1 ;; esac
    case " $X2FL " in *" -falign-functions=256 "*) ;; *) echo "X2 flags '$X2FL' have no -falign-functions=256"; return 1 ;; esac
    [ "$X2FL" != "$AFL" ] || { echo "X2 flags equal A's ('$AFL')"; return 1; }
    return 0
}

# ---- under the lock ---------------------------------------------------------------------------------------------------
if [ "${GATE_XVAL_LOCKED:-0}" = 1 ]; then
    [ "${RIG_LOCK_HELD_HIL:-0}" = 1 ] || { say "REFUSED: GATE_XVAL_LOCKED=1 without the hil lock (RIG_LOCK_HELD_HIL)"; exit 1; }
    for v in A TAG OUTD RUNNER CELLS REPS; do [ -n "${!v:-}" ] || { say "REFUSED: $v is not set under the lock"; exit 1; }; done
    ulimit -v "$MEM_KB" # every PC process from here on (ssh, scp, the reader); the builds are on the Pis
    cd "$RUNNER" || exit 1
    X=$RUNNER/$XREL
    unset TICKLE_EXTRA_CFLAGS P_EXTRA_CFLAGS B_EXTRA_CFLAGS PRIMARY WITNESS
    why=$(xval_flags "$X/gate_v2_lib.sh") || { say "REFUSED: $why"; exit 1; }
    xval_flags "$X/gate_v2_lib.sh" || exit 1
    say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null); estimate ~$EST_MIN min, 1.5x deadline:" \
        "$(date -d "+$((EST_MIN * 3 / 2)) min" -Is) ---"
    [ -n "${STANDIN:-}" ] && say "STAND-IN: $STANDIN"
    declare -A R
    for x in X1 X2; do
        t=${TAG}_$x
        if [ "$x" = X1 ]; then bfl=$AFL; else bfl=$X2FL; fi
        say "--- $x: A = B = ${A:0:12}, gate v2 A P B B P A, REPS=$REPS, cells [$CELLS], flags A='$AFL' P='$PFL' B='$bfl' ---"
        if [ "$x" = X1 ]; then
            env TAG="$t" A="$A" B="$A" GATE=v2 REPS="$REPS" CELLS="$CELLS" PREFLIGHT=0 "$X/campaign_ab_chain.sh" \
                > "$OUTD/$x.chain.log" 2>&1
        else
            env TAG="$t" A="$A" B="$A" B_EXTRA_CFLAGS="$bfl" GATE=v2 REPS="$REPS" CELLS="$CELLS" PREFLIGHT=0 \
                "$X/campaign_ab_chain.sh" > "$OUTD/$x.chain.log" 2>&1
        fi
        cr=$?
        cp /tmp/"${t}"_* /tmp/"${t}".done "$OUTD/" 2>/dev/null
        say "  $x chain rc=$cr: $(cat "$OUTD/${t}.done" 2>/dev/null || echo '(no done file)')"
        timeout 300 python3 "$X/gate_xval_read.py" --name "$x" --compare "$OUTD/${t}_compare.txt" --done "$OUTD/${t}.done" \
            --chain-log "$OUTD/$x.chain.log" --cells "$CELLS" --a "$A" --a-flags "$AFL" --b-flags "$bfl" \
            --stand-in "${STANDIN:-}" > "$OUTD/reading_$x.txt" 2>&1
        rr=$?
        grep -aE '^(STAND-IN|chain|gate v2|treatment|  FAIL|  reported|  [0-9]+ deciding|GATE XVAL)' "$OUTD/reading_$x.txt" |
            cut -c1-230 | sed 's/^/  /'
        case $rr in
        0) R[$x]=PASS ;;
        1) R[$x]=FAIL ;;
        3) R[$x]=VOID ;;
        *) R[$x]="VOID(reader rc $rr)"; say "the $x reader did not finish ($OUTD/reading_$x.txt)"; tail -5 "$OUTD/reading_$x.txt" ;;
        esac
    done
    if [ "${R[X1]}" = PASS ] && [ "${R[X2]}" = PASS ]; then
        say "READING: VALIDATED - X1 (A/A) PASS, X2 (A vs A at 256) PASS${STANDIN:+ [STAND-IN, not the rig]}"
        exit 0
    elif [ "${R[X1]}" = FAIL ] || [ "${R[X2]}" = FAIL ]; then
        say "READING: NOT VALIDATED - X1 ${R[X1]}, X2 ${R[X2]} (the FAIL rows above)${STANDIN:+ [STAND-IN, not the rig]}"
        exit 2
    fi
    say "READING: NOT ANSWERED - X1 ${R[X1]}, X2 ${R[X2]}${STANDIN:+ [STAND-IN, not the rig]}"
    exit 3
fi

# ---- before the lock: checks, PC preflight, the wait ------------------------------------------------------------------
TMPX=""
trap '[ -n "$TMPX" ] && rm -rf -- "$TMPX"; echo "gate xval queue done rc=$rc $(date -Is)"' EXIT
G() { git -C "$REPO" "$@"; }
[ "$DRY_RUN" = 1 ] && say "=== DRY_RUN: checks only; every command that would build, preflight, wait, lock or touch the rig is printed ==="
CELLS=$CORE_CELLS REPS=$CORE_REPS STANDIN=""
if [ -n "${XVAL_STANDIN_CELLS:-}${XVAL_STANDIN_REPS:-}" ]; then
    [ "${RIG_LOCK_HELD_HIL:-0}" = 1 ] ||
        refuse "XVAL_STANDIN_CELLS/REPS are for rig_dryrun.sh only (no stand-in lock held: this would be a real rig run)"
    CELLS=${XVAL_STANDIN_CELLS:-$CORE_CELLS} REPS=${XVAL_STANDIN_REPS:-$CORE_REPS}
    case "$CELLS $REPS" in *[!0-9\ ]*) refuse "XVAL_STANDIN_CELLS/REPS take numbers" ;; esac
    STANDIN="rig_dryrun stand-in stage: cells [$CELLS] REPS=$REPS (pre-registered: [$CORE_CELLS] REPS=$CORE_REPS)"
    say "STAND-IN: $STANDIN"
fi
for v in TICKLE_EXTRA_CFLAGS P_EXTRA_CFLAGS B_EXTRA_CFLAGS PRIMARY WITNESS; do
    [ -z "${!v+set}" ] || refuse "$v is set in the environment; this stage defines every arm's flags and the rule itself"
done
[ -n "${A:-}" ] || refuse "set A to the pushed main commit to validate the gate on"
G fetch -q origin || refuse "git fetch origin failed"
A=$(G rev-parse --verify -q "$A^{commit}") || refuse "no commit A=$A"
HARNESS=$(G rev-parse --verify -q "${HARNESS:-HEAD}^{commit}") || refuse "no commit HARNESS=${HARNESS:-HEAD}"
G merge-base --is-ancestor "$A" refs/remotes/origin/main || refuse "A ${A:0:8} is not on origin/main (push it; the Pis fetch it)"
say "A ${A:0:12} is on origin/main: $(G log -1 --format='%h %s' "$A" | cut -c1-120)"
TAG=${TAG:-gxval_${A:0:8}_$(date +%Y%m%d-%H%M%S)}
case "$TAG" in *[!A-Za-z0-9_-]* | "") refuse "TAG '$TAG' may hold only [A-Za-z0-9_-]" ;; esac
compgen -G "/tmp/${TAG}_*" > /dev/null || [ -e "/tmp/${TAG}_X1.done" ] || [ -e "/tmp/${TAG}_X2.done" ] &&
    refuse "/tmp/${TAG}_* exists: campaign_ab_chain.sh would append to an earlier chain's arm files"
OUTD=${OUTD:-$SAFE/gate_xval/$TAG}
compgen -G "$OUTD/reading_*.txt" > /dev/null && refuse "$OUTD already holds a reading"
RUNNER=${RUNNER:-/home/semih/tickle_runner_gate_xval_${HARNESS:0:12}}

# The harness commit carries every file the queue runs, and this script is its copy.
G show "$HARNESS:$SELF_REL" 2> /dev/null | cmp -s - "${BASH_SOURCE[0]}" ||
    refuse "this script differs from ${HARNESS:0:8}:$SELF_REL (commit it, or launch the harness commit's copy)"
for f in "$XREL/gate_xval_read.py:GATE XVAL" "$XREL/core_ab_read.py:CORE AB VERDICT" \
    "$XREL/campaign_ab_chain.sh:B_EXTRA_CFLAGS=\${B_EXTRA_CFLAGS-\$TICKLE_EXTRA_CFLAGS}" \
    "$XREL/campaign_ab_chain.sh:v2) ARMS=\"A P B B P A\"" "$XREL/ab_compare.py:--placement" "$XREL/gate_v2.py:def report" \
    "$XREL/gate_v2_lib.sh:TICKLE_A53FIX_OFF_CFLAGS=" "$XREL/campaign_sweep.sh:PREFLIGHT_CELLS_ONLY" \
    "$XREL/rig_preflight.sh:SHA" "examples/perf_hil/rig_lock.sh:RIG_LOCK_SCOPE"; do
    G show "$HARNESS:${f%%:*}" 2> /dev/null | grep -qF -- "${f#*:}" ||
        refuse "${HARNESS:0:8}:${f%%:*} has no '${f#*:}' (not an A1'' gate v2 harness)"
done
G cat-file -e "$A:$XREL/pc_preflight_build.sh" 2> /dev/null || refuse "${A:0:8} has no pc_preflight_build.sh (cannot build its PC preflight)"
G cat-file -e "$A:$XREL/gate_v2_lib.sh" 2> /dev/null || refuse "${A:0:8} predates gate v2"
if [ -d "$RUNNER" ]; then
    [ "$(git -C "$RUNNER" rev-parse HEAD 2> /dev/null)" = "$HARNESS" ] || refuse "$RUNNER exists and is not at ${HARNESS:0:8}"
    [ -z "$(git -C "$RUNNER" status --porcelain --untracked-files=no)" ] || refuse "$RUNNER has local edits"
    say "harness: $RUNNER at ${HARNESS:0:12} (exists, clean)"
elif [ "$DRY_RUN" = 1 ]; then
    would git -C "$REPO" worktree add -q --detach "$RUNNER" "$HARNESS"
else
    G worktree add -q --detach "$RUNNER" "$HARNESS" || refuse "worktree add $RUNNER"
    say "harness: $RUNNER at ${HARNESS:0:12} (created)"
fi

# The readers' selftests, the arms' flags and the cell list, from the harness commit's own files (a private export).
TMPX=$(mktemp -d "$TMPDIR/gate_xval_XXXXXX") || refuse "mktemp in $TMPDIR"
G archive "$HARNESS" "$XREL/gate_xval_read.py" "$XREL/core_ab_read.py" "$XREL/gate_v2.py" "$XREL/campaign_sweep.sh" \
    "$XREL/gate_v2_lib.sh" "$XREL/pi_checkout.sh" | tar -x -C "$TMPX" || refuse "git archive of the harness files"
for r in gate_xval_read.py core_ab_read.py; do
    st=$(lim timeout 300 python3 "$TMPX/$XREL/$r" --selftest 2>&1)
    str=$?
    [ "$str" = 0 ] && [ "$(tail -1 <<<"$st" | cut -c1-11)" = "SELFTEST OK" ] ||
        { tail -15 <<<"$st" | sed 's/^/    | /'; refuse "$r --selftest (rc $str)"; }
    say "$r --selftest: $(tail -1 <<<"$st")"
done
why=$(xval_flags "$TMPX/$XREL/gate_v2_lib.sh") || refuse "$why"
xval_flags "$TMPX/$XREL/gate_v2_lib.sh" || refuse "xval_flags"
say "flags (GATE_BUILD_ARCH=${GATE_BUILD_ARCH:-aarch64}): A and X1's B '$AFL'; P '$PFL'; X2's B '$X2FL'"
mapfile -t xspecs < <(cd "$TMPX" && PREFLIGHT_CELLS_ONLY=1 FWS=tickle CELLS="$CELLS" lim timeout 60 "$XREL/campaign_sweep.sh")
ncells=$(wc -w <<<"$CELLS")
[ "${#xspecs[@]}" = "$ncells" ] || refuse "campaign_sweep.sh listed ${#xspecs[@]} cell spec(s) for [$CELLS], not $ncells"
say "cells [$CELLS]: ${#xspecs[@]} cell specs"
printf '  spec: %s\n' "${xspecs[@]}"
[ "$DRY_RUN" = 1 ] || mkdir -p "$OUTD" "$TMPDIR" || refuse "mkdir $OUTD"
say "=== gate xval queue: A=${A:0:12} TAG=$TAG harness ${HARNESS:0:12} ($RUNNER) out $OUTD ==="

# ---- PC preflight build of A (no ulimit: performance_test needs more than 2 GiB) -------------------------------------
pf=$PF_BUILDS/${A:0:12}
if [ -f "$pf/BUILD_OK" ]; then
    say "PC build ${A:0:8}: $pf ($(tail -1 "$pf/BUILD_OK" 2> /dev/null))"
else
    wt=/home/semih/tickle_pfbuild_${A:0:8}
    if [ "$DRY_RUN" = 1 ]; then
        say "PC build ${A:0:8}: missing ($pf/BUILD_OK), would build (~5 min):"
        [ -d "$wt" ] || would git -C "$REPO" worktree add -q --detach "$wt" "$A"
        would git -C "$wt" checkout -q --detach "$A"
        echo "WOULD RUN: (cd $wt && PF_BUILDS=$PF_BUILDS timeout 3600 $XREL/pc_preflight_build.sh) > $OUTD/pfbuild_${A:0:8}.log 2>&1"
    else
        [ -d "$wt" ] || G worktree add -q --detach "$wt" "$A" || refuse "worktree $wt"
        git -C "$wt" checkout -q --detach "$A" || refuse "checkout ${A:0:8} in $wt"
        say "PC preflight build of ${A:0:8} (~5 min) -> $OUTD/pfbuild_${A:0:8}.log"
        (cd "$wt" && PF_BUILDS=$PF_BUILDS timeout 3600 "$XREL/pc_preflight_build.sh") > "$OUTD/pfbuild_${A:0:8}.log" 2>&1
        [ -f "$pf/BUILD_OK" ] || { tail -15 "$OUTD/pfbuild_${A:0:8}.log" | sed 's/^/    | /'; refuse "PREFLIGHT FAILED: no PC build of ${A:0:8}"; }
        say "PC build ${A:0:8}: $pf ($(tail -1 "$pf/BUILD_OK" 2> /dev/null))"
    fi
fi

# ---- the cell shapes through rig_preflight.sh for A (private namespaces on this PC) -----------------------------------
log=$OUTD/preflight_${A:0:8}.log
if [ "$DRY_RUN" = 1 ]; then
    printf 'WOULD RUN: (cd %q && ulimit -v %s && SHA=%q FWS=tickle timeout 3600 %q' "$RUNNER" "$MEM_KB" "$A" "$RUNNER/$XREL/rig_preflight.sh"
    printf ' %q' "${xspecs[@]}"
    printf ') > %q 2>&1\n' "$log"
else
    if ! (cd "$RUNNER" && lim env SHA="$A" FWS=tickle timeout 3600 "$RUNNER/$XREL/rig_preflight.sh" "${xspecs[@]}") > "$log" 2>&1; then
        tail -15 "$log" | sed 's/^/    | /'
        refuse "PREFLIGHT FAILED: cells for ${A:0:8} ($log)"
    fi
    say "preflight ${A:0:8}: $(tail -1 "$log" | cut -c1-120)"
    say "--- PC preflight clean ---"
fi
if [ "$PREFLIGHT_ONLY" = 1 ] && [ "$DRY_RUN" != 1 ]; then say "=== PREFLIGHT_ONLY: stopping before the wait ==="; rc=0; exit 0; fi

# ---- the queue ahead ---------------------------------------------------------------------------------------------------
if [ -n "$WAIT_LOG" ]; then
    [ -n "$WAIT_LINE" ] || refuse "WAIT_LOG is set without WAIT_LINE (the line the queue ahead always prints last)"
    if [ "$DRY_RUN" = 1 ]; then
        if grep -aq -- "$WAIT_LINE" "$WAIT_LOG" 2> /dev/null; then hit="present now"
        elif [ -e "$WAIT_LOG" ]; then hit="not yet there"
        else hit="the log does not exist yet"; fi
        echo "WOULD WAIT: until grep -aq -- '$WAIT_LINE' $WAIT_LOG, polling 60 s, at most ${WAIT_MAX_S}s (then rc 4) - $hit"
    else
        say "waiting for '$WAIT_LINE' in $WAIT_LOG (at most ${WAIT_MAX_S}s)"
        waited=0
        until grep -aq -- "$WAIT_LINE" "$WAIT_LOG" 2> /dev/null; do
            [ "$waited" -ge "$WAIT_MAX_S" ] && { say "REFUSED: $WAIT_LOG did not show '$WAIT_LINE' in ${WAIT_MAX_S}s"; rc=4; exit 4; }
            sleep 60
            waited=$((waited + 60))
        done
        say "the queue ahead ended: $(grep -a -m1 -- "$WAIT_LINE" "$WAIT_LOG" | cut -c1-160)"
    fi
fi

# ---- the rig, under one hil lock --------------------------------------------------------------------------------------
# rig_lock.sh execs the harness copy of this script under the lock (GATE_XVAL_LOCKED=1), as a child, so that a lock
# that never frees (rig_lock.sh exit 75) still ends with this queue's last line.
if [ "$DRY_RUN" = 1 ]; then
    echo "WOULD RUN: GATE_XVAL_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=$RIG_LOCK_WAIT A=$A TAG=$TAG OUTD=$OUTD RUNNER=$RUNNER" \
        "CELLS=\"$CELLS\" REPS=$REPS $RUNNER/examples/perf_hil/rig_lock.sh $RUNNER/$SELF_REL"
    for x in X1 X2; do
        if [ "$x" = X1 ]; then bfl=$AFL; bset=""; else bfl=$X2FL; bset="B_EXTRA_CFLAGS='$X2FL' "; fi
        echo "WOULD RUN (under the lock): cd $RUNNER && TAG=${TAG}_$x A=$A B=$A ${bset}GATE=v2 REPS=$REPS CELLS=\"$CELLS\"" \
            "PREFLIGHT=0 $RUNNER/$XREL/campaign_ab_chain.sh > $OUTD/$x.chain.log 2>&1"
        echo "WOULD RUN (under the lock): python3 $RUNNER/$XREL/gate_xval_read.py --name $x --compare $OUTD/${TAG}_${x}_compare.txt" \
            "--done $OUTD/${TAG}_$x.done --chain-log $OUTD/$x.chain.log --cells \"$CELLS\" --a $A --a-flags '$AFL' --b-flags '$bfl'" \
            "> $OUTD/reading_$x.txt"
    done
    say "=== DRY_RUN clean: every check passed; nothing was built, preflighted, waited for or locked ==="
    rc=0
    exit 0
fi
say "--- taking the rig lock (hil, RIG_LOCK_WAIT=${RIG_LOCK_WAIT}s) ---"
export GATE_XVAL_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT A TAG OUTD RUNNER CELLS REPS STANDIN
"$RUNNER/examples/perf_hil/rig_lock.sh" "$RUNNER/$SELF_REL"
lr=$?
case $lr in
0 | 2 | 3) rc=$lr ;;
75) say "the rig lock did not free within ${RIG_LOCK_WAIT}s"; rc=5 ;;
*) rc=1 ;;
esac
exit "$rc"

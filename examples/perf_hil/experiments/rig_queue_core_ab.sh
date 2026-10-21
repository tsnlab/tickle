#!/usr/bin/env bash
# rig_queue_core_ab.sh - the house-rule rig A/B of one change against its parent on the core cells (gate v2), as a
# reusable queue. Written 2026-10-15 from ~/rig_jobs/rig_queue_rx_gap3c_core.sh (stage 2 of the rx-gap3 queues), with
# everything specific to rx-gap3c removed; before any run of it.
#
#   A        the control (default origin/main). Must be on some origin branch: the Pis fetch it.
#   B        the change. Must be pushed: reachable from origin/$BRANCH. A must be an ancestor of B (B = A + the change).
#   BRANCH   the origin branch that carries B (e.g. ab/<name>, the house rule's A/B branch), checked.
#   HARNESS  the commit whose examples/perf_hil runs the queue (default: HEAD of this script's checkout). It is checked
#            out detached in RUNNER (default ~/tickle_runner_core_ab_<harness12>) and never moved there, so neither the
#            shared checkout's later edits nor another queue reach a running harness. This script must be byte-identical
#            to the harness commit's copy, which is the one that runs under the lock.
#   TAG      the chain's file prefix (default coreAB_<b8>_<stamp>; campaign_ab_chain.sh writes /tmp/<TAG>_*, and appends
#            to /tmp/<TAG>_<arm>.new, so the queue refuses a TAG with files already there).
#   WAIT_LOG / WAIT_LINE   start only after WAIT_LINE (a grep pattern) appears in WAIT_LOG - the last line the queue ahead
#            always prints; WAIT_MAX_S (default 86400) bounds the wait (then rc 4). WAIT_LOG empty (default): no wait.
#   RIG_LOCK_WAIT  seconds rig_lock.sh waits for the hil lock (default 36000).
#   DRY_RUN=1      every check below, and the exact commands it would run, printed; builds nothing, runs no preflight,
#                  creates no worktree or result directory, waits for nothing, takes no lock, touches no Pi.
#   PREFLIGHT_ONLY=1  the PC preflight, then stop before the wait (B need not be pushed yet: the branch check is skipped).
#
# THE STAGE (as the rx-gap3b/3c core queues ran it): campaign_ab_chain.sh GATE=v2 - mirrored blocks A P B B P A (P = A's
# commit rebuilt with the placement flags: the floor), REPS=3 per block (6 runs per arm), campaign_sweep.sh's own
# durations, cells CORE_CELLS="1 2 3 4 8 13 14 15 10 11 16 17" (reliable and best-effort throughput p1-p4, reliable
# latency p1-p4, N0) - 88 deciding comparisons (48 instruction rows + 40 witness rows: CPU, rate, latency).
#
# PRE-REGISTRATION (the code is core_ab_read.py; its docstring is the rule; nothing is judged by hand):
#   statistics   gate_v2.py's, unchanged: WORSE = beyond the Bonferroni Student t over the 88 AND beyond |P - A|.
#   PASS         no CPU witness row (cpu_s_per_Msample, any role) and no latency row (rtt_*) WORSE, B against A.
#   FAIL         any of those WORSE. Every other WORSE row is listed and counted "n/88 WORSE"; it does not block.
#   VOID         the chain's own VOID / REFUSED, gate v2's NO VERDICT, a k that does not match the rows read, a requested
#                cell with no CPU witness row, a latency cell with no rtt row, a missing or empty file.
#   reported     gate v2's own verdict line, cells with void rows, P-vs-A drift rows, user instructions above +0.1%.
#   service      with the service cells in CORE_CELLS (c18-c21, 2026-10-21): a service cell also needs an rtt row, and
#                the arm files are read for DEFECT rows (a callback run twice for one call): any in B is FAIL (B's own
#                criterion, even when A has them too); in A or P only, REPORTED. calls_per_s is a witness (rate:
#                listed, not blocking). On the PC, main itself ran a callback twice in c21 (N4) in 2 of 3 runs
#                (2026-10-21): until core is fixed, c21 fails any B, so a change that is not that fix A/Bs on
#                CORE_CELLS="18 19 20" and reads c21 as the defect's tracker.
#
# SERVICE CELLS RIG TIME (estimated 2026-10-21, not yet run): a service run is ~20 s (3 s server start, 2 s discovery,
# 2 + 5 + 2 s, the drain, ssh), 25 s at 10 Hz (-d 10); 4 cells x 3 reps x 6 blocks = 72 runs = ~26 min, plus the six
# blocks' Pi builds (~5 min each, as the core stage's) - CORE_CELLS="18 19 20 21" EST_MIN=60. Added to the 12-cell
# stage: EST_MIN=135.
#
# BEFORE THE WAIT (nothing of the rig is taken): the checks; core_ab_read.py --selftest at the harness commit; PC
# preflight builds of A and B (pc_preflight_build.sh at each commit, PF_BUILDS/<sha12>, built when missing, WITHOUT
# ulimit -v: performance_test cannot build under 2 GiB; every other PC process here runs under ulimit -v 2097152);
# the twelve cell shapes through rig_preflight.sh for A and for B (private namespaces on this PC). Any failure refuses.
#
# RIG TIME: the rx-gap3c core stage, 6 blocks x 12 cells x 3 reps, took 105 min (2026-10-15 01:58-03:44) including the
# Pi builds. The 1.5x deadline (158 min after the lock) is printed when the lock is taken; schedule a wakeup at it.
#
# Usage (from a checkout at the harness commit; launch detached, results outside the session):
#   A=<sha|origin/main> B=<sha> BRANCH=ab/<name> [WAIT_LOG=<log> WAIT_LINE='^... done'] \
#     setsid nohup examples/perf_hil/experiments/rig_queue_core_ab.sh > ~/rig_results_safe/queue_core_ab_<name>.log 2>&1 < /dev/null &
#   DRY_RUN=1 A=... B=... BRANCH=... examples/perf_hil/experiments/rig_queue_core_ab.sh      (seconds; no rig, no lock)
# Results: ~/rig_results_safe/core_ab/<TAG>/ (reading.txt, chain.log, the chain's files, preflight logs).
# Exit status: 0 PASS (or a clean DRY_RUN / PREFLIGHT_ONLY), 1 REFUSED or preflight failed, 2 FAIL, 3 VOID, 4 the wait
# timed out, 5 the rig lock was not obtained. The last line is always "core ab queue done rc=<n> <date>".
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
SELF_REL=examples/perf_hil/experiments/rig_queue_core_ab.sh
XREL=examples/perf_hil/experiments
# CORE_CELLS= (2026-10-21) chooses the campaign cells, e.g. the service cells for a service-path change:
#   CORE_CELLS="18 19 20 21"                              the service cells alone (c18-c21, TickLE core client/server)
#   CORE_CELLS="1 2 3 4 8 13 14 15 10 11 16 17 18 19 20 21"   the core stage plus the service cells
# Every number must name a campaign_sweep.sh cell; the spec count is checked against it below. EST_MIN= is the rig
# estimate for the 1.5x deadline line (default 105, the 12-cell stage; see RIG TIME).
CORE_CELLS="${CORE_CELLS:-1 2 3 4 8 13 14 15 10 11 16 17}"
N_SPECS=$(wc -w <<<"$CORE_CELLS")
MEM_KB=2097152
SAFE=$HOME/rig_results_safe
PF_BUILDS=${PF_BUILDS:-/home/semih/pc_preflight_builds}
DRY_RUN=${DRY_RUN:-0}
PREFLIGHT_ONLY=${PREFLIGHT_ONLY:-0}
WAIT_LOG=${WAIT_LOG:-}
WAIT_LINE=${WAIT_LINE:-}
WAIT_MAX_S=${WAIT_MAX_S:-86400}
RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
EST_MIN=${EST_MIN:-105}
case "$EST_MIN" in "" | *[!0-9]*) echo "EST_MIN must be whole minutes" >&2; exit 1 ;; esac
export TMPDIR=/home/semih/tmp_build
rc=1
say() { echo "$(date -Is) $*"; }
lim() { (ulimit -v "$MEM_KB" && "$@"); } # one PC command under the 2 GiB cap (the PC hosts another project's VMs)
would() { printf 'WOULD RUN:'; printf ' %q' "$@"; echo; }
refuse() { say "REFUSED: $*"; rc=1; exit 1; }

# ---- under the lock ---------------------------------------------------------------------------------------------------
if [ "${CORE_AB_LOCKED:-0}" = 1 ]; then
    [ "${RIG_LOCK_HELD_HIL:-0}" = 1 ] || { say "REFUSED: CORE_AB_LOCKED=1 without the hil lock (RIG_LOCK_HELD_HIL)"; exit 1; }
    for v in A B TAG OUTD RUNNER; do [ -n "${!v:-}" ] || { say "REFUSED: $v is not set under the lock"; exit 1; }; done
    ulimit -v "$MEM_KB" # every PC process from here on (ssh, scp, the reader); the builds are on the Pis
    cd "$RUNNER" || exit 1
    X=$RUNNER/$XREL
    say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null); estimate ~$EST_MIN min, 1.5x deadline:" \
        "$(date -d "+$((EST_MIN * 3 / 2)) min" -Is) ---"
    say "--- core cells A/B, gate v2 A P B B P A, REPS=3, cells [$CORE_CELLS], A=${A:0:8} B=${B:0:8} TAG=$TAG ---"
    TAG=$TAG A=$A B=$B GATE=v2 REPS=3 CELLS="$CORE_CELLS" PREFLIGHT=0 "$X/campaign_ab_chain.sh" > "$OUTD/chain.log" 2>&1
    cr=$?
    cp /tmp/"${TAG}"_* /tmp/"${TAG}".done "$OUTD/" 2>/dev/null
    say "  chain rc=$cr: $(cat "$OUTD/${TAG}.done" 2>/dev/null || echo '(no done file)')"
    # The arm files too: a service cell's DEFECT row (a callback run twice) is not an ok row, so only they show it.
    timeout 300 python3 "$X/core_ab_read.py" --compare "$OUTD/${TAG}_compare.txt" --done "$OUTD/${TAG}.done" \
        --cells "$CORE_CELLS" --a "${A:0:12}" --b "${B:0:12}" --arm-a "$OUTD/${TAG}_A.txt" --arm-b "$OUTD/${TAG}_B.txt" \
        --arm-p "$OUTD/${TAG}_P.txt" > "$OUTD/reading.txt" 2>&1
    rr=$?
    grep -aE '^(chain|gate v2|  BLOCKING|  OTHER|  DEFECT|  REPORTED|  [0-9]+/[0-9]+ WORSE|CORE AB VERDICT)' "$OUTD/reading.txt" | cut -c1-230 | sed 's/^/  /'
    case $rr in
    0) exit 0 ;;
    1) exit 2 ;;
    3) exit 3 ;;
    *) say "the reader did not finish (rc=$rr, $OUTD/reading.txt)"; tail -5 "$OUTD/reading.txt"; exit 3 ;;
    esac
fi

# ---- before the lock: checks, PC preflight, the wait ------------------------------------------------------------------
TMPX=""
trap '[ -n "$TMPX" ] && rm -rf -- "$TMPX"; echo "core ab queue done rc=$rc $(date -Is)"' EXIT
G() { git -C "$REPO" "$@"; }
[ "$DRY_RUN" = 1 ] && say "=== DRY_RUN: checks only; every command that would build, preflight, wait, lock or touch the rig is printed ==="
[ -n "${B:-}" ] || refuse "set B to the change's commit"
if [ "$PREFLIGHT_ONLY" != 1 ]; then
    [ -n "${BRANCH:-}" ] || refuse "set BRANCH to the origin branch that carries B (the Pis fetch it)"
    case "$BRANCH" in *[!A-Za-z0-9._/-]* | -* | */) refuse "BRANCH '$BRANCH' is not a plain branch name" ;; esac
fi
G fetch -q origin || refuse "git fetch origin failed"
A=$(G rev-parse --verify -q "${A:-origin/main}^{commit}") || refuse "no commit A=${A:-origin/main}"
B=$(G rev-parse --verify -q "$B^{commit}") || refuse "no commit B=$B"
HARNESS=$(G rev-parse --verify -q "${HARNESS:-HEAD}^{commit}") || refuse "no commit HARNESS=${HARNESS:-HEAD}"
[ "$A" != "$B" ] || refuse "A and B are the same commit ($A)"
G merge-base --is-ancestor "$A" "$B" || refuse "B ${B:0:8} is not on A ${A:0:8} (A must be an ancestor of B)"
say "A ${A:0:12} is an ancestor of B ${B:0:12}: $(G rev-list --count "$A..$B") commit(s)"
G log --oneline "$A..$B" | head -20 | sed 's/^/  B: /'
if [ "$PREFLIGHT_ONLY" != 1 ]; then
    G rev-parse --verify -q "refs/remotes/origin/$BRANCH" > /dev/null || refuse "no origin/$BRANCH (push B there first)"
    G merge-base --is-ancestor "$B" "refs/remotes/origin/$BRANCH" ||
        refuse "B ${B:0:8} is not reachable from origin/$BRANCH (push it first; the Pis fetch it)"
    G branch -r --contains "$A" | grep -q . || refuse "A ${A:0:8} is on no origin branch (the Pis could not fetch it)"
    say "B is on origin/$BRANCH ($(G rev-parse --short "refs/remotes/origin/$BRANCH")); A is on $(G branch -r --contains "$A" | grep -v -- " -> " | head -1 | tr -d ' ')"
fi
TAG=${TAG:-coreAB_${B:0:8}_$(date +%Y%m%d-%H%M%S)}
case "$TAG" in *[!A-Za-z0-9_-]* | "") refuse "TAG '$TAG' may hold only [A-Za-z0-9_-]" ;; esac
compgen -G "/tmp/${TAG}_*" > /dev/null || [ -e "/tmp/${TAG}.done" ] &&
    refuse "/tmp/${TAG}_* or /tmp/${TAG}.done exists: campaign_ab_chain.sh would append to an earlier chain's arm files"
OUTD=${OUTD:-$SAFE/core_ab/$TAG}
[ -e "$OUTD/reading.txt" ] && refuse "$OUTD already holds a reading"
RUNNER=${RUNNER:-/home/semih/tickle_runner_core_ab_${HARNESS:0:12}}

# The harness commit carries every file the queue runs, and this script is its copy.
G show "$HARNESS:$SELF_REL" 2> /dev/null | cmp -s - "${BASH_SOURCE[0]}" ||
    refuse "this script differs from ${HARNESS:0:8}:$SELF_REL (commit it, or launch the harness commit's copy)"
for f in "$XREL/core_ab_read.py:CORE AB VERDICT" "$XREL/campaign_ab_chain.sh:v2) ARMS=\"A P B B P A\"" \
    "$XREL/ab_compare.py:--placement" "$XREL/gate_v2.py:def report" "$XREL/campaign_sweep.sh:PREFLIGHT_CELLS_ONLY" \
    "$XREL/rig_preflight.sh:SHA" "examples/perf_hil/rig_lock.sh:RIG_LOCK_SCOPE" "$XREL/core_ab_read.py:--arm-b" \
    "$XREL/service_cell_verdict.py:DEFECT(dup_exec"; do
    G show "$HARNESS:${f%%:*}" 2> /dev/null | grep -qF -- "${f#*:}" ||
        refuse "${HARNESS:0:8}:${f%%:*} has no '${f#*:}' (not a gate v2 harness)"
done
for c in "$A" "$B"; do
    G cat-file -e "$c:$XREL/pc_preflight_build.sh" 2> /dev/null || refuse "${c:0:8} has no pc_preflight_build.sh (cannot build its PC preflight)"
    # The Pis build each arm's bench from that arm's own tree, so a service cell needs the bench in A and in B: an A/B
    # branch made before the service cells landed is rebased onto them first (A = that main, B = the rebased change).
    for n in $CORE_CELLS; do
        [ "$n" -ge 18 ] 2> /dev/null || continue
        G cat-file -e "$c:examples/perf_hil/tickle/service_call/client.c" 2> /dev/null ||
            refuse "${c:0:8} has no tickle/service_call bench, needed by service cell c$n (rebase the A/B onto the commit that added it)"
        break
    done
done
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

# The reader's selftest and the cell list, from the harness commit's own files (a private export, removed on exit).
TMPX=$(mktemp -d "$TMPDIR/core_ab_XXXXXX") || refuse "mktemp in $TMPDIR"
G archive "$HARNESS" "$XREL/core_ab_read.py" "$XREL/gate_v2.py" "$XREL/campaign_sweep.sh" "$XREL/gate_v2_lib.sh" "$XREL/pi_checkout.sh" |
    tar -x -C "$TMPX" || refuse "git archive of the harness files"
st=$(lim timeout 300 python3 "$TMPX/$XREL/core_ab_read.py" --selftest 2>&1)
str=$?
[ "$str" = 0 ] && [ "$(tail -1 <<<"$st" | cut -c1-11)" = "SELFTEST OK" ] ||
    { tail -15 <<<"$st" | sed 's/^/    | /'; refuse "core_ab_read.py --selftest (rc $str)"; }
say "core_ab_read.py --selftest: $(tail -1 <<<"$st")"
mapfile -t xspecs < <(cd "$TMPX" && PREFLIGHT_CELLS_ONLY=1 FWS=tickle CELLS="$CORE_CELLS" lim timeout 60 "$XREL/campaign_sweep.sh")
[ "${#xspecs[@]}" = "$N_SPECS" ] || refuse "campaign_sweep.sh listed ${#xspecs[@]} cell spec(s) for [$CORE_CELLS], not $N_SPECS"
say "core cells [$CORE_CELLS]: ${#xspecs[@]} cell specs"
printf '  spec: %s\n' "${xspecs[@]}"
[ "$DRY_RUN" = 1 ] || mkdir -p "$OUTD" "$TMPDIR" || refuse "mkdir $OUTD"
say "=== core ab queue: A=${A:0:12} B=${B:0:12} BRANCH=${BRANCH:-} TAG=$TAG harness ${HARNESS:0:12} ($RUNNER) out $OUTD ==="

# ---- PC preflight builds of A and B, each at its own commit (no ulimit: performance_test needs more than 2 GiB) -------
for sha in "$A" "$B"; do
    pf=$PF_BUILDS/${sha:0:12}
    if [ -f "$pf/BUILD_OK" ]; then
        say "PC build ${sha:0:8}: $pf ($(tail -1 "$pf/BUILD_OK" 2> /dev/null))"
        continue
    fi
    wt=/home/semih/tickle_pfbuild_${sha:0:8}
    if [ "$DRY_RUN" = 1 ]; then
        say "PC build ${sha:0:8}: missing ($pf/BUILD_OK), would build (~5 min):"
        [ -d "$wt" ] || would git -C "$REPO" worktree add -q --detach "$wt" "$sha"
        would git -C "$wt" checkout -q --detach "$sha"
        echo "WOULD RUN: (cd $wt && PF_BUILDS=$PF_BUILDS timeout 3600 $XREL/pc_preflight_build.sh) > $OUTD/pfbuild_${sha:0:8}.log 2>&1"
        continue
    fi
    [ -d "$wt" ] || G worktree add -q --detach "$wt" "$sha" || refuse "worktree $wt"
    git -C "$wt" checkout -q --detach "$sha" || refuse "checkout ${sha:0:8} in $wt"
    say "PC preflight build of ${sha:0:8} (~5 min) -> $OUTD/pfbuild_${sha:0:8}.log"
    (cd "$wt" && PF_BUILDS=$PF_BUILDS timeout 3600 "$XREL/pc_preflight_build.sh") > "$OUTD/pfbuild_${sha:0:8}.log" 2>&1
    [ -f "$pf/BUILD_OK" ] || { tail -15 "$OUTD/pfbuild_${sha:0:8}.log" | sed 's/^/    | /'; refuse "PREFLIGHT FAILED: no PC build of ${sha:0:8}"; }
    say "PC build ${sha:0:8}: $pf ($(tail -1 "$pf/BUILD_OK" 2> /dev/null))"
done

# ---- the core cell shapes through rig_preflight.sh for A and B (private namespaces on this PC) -----------------------
for sha in "$A" "$B"; do
    log=$OUTD/preflight_core_${sha:0:8}.log
    if [ "$DRY_RUN" = 1 ]; then
        printf 'WOULD RUN: (cd %q && ulimit -v %s && SHA=%q FWS=tickle timeout 3600 %q' "$RUNNER" "$MEM_KB" "$sha" "$RUNNER/$XREL/rig_preflight.sh"
        printf ' %q' "${xspecs[@]}"
        printf ') > %q 2>&1\n' "$log"
        continue
    fi
    if ! (cd "$RUNNER" && lim env SHA="$sha" FWS=tickle timeout 3600 "$RUNNER/$XREL/rig_preflight.sh" "${xspecs[@]}") > "$log" 2>&1; then
        tail -15 "$log" | sed 's/^/    | /'
        refuse "PREFLIGHT FAILED: core cells for ${sha:0:8} ($log)"
    fi
    say "preflight core ${sha:0:8}: $(tail -1 "$log" | cut -c1-120)"
done
[ "$DRY_RUN" = 1 ] || say "--- PC preflight clean ---"
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
# rig_lock.sh execs the harness copy of this script under the lock (CORE_AB_LOCKED=1). It runs as a child rather than
# replacing this process, so that a lock that never frees (rig_lock.sh exit 75) still ends with this queue's last line.
if [ "$DRY_RUN" = 1 ]; then
    echo "WOULD RUN: CORE_AB_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=$RIG_LOCK_WAIT A=$A B=$B TAG=$TAG OUTD=$OUTD RUNNER=$RUNNER" \
        "$RUNNER/examples/perf_hil/rig_lock.sh $RUNNER/$SELF_REL"
    echo "WOULD RUN (under the lock, ~$EST_MIN min): cd $RUNNER && TAG=$TAG A=$A B=$B GATE=v2 REPS=3 CELLS=\"$CORE_CELLS\"" \
        "PREFLIGHT=0 $RUNNER/$XREL/campaign_ab_chain.sh > $OUTD/chain.log 2>&1"
    echo "WOULD RUN (under the lock): cp /tmp/${TAG}_* /tmp/${TAG}.done $OUTD/; python3 $RUNNER/$XREL/core_ab_read.py" \
        "--compare $OUTD/${TAG}_compare.txt --done $OUTD/${TAG}.done --cells \"$CORE_CELLS\" --a ${A:0:12} --b ${B:0:12}" \
        "--arm-a $OUTD/${TAG}_A.txt --arm-b $OUTD/${TAG}_B.txt --arm-p $OUTD/${TAG}_P.txt > $OUTD/reading.txt"
    say "=== DRY_RUN clean: every check passed; nothing was built, preflighted, waited for or locked ==="
    rc=0
    exit 0
fi
say "--- taking the rig lock (hil, RIG_LOCK_WAIT=${RIG_LOCK_WAIT}s) ---"
export CORE_AB_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT A B TAG OUTD RUNNER CORE_CELLS EST_MIN
"$RUNNER/examples/perf_hil/rig_lock.sh" "$RUNNER/$SELF_REL"
lr=$?
case $lr in
0 | 2 | 3) rc=$lr ;;
75) say "the rig lock did not free within ${RIG_LOCK_WAIT}s"; rc=5 ;;
*) rc=1 ;;
esac
exit "$rc"

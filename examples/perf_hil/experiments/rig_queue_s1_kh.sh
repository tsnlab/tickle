#!/usr/bin/env bash
# rig_queue_s1_kh.sh - two rig measurements of one commit on origin/main under ONE hil lock. Written 2026-10-22 before
# any of its runs; the reading rules are s1_kh_rig_read.py's docstring and code (summarised here, decided there).
#
#   S1  ROADMAP "Wired work": Stage 1 / S1, the CPU-time half on the rig. stage1_payg.sh (d131d82e) asked on the PC
#       whether the shared-memory module costs anything where it is not used, and could not resolve CPU time there
#       (load 14 on 16 cores; its amendments moved to instructions: +1.7-1.9% user+kernel at p4 in full5, the residual).
#       Here the two processes are on two Pis, so the segment is on and cannot be found, and every datagram crosses
#       eth0. campaign_ab_chain.sh GATE=v2, mirrored blocks A P B B P A, REPS=S1_REPS (3) per block (6 runs an arm),
#       cells 1-4 (RELIABLE KEEP_ALL throughput p1-p4: stage1_payg.sh's shapes and KEEP_ALL bounds), all at SHA:
#         A  alloff: -Dtt_SEGMENT_ENABLED=0 -Dtt_SAMPLE_LENDING=0 on the layout-stable build (gate_v2_lib.sh A1)
#         P  alloff on the placement build (A3): |P - A| is every row's floor
#         B  on: the default build (the module compiled in) on the layout-stable build
#       Read by s1_kh_rig_read.py --s1: VOID on the chain's own VOID rules (core_ab_read.read), n < 5, or a treatment
#       not applied (tx_shm/rx_shm != 0, an alloff run making attach attempts, an on run making none); COST SHOWN when
#       cpu_s_per_Msample is WORSE (gate v2: Bonferroni t AND the floor) in >= 2 of 4 cells for one role; CANDIDATE at 1;
#       otherwise NO COST SHOWN.
#   KH  ROADMAP "Other open items": the pong Pi's tap -> receive return, ~4-5 us behind the vendors in KH (430ae86d,
#       2026-10-21: 18.0 against CycloneDDS 12.8, FastDDS 13.8 us, Bench BEST_EFFORT). Since then the wait ring landed
#       (769bd13c..71ed68f4: tt_receive()'s wait and read in one io_uring_enter). Re-measured exactly as KH was:
#       rmw_holdover_rig.sh part K's call of rmw_crosshost_rtt.sh (ROTATE=1 WAITS=block REPS=3 MSGS=bench, CAPTURE=1
#       STAMPS=1 SYSSTAMP_ARMS="off on" TICKLE_VARIANTS="default libc", the three rmws), read by rmw_pcap_split.py's
#       wake summary (K0-K3, its wire control K1) and then s1_kh_rig_read.py --kh against KH's split file: CLOSED /
#       NARROWED / NOT CLOSED / NOT COMPARABLE (a vendor's own figure moved >= 1 us: the rig moved) / NOT READ.
#
# BEFORE THE LOCK (nothing of the rig is taken; any failure refuses): SHA on origin/main and containing the hold-over
# 58bd6b1a and the wait ring 769bd13c; ping_node --dues and TT_HAL_LIBC_CALLS present at SHA; this script byte-identical
# to HARNESS's copy, which runs from RUNNER (a detached worktree at HARNESS, so no later edit reaches a running script);
# the readers' selftests; PC preflight of every build the rig will make, at SHA, in private network namespaces
# (rig_preflight.sh): cells 1-4 as on and as alloff, each checked for its treatment in the preflight's own RESULT lines
# (on: attach attempts > 0; alloff: 0), and cell 1 as -DTT_HAL_LIBC_CALLS (the KH libc variant's core); the rmw stack
# at SHA (pc_preflight_build.sh, PF_BUILDS/<sha12>, built when missing).
# UNDER THE LOCK: refuses if either Pi's eth0 carries netem/tbf/ingress (so anything found at exit is this queue's).
# One EXIT trap per process: under the lock it removes a netem/tbf root or ingress qdisc left on either Pi's eth0 and
# reports bench processes left there (by /proc/PID/exe); the last line is always "S1KH QUEUE END rc=<n> <date>".
#
# ESTIMATE (written before the run): S1 6 blocks x (Pi builds of 4 cells ~3 min + 12 runs ~4 min) = ~42 min; KH as
# its last run (2026-10-21 19:10-20:01) ~52 min; ~100 min under the lock (EST_MIN). The 1.5x deadline is printed when
# the lock is taken. Before it: the PC preflight ~8 min (~3 min with the rmw build present), then the queue ahead.
#
# Usage (from a checkout at the harness commit; launch detached, results outside the session):
#   setsid nohup examples/perf_hil/experiments/rig_queue_s1_kh.sh > ~/rig_results_safe/<name>.log 2>&1 < /dev/null &
#   env: SHA (origin/main) HARNESS (HEAD) RUNNER (~/tickle_runner_s1kh_<harness12>) PARTS ("S1 KH") S1_REPS (3)
#        K_REPS (3) RIG_LOCK_WAIT (36000) EST_MIN (100) KH_PREV (KH's split file) DRY_RUN=1 (checks only)
# Results: ~/rig_results_safe/s1kh/<sha8>_<stamp>/ (s1_reading.txt, kh_reading.txt, the chain's files, preflights) and
# ~/rig_results_safe/s1kh_rmwK_<sha8>_<stamp>.* (rmw_crosshost_rtt.sh's own files).
# Exit status: 0 both parts read, 1 refused or a preflight failed, 3 a part VOID / NOT READ, 5 the lock was not obtained.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
SELF_REL=examples/perf_hil/experiments/rig_queue_s1_kh.sh
XREL=examples/perf_hil/experiments
SAFE=$HOME/rig_results_safe
MEM_KB=2097152
PARTS=${PARTS:-S1 KH}
S1_REPS=${S1_REPS:-3}
K_REPS=${K_REPS:-3}
EST_MIN=${EST_MIN:-100}
RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
DRY_RUN=${DRY_RUN:-0}
PF_BUILDS=${PF_BUILDS:-/home/semih/pc_preflight_builds}
KH_PREV=${KH_PREV:-$SAFE/holdover_rmwK_430ae86d_20261021-185529.split.txt}
S1_CELLS="1 2 3 4"
ALLOFF="-Dtt_SEGMENT_ENABLED=0 -Dtt_SAMPLE_LENDING=0"
HOLDOVER=58bd6b1a
WAIT_RING=769bd13c
PIS="10.1.1.214 10.1.1.213"
export TMPDIR=/home/semih/tmp_build
say() { echo "$(date -Is) $*"; }
lim() { (ulimit -v "$MEM_KB" && "$@"); } # one PC command under the 2 GiB cap (the PC hosts other users' VMs)
case "$S1_REPS$K_REPS$EST_MIN" in *[!0-9]*) echo "S1_REPS, K_REPS and EST_MIN are whole numbers" >&2; exit 1 ;; esac
for p in $PARTS; do case "$p" in S1 | KH) ;; *) echo "PARTS takes S1 and/or KH, not '$p'" >&2; exit 1 ;; esac; done

# ---- under the lock ---------------------------------------------------------------------------------------------------
if [ "${S1KH_LOCKED:-0}" = 1 ]; then
    [ "${RIG_LOCK_HELD_HIL:-0}" = 1 ] || { say "REFUSED: S1KH_LOCKED=1 without the hil lock"; exit 1; }
    for v in SHA OUTD RUNNER TS; do [ -n "${!v:-}" ] || { say "REFUSED: $v is not set under the lock"; exit 1; }; done
    ulimit -v "$MEM_KB" # every PC process from here on; the builds are on the Pis
    cd "$RUNNER" || exit 1
    X=$RUNNER/$XREL
    K=$HOME/.ssh/tickle_ci_ed25519
    pi() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "$2" </dev/null; }
    rc_all=0
    # shellcheck disable=SC2329 # invoked by the EXIT trap below
    finish() {
        local rc=$? h q
        [ "$rc" = 0 ] && rc=$rc_all
        for h in $PIS; do
            q=$(pi "$h" "tc qdisc show dev eth0" 2>&1)
            case "$q" in
            *netem* | *tbf*) pi "$h" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1
                say "trap: removed a shaped root qdisc on $h eth0 ($(echo "$q" | head -1))" ;;
            esac
            case "$q" in *ingress*) pi "$h" "sudo -n tc qdisc del dev eth0 ingress" >/dev/null 2>&1
                say "trap: removed an ingress qdisc on $h eth0" ;; esac
            say "trap: $h eth0 now: $(pi "$h" "tc qdisc show dev eth0" 2>&1 | head -1)"
            # shellcheck disable=SC2016 # the $ are for the Pi's shell
            say "trap: bench processes left on $h: $(pi "$h" 'for q in /proc/[0-9]*; do e=$(readlink $q/exe 2>/dev/null); case "$e" in
                */ping_node|*/pong_node|*/rmw_zenohd|*_throughput_p[1-4]/client|*_throughput_p[1-4]/server) echo "${q#/proc/}=$e";; esac; done' 2>&1 | tr '\n' ' ')"
        done
        exit "$rc"
    }
    trap finish EXIT
    say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null); estimate ~$EST_MIN min, 1.5x deadline:" \
        "$(date -d "+$((EST_MIN * 3 / 2)) min" -Is) ---"
    for h in $PIS; do
        q=$(pi "$h" "tc qdisc show dev eth0") || { say "REFUSED: cannot read $h eth0's qdisc"; exit 1; }
        case "$q" in *netem* | *tbf* | *ingress*) say "REFUSED: $h eth0 is shaped ($q)"; exit 1 ;; esac
        say "qdisc $h eth0: $(echo "$q" | head -1)"
    done
    case " $PARTS " in *" S1 "*)
        # shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
        # shellcheck disable=SC1091 # the runner's copy, found at runtime
        . "$X/gate_v2_lib.sh"
        TAG=s1rig_${SHA:0:8}_$TS
        say "--- S1: on vs alloff, gate v2 A P B B P A, REPS=$S1_REPS, cells [$S1_CELLS], SHA ${SHA:0:8}, TAG $TAG ---"
        say "    A='$TICKLE_LAYOUT_CFLAGS $ALLOFF' P='$TICKLE_PLACEMENT_CFLAGS $ALLOFF' B='$TICKLE_LAYOUT_CFLAGS'"
        TAG=$TAG A=$SHA B=$SHA GATE=v2 REPS=$S1_REPS CELLS="$S1_CELLS" PREFLIGHT=0 \
            TICKLE_EXTRA_CFLAGS="$TICKLE_LAYOUT_CFLAGS $ALLOFF" P_EXTRA_CFLAGS="$TICKLE_PLACEMENT_CFLAGS $ALLOFF" \
            B_EXTRA_CFLAGS="$TICKLE_LAYOUT_CFLAGS" "$X/campaign_ab_chain.sh" >"$OUTD/s1_chain.log" 2>&1
        cr=$?
        cp /tmp/"${TAG}"_* /tmp/"${TAG}".done "$OUTD/" 2>/dev/null
        say "  S1 chain rc=$cr: $(cat "$OUTD/${TAG}.done" 2>/dev/null || echo '(no done file)')"
        timeout 300 python3 "$X/s1_kh_rig_read.py" --s1 --compare "$OUTD/${TAG}_compare.txt" --done "$OUTD/${TAG}.done" \
            --arm-a "$OUTD/${TAG}_A.txt" --arm-b "$OUTD/${TAG}_B.txt" --arm-p "$OUTD/${TAG}_P.txt" --cells "$S1_CELLS" \
            >"$OUTD/s1_reading.txt" 2>&1
        rr=$?
        grep -aE '^(  treatment|  CPU|S1 RIG VERDICT)' "$OUTD/s1_reading.txt" | cut -c1-240 | sed 's/^/  /'
        case $rr in 0 | 1) ;; *) rc_all=3 ;; esac
        ;;
    esac
    case " $PARTS " in *" KH "*)
        KO=$SAFE/s1kh_rmwK_${SHA:0:8}_$TS.txt
        say "--- KH: the Pi's receive return (sysstamp off/on, rmw_tickle default + libc, capture), as KH -> $KO ---"
        SHA=$SHA ROTATE=1 WAITS=block REPS=$K_REPS MSGS=bench TICKLE_VARIANTS="default libc" SYSSTAMP_ARMS="off on" \
            CAPTURE=1 STAMPS=1 OUT=$KO "$X/rmw_crosshost_rtt.sh" >"$KO.launch.log" 2>&1
        kr=$?
        say "  KH rc=$kr rows ok: $(grep -c ' | ok | ' "$KO" 2>/dev/null) of $(grep -cE '^rmw_[a-z_@]+ bench ' "$KO" 2>/dev/null)"
        timeout 600 python3 "$X/rmw_pcap_split.py" "$KO" >"${KO%.txt}.split.txt" 2>&1
        say "  KH split -> ${KO%.txt}.split.txt:"
        sed -n '/^=== kernel wake summary/,/^=== [^k]/p' "${KO%.txt}.split.txt" | sed 's/^/  /'
        timeout 120 python3 "$X/s1_kh_rig_read.py" --kh "${KO%.txt}.split.txt" --prev "$KH_PREV" >"$OUTD/kh_reading.txt" 2>&1
        kk=$?
        sed 's/^/  /' "$OUTD/kh_reading.txt"
        [ "$kk" = 0 ] || rc_all=3
        ;;
    esac
    say "S1KH_DONE rc=$rc_all"
    exit "$rc_all"
fi

# ---- before the lock --------------------------------------------------------------------------------------------------
rc=1
trap 'echo "S1KH QUEUE END rc=$rc $(date -Is)"' EXIT
refuse() { say "REFUSED: $*"; rc=1; exit 1; }
G() { git -C "$REPO" "$@"; }
say "=== rig_queue_s1_kh: parts '$PARTS'; estimate under the lock ~$EST_MIN min (S1 ~42, KH ~52), 1.5x = $((EST_MIN * 3 / 2)) min after the lock is taken; PC preflight first (~8 min) ==="
say "uptime: $(uptime)"
G fetch -q origin || refuse "git fetch origin failed"
SHA=$(G rev-parse --verify -q "${SHA:-origin/main}^{commit}") || refuse "no commit SHA=${SHA:-origin/main}"
HARNESS=$(G rev-parse --verify -q "${HARNESS:-HEAD}^{commit}") || refuse "no commit HARNESS=${HARNESS:-HEAD}"
G merge-base --is-ancestor "$SHA" origin/main || refuse "${SHA:0:8} is not on origin/main (the Pis fetch it)"
G merge-base --is-ancestor "$HOLDOVER" "$SHA" || refuse "${SHA:0:8} does not contain the hold-over $HOLDOVER"
G merge-base --is-ancestor "$WAIT_RING" "$SHA" || refuse "${SHA:0:8} does not contain the wait ring $WAIT_RING"
G show "$SHA:rmw_tickle/rmw_perf_pingpong/src/ping_node.cpp" | grep -q -- '"--dues"' || refuse "ping_node at ${SHA:0:8} has no --dues"
G show "$SHA:src/hal_linux.c" | grep -q 'TT_HAL_LIBC_CALLS' || refuse "hal_linux.c at ${SHA:0:8} has no TT_HAL_LIBC_CALLS"
G show "$SHA:include/tickle/hal_linux.h" | grep -qx '#define tt_RX_URING_WAIT 1' || refuse "the wait ring is not on by default at ${SHA:0:8}"
G show "$HARNESS:$SELF_REL" 2>/dev/null | cmp -s - "${BASH_SOURCE[0]}" ||
    refuse "this script differs from ${HARNESS:0:8}:$SELF_REL (commit it, or launch the harness commit's copy)"
for f in "$XREL/s1_kh_rig_read.py:S1 RIG VERDICT" "$XREL/campaign_ab_chain.sh:B_EXTRA_CFLAGS" \
    "$XREL/rmw_pcap_split.py:def wake_summary" "$XREL/rmw_pcap_split.py:io_uring_enter" "$XREL/rmw_crosshost_rtt.sh:ROTATE" \
    "$XREL/rmw_crosshost_rtt.sh:libc)" "examples/perf_hil/rig_lock.sh:RIG_LOCK_SCOPE"; do
    G show "$HARNESS:${f%%:*}" 2>/dev/null | grep -qF -- "${f#*:}" || refuse "${HARNESS:0:8}:${f%%:*} has no '${f#*:}'"
done
[ -r "$KH_PREV" ] || refuse "KH's split file $KH_PREV is not readable (the KH comparison needs it)"
TS=$(date +%Y%m%d-%H%M%S)
OUTD=${OUTD:-$SAFE/s1kh/${SHA:0:8}_$TS}
RUNNER=${RUNNER:-/home/semih/tickle_runner_s1kh_${HARNESS:0:12}}
if [ -d "$RUNNER" ]; then
    [ "$(git -C "$RUNNER" rev-parse HEAD 2>/dev/null)" = "$HARNESS" ] || refuse "$RUNNER exists and is not at ${HARNESS:0:8}"
    [ -z "$(git -C "$RUNNER" status --porcelain --untracked-files=no)" ] || refuse "$RUNNER has local edits"
elif [ "$DRY_RUN" != 1 ]; then
    G worktree add -q --detach "$RUNNER" "$HARNESS" || refuse "worktree add $RUNNER"
fi
say "SHA ${SHA:0:12} (origin/main $(G rev-parse --short origin/main)), harness ${HARNESS:0:12} in $RUNNER, out $OUTD"
RX=$RUNNER/$XREL
[ "$DRY_RUN" = 1 ] && RX=$HERE
for t in s1_kh_rig_read.py core_ab_read.py; do
    out=$(lim timeout 300 python3 "$RX/$t" --selftest 2>&1)
    [ "$(tail -1 <<<"$out" | cut -c1-11)" = "SELFTEST OK" ] || { tail -12 <<<"$out" | sed 's/^/    | /'; refuse "$t --selftest"; }
    say "$t --selftest: SELFTEST OK"
done
mapfile -t specs < <(cd "$RUNNER" 2>/dev/null || cd "$REPO" || exit; SHA=$SHA PREFLIGHT_CELLS_ONLY=1 FWS=tickle CELLS="$S1_CELLS" lim timeout 60 "$RX/campaign_sweep.sh")
[ "${#specs[@]}" = 4 ] || refuse "campaign_sweep.sh listed ${#specs[@]} cell spec(s) for [$S1_CELLS], not 4"
printf '  spec: %s\n' "${specs[@]}"
if [ "$DRY_RUN" = 1 ]; then
    say "=== DRY_RUN clean: checks passed; no preflight, no lock ==="
    rc=0
    exit 0
fi
mkdir -p "$OUTD" || refuse "mkdir $OUTD"

# ---- PC preflight: every build the rig will make, at SHA, in private namespaces ---------------------------------------
# preflight <name> <flags> <spec>...: rig_preflight.sh with TICKLE_EXTRA_CFLAGS=<flags>, its logs in $OUTD/preflight_<name>.
preflight() {
    local name=$1 flags=$2
    shift 2
    if ! (cd "$RUNNER" && lim env SHA="$SHA" FWS=tickle TICKLE_EXTRA_CFLAGS="$flags" PREFLIGHT_OUT="$OUTD/preflight_$name" \
        timeout 3600 "$RX/rig_preflight.sh" "$@") >"$OUTD/preflight_$name.log" 2>&1; then
        tail -15 "$OUTD/preflight_$name.log" | sed 's/^/    | /'
        refuse "PREFLIGHT FAILED: $name ($OUTD/preflight_$name.log)"
    fi
    say "preflight $name (TICKLE_EXTRA_CFLAGS='$flags'): $(tail -1 "$OUTD/preflight_$name.log" | cut -c1-110)"
}
# attempts <name>: "<jobs> <jobs whose client + server made an attach attempt> <jobs without the field>".
attempts() {
    local d n=0 hit=0 miss=0 vals
    for d in "$OUTD/preflight_$1"/jobs/*/; do
        n=$((n + 1))
        vals=$(cat "$d/client.log" "$d/server.log" 2>/dev/null | grep -o 'shm_attach_attempts=[0-9]*' | cut -d= -f2)
        if [ "$(grep -c . <<<"$vals")" -lt 2 ]; then miss=$((miss + 1)); continue; fi
        [ "$(awk '{s += $1} END {print s + 0}' <<<"$vals")" -gt 0 ] && hit=$((hit + 1))
    done
    echo "$n $hit $miss"
}
case " $PARTS " in *" S1 "*)
    preflight on "" "${specs[@]}"
    preflight alloff "$ALLOFF" "${specs[@]}"
    read -r n_on a_on m_on < <(attempts on)
    read -r n_off a_off m_off < <(attempts alloff)
    say "preflight treatment: on - $a_on of $n_on jobs attempted an attach ($m_on without both RESULT fields);" \
        "alloff - $a_off of $n_off ($m_off without)"
    [ "$n_on" = 4 ] && [ "$a_on" = 4 ] && [ "$m_on" = 0 ] || refuse "PREFLIGHT: the on build did not attempt an attach in every job"
    [ "$n_off" = 4 ] && [ "$a_off" = 0 ] && [ "$m_off" = 0 ] || refuse "PREFLIGHT: the alloff build attempted an attach (the flags did not reach it)"
    ;;
esac
case " $PARTS " in *" KH "*)
    preflight libc "-DTT_HAL_LIBC_CALLS" "${specs[0]}"
    pf=$PF_BUILDS/${SHA:0:12}
    if [ ! -f "$pf/BUILD_OK" ]; then
        wt=/home/semih/tickle_pfbuild_${SHA:0:8}
        [ -d "$wt" ] || G worktree add -q --detach "$wt" "$SHA" || refuse "worktree $wt"
        git -C "$wt" checkout -q --detach "$SHA" || refuse "checkout ${SHA:0:8} in $wt"
        say "PC rmw stack build of ${SHA:0:8} (~5 min) -> $OUTD/pfbuild_${SHA:0:8}.log"
        (cd "$wt" && PF_BUILDS=$PF_BUILDS timeout 3600 "$XREL/pc_preflight_build.sh") >"$OUTD/pfbuild_${SHA:0:8}.log" 2>&1
        [ -f "$pf/BUILD_OK" ] || { tail -15 "$OUTD/pfbuild_${SHA:0:8}.log" | sed 's/^/    | /'; refuse "PREFLIGHT FAILED: no PC rmw build of ${SHA:0:8}"; }
    fi
    say "PC rmw stack build ${SHA:0:8}: $pf ($(tail -1 "$pf/BUILD_OK" 2>/dev/null))"
    ;;
esac
say "--- PC preflight clean; taking the rig lock (hil, RIG_LOCK_WAIT=${RIG_LOCK_WAIT}s; now held by: $(cat /tmp/tickle-hil.lock 2>/dev/null)) ---"

# ---- the rig, under one hil lock (rig_lock.sh runs the harness copy as a child, so this process prints the last line) --
export S1KH_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT SHA OUTD RUNNER TS PARTS S1_REPS K_REPS EST_MIN KH_PREV
"$RUNNER/examples/perf_hil/rig_lock.sh" "$RUNNER/$SELF_REL"
lr=$?
case $lr in
0 | 3) rc=$lr ;;
75) say "the rig lock did not free within ${RIG_LOCK_WAIT}s"; rc=5 ;;
*) rc=1 ;;
esac
exit "$rc"

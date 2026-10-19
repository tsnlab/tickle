#!/usr/bin/env bash
# qos_be_ab.sh - did the QoS work lower TickLE's same-host BEST_EFFORT KEEP_LAST 1 max-rate goodput? A TickLE-only
# rig A/B with a placement control, written 2026-10-19 before any run. The question, the design, the metrics and the
# reading rule are qos_be_ab.py's header (implemented there); this script only runs it, through the existing
# ab_samehost.sh (gate v2: blocks A P B B P A, per-arm flags, identity and flag checks, sentinel, PC preflight, one
# lock) - nothing here measures or reads a value itself.
#
#   A  = 93ec2b3b (main just before the QoS work: the parent of e4fc0683, its first commit; NOT a1e8e887's parent
#        6624e9bc, which already holds 1,137 lines of the QoS core), layout-stable build (TICKLE_LAYOUT_CFLAGS)
#   P  = A rebuilt with TICKLE_PLACEMENT_CFLAGS (the placement control)       B = origin/main at launch, A's flags
#   cells (s6_transport_cells.sh, as SB ran them: tickle only, free, DUR 5 s + 2 s warm-up/cool-down, one Pi, shared
#   memory, BE_HISTORY=1 = -K 1 on client and server): best_effort_throughput p2 p3 p4, reliable_throughput p3
#   REPS 4 per block (8 per arm): `qos_be_ab.py --samplesize` on the SB files recommends 3 (p2/p3 power >= 0.99, p4
#   false VOID 0.05); 4 because those SDs come from 3 and 5 reps, and p4's chance to exclude its 1.2% rises 0.40 -> 0.56.
#   sentinel: A's best_effort_throughput p3 in every block (the Pi's drift; ab_samehost's control; never decides here)
#
# PHASES. Before the lock: fetch, resolve and check the commits (A must predate e4fc0683, B must hold 47eb05e8), the
# runner worktree at HARNESS_SHA (this script and its readers run from there, so an edit here never reaches a running
# copy), both readers' self-tests, a read-only check that neither Pi is shaped (netem/tbf on eth0 or lo), the two
# pre-registrations (ab_samehost's, then qos_be_ab.py's bound to its sha256), then ab_samehost.sh: its PC preflight of
# every arm (A, P, B, each with its flags as x86 gcc takes them: the aarch64-only -mno-fix-cortex-a53-843419 dropped)
# and the rig lock, taken ONCE inside this detached process for every block. After it: qos_be_ab.py summary. This
# script applies nothing to the Pis (no tc, no setting); s6 stops its own processes. One EXIT trap; the last line is
# always   QOSBE_DONE rc=<n> answer=<...> outb=<...>
#
# ESTIMATE: under the lock 6 blocks x 5 s6 calls (4 cells + the sentinel) x (30 s + 4 reps x ~36-42 s, ON and OFF arm
# each) ~ 87-99 min, plus the first build of each arm on the Pi; the PC preflight before it ~10 min. ~1 h 50 min in all;
# analyse an overrun at 2 h 45 min.
#
# Usage:
#   qos_be_ab.sh --selftest            both readers' self-tests (no host, no lock)
#   qos_be_ab.sh --prereg              resolve, write and print both pre-registrations and the estimate; nothing else
#   PREFLIGHT_ONLY=1 qos_be_ab.sh      the same, then the PC preflight of every arm; no lock, nothing on the rig
#   setsid nohup examples/perf_hil/experiments/qos_be_ab.sh > ~/rig_results_safe/qosbe_$(date +%Y%m%d-%H%M%S).log \
#     2>&1 < /dev/null &               the run (detached)
# env: A (default 93ec2b3b), B (default origin/main), REPS (default 4), HARNESS_SHA (default this checkout's HEAD),
#      RUNNER (/home/semih/tickle_runner_qosbe)
set -u
SELF_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SELF_X=$SELF_REPO/examples/perf_hil/experiments
if [ "${1:-}" = --selftest ]; then
    python3 "$SELF_X/ab_samehost.py" selftest | tail -1
    exec python3 "$SELF_X/qos_be_ab.py" --selftest
fi
MODE=run
[ "${1:-}" = --prereg ] && MODE=prereg
[ "${PREFLIGHT_ONLY:-0}" = 1 ] && MODE=preflight
MAIN=/home/semih/tickle
RUNNER=${RUNNER:-/home/semih/tickle_runner_qosbe}
HARNESS_SHA=${HARNESS_SHA:-$(git -C "$SELF_REPO" rev-parse HEAD)}
QOS_FIRST=e4fc0683 # tickle.h: struct tt_Topic becomes struct tt_TypeSupport - the first commit of the QoS work
QOS_DONE=47eb05e8  # docs: QoS plan step 9 - the last
PIS="10.1.1.214 10.1.1.213"
K=$HOME/.ssh/tickle_ci_ed25519
say() { echo "$(date -Is) $*"; }
pi() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "$2" </dev/null; }
RC=0 ANSWER=none OUTB=none
finish() { # the one EXIT trap: nothing was applied to the Pis, so it reverts nothing; it prints the last line
    local rc=$?
    [ "$RC" = 0 ] && RC=$rc
    echo "QOSBE_DONE rc=$RC answer=$ANSWER outb=$OUTB mode=$MODE $(date -Is)"
}
trap finish EXIT

git -C "$MAIN" fetch -q origin || { say "REFUSED: git fetch failed"; RC=1; exit 1; }
A=$(git -C "$MAIN" rev-parse --verify -q "${A:-93ec2b3b}^{commit}") || { say "REFUSED: A does not resolve"; RC=1; exit 1; }
B=$(git -C "$MAIN" rev-parse --verify -q "${B:-origin/main}^{commit}") || { say "REFUSED: B does not resolve"; RC=1; exit 1; }
for v in A B; do
    git -C "$MAIN" merge-base --is-ancestor "${!v}" origin/main ||
        { say "REFUSED: $v ${!v} is not on origin/main (the Pi fetches it)"; RC=1; exit 1; }
done
if git -C "$MAIN" merge-base --is-ancestor "$QOS_FIRST" "$A"; then
    say "REFUSED: A $A already holds the QoS work ($QOS_FIRST is its ancestor)"
    RC=1
    exit 1
fi
git -C "$MAIN" merge-base --is-ancestor "$QOS_DONE" "$B" || { say "REFUSED: B $B lacks the QoS work ($QOS_DONE)"; RC=1; exit 1; }
say "A=$A ($(git -C "$MAIN" log -1 --format=%s "$A" | cut -c1-80))"
say "B=$B ($(git -C "$MAIN" log -1 --format=%s "$B" | cut -c1-80))"
say "QoS commits between them touching src/ include/: $(git -C "$MAIN" rev-list --count "$A..$B" -- src include)"

if [ "$MODE" = prereg ]; then
    X=$SELF_X
else
    git -C "$SELF_REPO" cat-file -e "$HARNESS_SHA^{commit}" 2>/dev/null || { say "REFUSED: HARNESS_SHA $HARNESS_SHA unknown"; RC=1; exit 1; }
    if [ ! -d "$RUNNER" ]; then
        git -C "$SELF_REPO" worktree add -q --detach "$RUNNER" "$HARNESS_SHA" || { say "REFUSED: worktree $RUNNER"; RC=1; exit 1; }
    fi
    git -C "$RUNNER" checkout -q --detach "$HARNESS_SHA" || { say "REFUSED: checkout $HARNESS_SHA in $RUNNER"; RC=1; exit 1; }
    X=$RUNNER/examples/perf_hil/experiments
    say "harness ${HARNESS_SHA:0:8} in $RUNNER"
fi
python3 "$X/ab_samehost.py" selftest | tail -1 | grep -qx 'SELFTEST PASSED' ||
    { say "REFUSED: ab_samehost.py's selftest does not pass"; RC=1; exit 1; }
python3 "$X/qos_be_ab.py" --selftest | tail -1 | grep -qx 'selftest: PASS' ||
    { say "REFUSED: qos_be_ab.py's selftest does not pass"; RC=1; exit 1; }
say "self-tests: ab_samehost.py and qos_be_ab.py PASS"

if [ "$MODE" = run ]; then
    bad=0
    for h in $PIS; do
        for d in eth0 lo; do
            if ! q=$(pi "$h" "tc qdisc show dev $d" 2>&1); then
                say "REFUSED: could not read the qdisc of $h $d ($q)"
                bad=1
                continue
            fi
            case "$q" in
            *netem* | *tbf*) say "REFUSED: $h $d is shaped: $(echo "$q" | tr '\n' ' ')"; bad=1 ;;
            *) say "  $h $d not shaped" ;;
            esac
        done
    done
    [ "$bad" = 0 ] || { RC=1; exit 1; }
fi

# The ab_samehost invocation, identical for the pre-registration and the run (it rebuilds its pre-registration from
# this environment under the lock and refuses on any difference).
export GATE=v2 TAG=qosbe REPS="${REPS:-4}" DUR=5 BE_HISTORY=1 SENTINEL=$A A B \
    CELLS="best_effort_throughput:p2;best_effort_throughput:p3;best_effort_throughput:p4;reliable_throughput:p3" \
    DERIVED="cpu_us=1e6*(client.sched_cpu_s+server.sched_cpu_s)/server.recv" \
    WITNESS="*:server.win_recv_mbps,*:derived.cpu_us/lo" \
    CONTROL="best_effort_throughput:p3:server.win_recv_mbps" \
    TREATMENT="*:*:client.tx_shm>0 and server.rx_shm>0" \
    TMPDIR=/home/semih/tmp_build RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
unset OUTB TICKLE_EXTRA_CFLAGS P_EXTRA_CFLAGS B_EXTRA_CFLAGS C_EXTRA_CFLAGS C PRIMARY SECONDARY COMPARE CONTROL_SE_FLOOR
pre_out=$(PREREG_ONLY=1 "$X/ab_samehost.sh" 2>&1)
prc=$?
echo "$pre_out"
OUTB=$(echo "$pre_out" | sed -n 's/^PREREG_ONLY=1: pre-registration written to \(.*\)\.prereg\.json; nothing run.*/\1/p')
if [ "$prc" != 0 ] || [ -z "$OUTB" ] || [ ! -s "$OUTB.prereg.json" ]; then
    say "REFUSED: ab_samehost.sh wrote no pre-registration (exit $prc)"
    OUTB=none RC=1
    exit 1
fi
python3 "$X/qos_be_ab.py" --prereg "$OUTB.prereg.json" "$OUTB.qosbe_prereg.json" ||
    { say "REFUSED: qos_be_ab.py refused the pre-registration"; RC=1; exit 1; }
say "pre-registrations: $OUTB.prereg.json (sha256 $(sha256sum "$OUTB.prereg.json" | cut -c1-16)), $OUTB.qosbe_prereg.json"
if [ "$MODE" = prereg ]; then
    say "--prereg: nothing run, no preflight, no lock"
    exit 0
fi

export OUTB
[ "$MODE" = preflight ] && export PREFLIGHT_ONLY=1
say "--- ab_samehost.sh: PC preflight of every arm$([ "$MODE" = run ] && echo ', then the rig lock (hil) once, then every block') ---"
"$X/ab_samehost.sh"
arc=$?
say "ab_samehost.sh exit $arc"
if [ "$MODE" = preflight ]; then
    RC=$arc
    exit "$arc"
fi
tail -3 "$OUTB.driver.log" 2>/dev/null | sed 's/^/    /'
grep -q '^=== ALL_DONE' "$OUTB.driver.log" 2>/dev/null || say "NOTE: ab_samehost did not reach ALL_DONE; reading what there is"
python3 "$X/qos_be_ab.py" summary "$OUTB.qosbe_prereg.json" "$OUTB" >"$OUTB.qosbe_summary.txt" 2>&1
src=$?
ANSWER=$(sed -n 's/^ANSWER: .*? //p' "$OUTB.qosbe_summary.txt" | tail -1)
ANSWER=${ANSWER:-none}
grep -E '^(=== SAMPLE|  [a-z_]+:p[0-9] +A n=|WORSE|BETTER|NOT WORSE|VOID|NO VERDICT|CPU|SECOND|ANSWER)' \
    "$OUTB.qosbe_summary.txt" | sed 's/^/    /'
say "qos_be_ab.py summary exit $src -> $OUTB.qosbe_summary.txt"
[ "$arc" = 0 ] || RC=$arc

#!/usr/bin/env bash
# The second batch of ROADMAP "Other open items" measurement debt (written 2026-10-20, before any of its runs):
# "DATA_FRAG under-loss throughput in the full campaign" and "discovery join time (M3)".
#
# WHAT THE DEBT IS (traced 2026-10-20; the ROADMAP line is a one-line summary of older text):
#   DATA_FRAG under loss - DATAFRAG_PLAN 14 (2026-09-26, at 6234e915^; COMPARISON 4.4, 3d1ffeeb): per-datagram seq_no
#     raised throughput at 5% loss to 830-857 Mbps at 2 fragments (p4) and 527-536 Mbps at 4 fragments, "an
#     unpredicted result, to be confirmed in the full cross-vendor campaign". The campaign has a p4 cell only: c6, which
#     RESULTS `D` (d58873cf, 2026-10-20, 12 reps) re-measured - TickLE 888-898 Mbps, frag_reassembled = sent,
#     2.077 datagrams/sample: the 2-fragment half is confirmed. The 4-fragment figure was never a campaign cell (build.sh
#     TICKLE_DATAGRAM_BYTES=800 is "a TickLE-only diagnostic ... without adding a fifth payload shape to every
#     framework") and has not been measured since bceddf2a. Part F re-measures it at SHA with the same harness,
#     frag_count_scaling.sh, unchanged, REPS reps per arm. A cross-vendor 4-fragment cell would need non-default vendor
#     fragment sizes - the user's decision, not made here.
#   Discovery join time (M3) - CONTEXT_NODE_PLAN stage 3 (2026-09-28): "Not measured here: M3 join time.
#     discovery_join.sh counts alive entities ... so the tool needs adapting first." DISCOVERY_PLAN 10 (2026-09-27)
#     did adapt it and measured 3d3ed747 (stage 3) at 2.0 / 1.8 ms (E = 4 / 32), but no published document carries an
#     M3 figure since the 2026-10-05 consolidation, and the core has changed since (QoS redesign, wire changes). Part M
#     runs discovery_join.sh join 8 E 10 on this PC, in private namespaces, for SHA and for the parent 3d3ed747 (tool
#     c828e19f^, the tool of its own era) as the control, interleaved P C C P per E = 4 and 32: 20 joins per arm per
#     cell.
#
# HOW TO READ IT, written before running and implemented in debt2_read.py (--prereg prints the rules, --selftest checks
# that each verdict is reachable and each control can void): sample counts first; F is VOID unless the loss0 arms read 2
# and 4 datagrams per sample and the 2-fragment arms reproduce the lossless ceiling and D's c6 figure; F1 CONFIRMED /
# HIGHER / BELOW against 527-536 Mbps; F2 per-datagram or whole-sample retransmission; M is VOID per cell unless the
# parent reproduces its own published median within 5 ms; M3 PASS if SHA's median is at most the parent's + 5 ms.
# Nothing is written into the documents by this script.
#
# REFUSALS (each prints why, then the DONE line):
#   - SHA unset, not a full 40-hex id, not on origin/main after a fetch, or a fetch that fails;
#   - this checkout not at SHA, or local changes under examples/perf_hil;
#   - debt2_read.py's selftest failing;
#   - the PC preflight failing: rig_preflight.sh for p4 at N0 and N1 (5% loss), TickLE only, at the default datagram and
#     at TICKLE_DATAGRAM_BYTES=800 (the latter must show datagram_bytes=800 in its RESULT lines); and discovery_join.sh
#     join 3 4 1 for both M arms, each with an ok row;
#   - under the lock: a qdisc other than the default (netem or tbf) on either Pi's eth0, before F or after it;
#   - under the lock: not enough time left before NOT_AFTER (default 07:30) for 1.5x the estimate.
#
# THE RIG. The lock (hil) is taken ONCE, inside this detached process, for F and M (M runs on the PC only, but under
# the lock, so no rig campaign launched from this PC loads it during the join timings). This script applies nothing to
# the Pis; frag_count_scaling.sh applies netem on the client's eth0 and removes it in its own EXIT trap. The PC
# preflight runs with TICKLE_EXTRA_CFLAGS and GATE_BUILD_ARCH unset, so the rig's aarch64 flags never reach an x86
# build.
#
# ESTIMATE. PC preflight ~3 min (two TickLE-only rig_preflight runs and two discovery core builds). F: two deploy+build
# rounds on both Pis ~4 min each, then 4 arms x REPS 8 = 32 runs at ~20 s = ~11 min -> ~19 min. M: 8 discovery_join.sh
# runs of 5 + 10 x 9 + 5 s = ~100 s each -> ~14 min. Reading < 1 min. In all ~37 min; EST_MIN 40, 1.5x = 60 min.
#
# Usage (from a detached worktree at SHA, which must be on origin/main; results in ~/rig_results_safe):
#   git -C /home/semih/tickle fetch origin && SHA=$(git -C /home/semih/tickle rev-parse origin/main)
#   git -C /home/semih/tickle worktree add --detach /home/semih/tickle_runner_debt2 "$SHA"
#   cd /home/semih/tickle_runner_debt2 && SHA=$SHA setsid nohup examples/perf_hil/experiments/debt2_remeasure.sh \
#     > ~/rig_results_safe/debt2_launch.log 2>&1 < /dev/null &
# The last line is always "=== DEBT2 DONE rc=<n> ... ===". DRY_RUN=1 makes the refusals and prints the plan, without
# the preflight, the lock or the rig. PREFLIGHT_ONLY=1 stops after the PC preflight, before the lock.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SAFE=$HOME/rig_results_safe
REPS=${REPS:-8}
EST_MIN=${EST_MIN:-40}
NOT_AFTER=${NOT_AFTER:-07:30}
DRY_RUN=${DRY_RUN:-0}
PARENT=3d3ed747dcd1a2627c7ca81ddf836c99d41010c1
PARENT_TOOL=c828e19f^
SHA=${SHA:-}
DJ="$HERE/discovery_scaling/discovery_join.sh"
say() { echo "$(date -Is) $*"; }

if [ "${DEBT2_LOCKED:-0}" != 1 ]; then
    # The one EXIT trap of this (outer) instance: the DONE line, whatever happened. The instance under the lock is a
    # child, not an exec, so a lock that never frees still ends here with a DONE line.
    trap 'echo "=== DEBT2 DONE rc=$? sha ${SHA:-unset} $(date -Is) ==="' EXIT
    case "$SHA" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;;
    *) say "REFUSED: set SHA to the full id of a commit on origin/main (got '${SHA}')"; exit 2 ;;
    esac
    [ ${#SHA} = 40 ] || { say "REFUSED: SHA must be the full 40-hex id, not '${SHA}'"; exit 2; }
    git -C "$REPO" fetch -q origin main || { say "REFUSED: git fetch origin main failed - could not check SHA"; exit 1; }
    git -C "$REPO" merge-base --is-ancestor "$SHA" origin/main 2>/dev/null ||
        { say "REFUSED: $SHA is not on origin/main (the Pis fetch it)"; exit 1; }
    head=$(git -C "$REPO" rev-parse HEAD)
    [ "$head" = "$SHA" ] ||
        { say "REFUSED: this checkout ($REPO) is at $head, not $SHA - run from a detached worktree at SHA"; exit 1; }
    if ! git -C "$REPO" diff --quiet HEAD -- examples/perf_hil; then
        say "REFUSED: $REPO has local changes under examples/perf_hil - the harness would not be $SHA's"
        exit 1
    fi
    python3 "$HERE/debt2_read.py" --selftest | tail -1 | grep -q '^SELFTEST OK' ||
        { say "REFUSED: debt2_read.py's selftest does not pass"; exit 1; }
    case "$REPS" in '' | *[!0-9]*) say "REFUSED: REPS='$REPS'"; exit 2 ;; esac
    say "=== debt 2: sha ${SHA:0:8}; F 4-fragment DATA_FRAG under loss ($REPS reps an arm); M discovery join (M3) ==="
    say "estimate ~${EST_MIN} min (1.5x $((EST_MIN * 3 / 2)) min); must finish before $NOT_AFTER"
    say "uptime: $(uptime)"
    python3 "$HERE/debt2_read.py" --prereg
    if [ "$DRY_RUN" = 1 ]; then
        say "plan F: frag_count_scaling.sh SHA=$SHA REPS=$REPS (frag2/frag4 x loss0/loss5)"
        say "plan M: for E in 4 32: P C C P, discovery_join.sh join 8 E 10 (P: core ${PARENT:0:8} tool $PARENT_TOOL)"
        say "DRY_RUN=1: no preflight, no lock, nothing run"
        exit 0
    fi
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        mkdir -p "$SAFE/preflight"
        pfb=$SAFE/preflight/debt2_${SHA:0:8}_$(date +%Y%m%d-%H%M%S)
        for dg in default 800; do
            say "--- PC preflight F, TickLE p4 N0+N1, datagram $dg -> $pfb.f$dg ---"
            (
                unset TICKLE_EXTRA_CFLAGS GATE_BUILD_ARCH TICKLE_DATAGRAM_BYTES
                [ "$dg" = default ] || export TICKLE_DATAGRAM_BYTES=$dg
                TMPDIR=/home/semih/tmp_build PREFLIGHT_OUT="$pfb.f$dg" SHA="$SHA" FWS=tickle \
                    nice -n 10 "$HERE/rig_preflight.sh" 'reliable_throughput:p4:N0::-Q' 'reliable_throughput:p4:N1::-Q'
            ) >"$pfb.f$dg.log" 2>&1 || {
                say "REFUSED: PC preflight F (datagram $dg) failed - not taking the rig ($pfb.f$dg.log):"
                tail -15 "$pfb.f$dg.log" | sed 's/^/      /'
                exit 1
            }
            want=1472; [ "$dg" = default ] || want=$dg
            n=$(grep -rhoE "datagram_bytes=$want( |$)" "$pfb.f$dg" 2>/dev/null | wc -l)
            [ "$n" -gt 0 ] || { say "REFUSED: preflight F ($dg) has no datagram_bytes=$want in $pfb.f$dg"; exit 1; }
            say "    PASS: $(tail -1 "$pfb.f$dg.log"); datagram_bytes=$want in $n RESULT fields"
        done
        for arm in P C; do
            if [ $arm = P ]; then s=$PARENT; t=$PARENT_TOOL; else s=$SHA; t=""; fi
            say "--- PC preflight M, arm $arm (core ${s:0:8}${t:+, tool $t}) -> $pfb.m$arm.txt ---"
            SHA=$s TOOL_SHA=$t OUT="$pfb.m$arm.txt" nice -n 10 "$DJ" join 3 4 1 >"$pfb.m$arm.log" 2>&1
            grep -q '^join 1 | ok | ' "$pfb.m$arm.txt" 2>/dev/null || {
                say "REFUSED: preflight M arm $arm has no ok join ($pfb.m$arm.log):"
                tail -8 "$pfb.m$arm.log" | sed 's/^/      /'
                exit 1
            }
            say "    PASS: $(grep '^join 1 ' "$pfb.m$arm.txt" | cut -c1-140)"
        done
    fi
    if [ "${PREFLIGHT_ONLY:-0}" = 1 ]; then say "PREFLIGHT_ONLY=1: stopping before the lock"; exit 0; fi
    say "--- taking the rig lock (hil), once for F and M (wait at most ${RIG_LOCK_WAIT:-3600} s) ---"
    DEBT2_LOCKED=1 SHA=$SHA REPS=$REPS RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-3600} \
        "$REPO/examples/perf_hil/rig_lock.sh" "$HERE/debt2_remeasure.sh" "$@"
    exit $?
fi

# ---- under the lock (a child of the outer instance; no trap of its own: it applies nothing to the Pis) -----------
say "--- rig lock held: $(tr '\n' ' ' </tmp/tickle-hil.lock 2>/dev/null) ---"
K=$HOME/.ssh/tickle_ci_ed25519
unshaped() { # $1 when
    local h q
    for h in 10.1.1.214 10.1.1.213; do
        if ! q=$(ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "tc qdisc show dev eth0" </dev/null 2>&1); then
            say "REFUSED ($1): could not read the qdisc on $h (ssh failed): $q"
            return 1
        fi
        case "$q" in *netem* | *tbf*)
            say "REFUSED ($1): $h eth0 is shaped with the lock held: $(tr '\n' ' ' <<<"$q")"
            return 1 ;;
        esac
        say "link $h eth0 unshaped ($1): $(tr '\n' ' ' <<<"$q" | cut -c1-100)"
    done
}
unshaped "before F" || exit 1
now=$(date +%s)
deadline=$(date -d "$NOT_AFTER" +%s) || { say "REFUSED: NOT_AFTER='$NOT_AFTER' is not a time"; exit 2; }
[ "$deadline" -gt "$now" ] || deadline=$((deadline + 86400))
if [ $((now + EST_MIN * 90)) -gt "$deadline" ]; then
    say "REFUSED: 1.5x the estimate ($((EST_MIN * 3 / 2)) min) from now passes $NOT_AFTER"
    exit 1
fi
TS=$(date +%Y%m%d-%H%M%S)
OUTB=$SAFE/debt2_${SHA:0:8}_$TS

say "--- F: frag_count_scaling.sh, REPS=$REPS -> $OUTB.frag.txt ---"
SHA=$SHA REPS=$REPS OUT="$OUTB.frag.txt" "$HERE/frag_count_scaling.sh" >"$OUTB.frag.log" 2>&1
rc_f=$?
say "    F rc=$rc_f, rows $(grep -cE '^frag[24] loss[05] rep[0-9]+ \| ' "$OUTB.frag.txt" 2>/dev/null), last line:" \
    "$(tail -1 "$OUTB.frag.txt" 2>/dev/null | cut -c1-120)"
unshaped "after F" || rc_f=9

say "--- M: discovery join, N=8, E 4 and 32, P C C P (P core ${PARENT:0:8} tool $PARENT_TOOL) ---"
rc_m=0
m_files=()
for e in 4 32; do
    r=0
    for arm in P C C P; do
        r=$((r + 1))
        if [ $arm = P ]; then s=$PARENT; t=$PARENT_TOOL; else s=$SHA; t=""; fi
        f=$OUTB.m3_E${e}_$r$arm.txt
        say "    E=$e run $r arm $arm (core ${s:0:8}), uptime: $(uptime | sed 's/.*load/load/')"
        SHA=$s TOOL_SHA=$t OUT="$f" nice -n 10 "$DJ" join 8 "$e" 10 >"$f.log" 2>&1 || rc_m=$?
        say "      ok $(grep -c '^join [0-9]* | ok | ' "$f" 2>/dev/null) of $(grep -c '^join ' "$f" 2>/dev/null)"
        m_files+=("$f")
    done
done

say "--- reading -> $OUTB.reading.txt ---"
python3 "$HERE/debt2_read.py" --frag "$OUTB.frag.txt" --main "$SHA" --m3 "${m_files[@]}" >"$OUTB.reading.txt" 2>&1
grep -E '^(F|F1|F2|M|M3) ' "$OUTB.reading.txt" | sed 's/^/  /'
say "rc: F $rc_f, M $rc_m"
[ "$rc_f" = 0 ] && [ "$rc_m" = 0 ]

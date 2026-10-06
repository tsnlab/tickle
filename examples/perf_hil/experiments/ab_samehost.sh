#!/usr/bin/env bash
# ab_samehost.sh - a same-host, TickLE-only A/B of two (or three) pushed commits over a list of s6 cells, 2026-10-06.
# The general form of ab_frag_fastpath.sh's part 1, which stays as it was (it may be queued).
#
# DESIGN. Arms A, B (and C for base / fix-only / full) run in mirrored blocks - A B B A, or A B C C B A - so a drift of
# the Pi over the session lands on every arm alike. Each block runs every cell in CELLS through s6_transport_cells.sh
# (FRAMEWORKS=tickle: its own build identity, ON/OFF arms and witness), REPS repetitions per block and cell, DUR s.
#
# PRE-REGISTRATION, ENFORCED IN CODE (ab_samehost.py; every rule is in its header). Before anything runs:
#   - the invocation is parsed and REFUSED unless it names PRIMARY metrics, a CONTROL and a TREATMENT check, every
#     item names a cell that is in CELLS, every primary has a known direction, and every expression is plain arithmetic
#     over fields the s6 output records;
#   - it is printed and written to $OUTB.prereg.json (+ .prereg.txt), and its sha256 goes into the driver log before
#     the first block; under the lock it is rebuilt from the environment and must be byte-identical to the file;
#   - the verdicts are computed from that file alone, never from the environment of whoever reads the results.
# Verdict per comparison (B-A, C-A by default): VOID (a treatment failed, the two arms built the same client binary for
# a cell with a primary, or a control moved), NO VERDICT (n < 2 for a primary or control in an arm, or a treatment not checkable), else WORSE /
# IMPROVED / PASS at 2 x SE with a Bonferroni threshold for (primaries x comparisons), as ab_compare.py.
#
# Before the rig lock, every commit is preflighted on this PC (rig_preflight.sh, PREFLIGHT_TOPO=samens: both processes
# in one private namespace) on every cell; a failure refuses to take the rig. PREFLIGHT=0 skips it. The lock is taken
# once, for all blocks (a per-cell lock lets a queued campaign cut in between cells).
#
# Usage (A/B/C may be any ref the PC can resolve; they are fixed to full SHAs, which must be on origin):
#   A=origin/main B=<sha> [C=<sha>] TAG=<name> CELLS="scen:size:extra args;..." PRIMARY="cell:metric,..." \
#   CONTROL="cell:metric,..." TREATMENT="cell:ARMS:expr;..." [SECONDARY=...] [DERIVED="name=expr;..."] \
#   [COMPARE="B-A,C-A"] [REPS=5] [DUR=10] examples/perf_hil/experiments/ab_samehost.sh
#   PREREG_ONLY=1 ...           resolve, refuse or print the pre-registration and the estimate, then stop
#   ab_samehost.sh --selftest    the reading shown to decide on ab_samehost_fixture.txt (no host, no lock)
# Launch detached (setsid nohup ... < /dev/null &); the deliverables are
#   ~/rig_results_safe/ab_samehost_<TAG>_<stamp>.{prereg.json,prereg.txt,driver.log,summary.txt,b<n>_<sha8>_<cell>.txt}
# and the driver log always ends with "=== ab_samehost ended" (success or not); "=== ALL_DONE" only on success.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
if [ "${1:-}" = --selftest ]; then
    exec python3 "$X/ab_samehost.py" selftest
fi
TAG=${TAG:?TAG=<name> (letters, digits, . _ -)}
case "$TAG" in *[!A-Za-z0-9._-]*) echo "TAG='$TAG': letters, digits, . _ - only" >&2; exit 2 ;; esac
export TAG REPS="${REPS:-5}" DUR="${DUR:-10}" CELLS="${CELLS:-}" PRIMARY="${PRIMARY:-}" CONTROL="${CONTROL:-}" \
    SECONDARY="${SECONDARY:-}" TREATMENT="${TREATMENT:-}" DERIVED="${DERIVED:-}" COMPARE="${COMPARE:-}"
export RIG_LOCK_SCOPE=hil

if [ -z "${OUTB:-}" ]; then
    # Fixed here, before the lock, so the run measures the commits named at launch even if a branch moves meanwhile.
    git -C "$REPO" fetch -q origin || { echo "REFUSED: git fetch origin failed" >&2; exit 2; }
    for v in A B C; do
        ref=${!v:-}
        [ -z "$ref" ] && continue
        full=$(git -C "$REPO" rev-parse --verify -q "$ref^{commit}") || { echo "REFUSED: $v=$ref does not resolve" >&2; exit 2; }
        # The rig checks out by SHA after a fetch, so an unpushed commit would fail there, after the lock is taken.
        if [ -z "$(git -C "$REPO" branch -r --contains "$full" 2>/dev/null)" ]; then
            echo "REFUSED: $v=$ref ($full) is on no origin branch - push it first" >&2
            exit 2
        fi
        export "$v=$full"
    done
    OUTB=$HOME/rig_results_safe/ab_samehost_${TAG}_$(date +%Y%m%d-%H%M%S)
    export OUTB
    mkdir -p "$(dirname "$OUTB")"
    python3 "$X/ab_samehost.py" prereg "$OUTB.prereg.json" >"$OUTB.prereg.txt" ||
        { cat "$OUTB.prereg.txt"; rm -f "$OUTB.prereg.json"; exit 2; }
    cat "$OUTB.prereg.txt"
fi
mapfile -t CELL_LINES < <(python3 "$X/ab_samehost.py" cells "$OUTB.prereg.json")
BLOCKS=$(printf '%s\n' "${CELL_LINES[@]}" | sed -n 's/^BLOCKS|//p')
[ -n "$BLOCKS" ] || { echo "REFUSED: no cell list from $OUTB.prereg.json" >&2; exit 2; }
SHAS=$(for v in A B C; do [ -n "${!v:-}" ] && echo "${!v}"; done)

if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    ncell=$((${#CELL_LINES[@]} - 1))
    nblk=$(echo "$BLOCKS" | wc -w)
    # ~30 s of build and setup per s6 call, and each repetition runs the ON and the OFF arm for about DUR + 16 s each
    # (warm-up/cool-down, server stop, ssh) - calibrated on ab_frag_fastpath's latency cells, 2026-10-06 (4.5 min per
    # cell at REPS=5 DUR=10).
    est=$((nblk * ncell * (30 + REPS * 2 * (DUR + 16)) / 60))
    echo "ESTIMATE: $nblk blocks x $ncell cells x REPS=$REPS at DUR=$DUR s -> about $est min on the rig, plus the lock wait"
    if [ "${PREREG_ONLY:-0}" = 1 ]; then
        echo "PREREG_ONLY=1: pre-registration written to $OUTB.prereg.json; nothing run, no lock taken"
        exit 0
    fi
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        specs=()
        for l in "${CELL_LINES[@]}"; do
            [[ $l == BLOCKS* ]] && continue
            IFS='|' read -r scen size extra _ <<<"$l"
            specs+=("$scen:$size:N0:$extra:-Q")
        done
        for sha in $SHAS; do
            echo "--- preflight ${sha:0:8} on this PC ($(date +%T))"
            if ! SHA=$sha PREFLIGHT_TOPO=samens FWS=tickle "$X/rig_preflight.sh" "${specs[@]}" \
                >"$OUTB.preflight_${sha:0:8}.log" 2>&1; then
                tail -25 "$OUTB.preflight_${sha:0:8}.log"
                echo "REFUSING TO TAKE THE RIG: preflight failed at $sha, see $OUTB.preflight_${sha:0:8}.log" >&2
                exit 1
            fi
        done
    fi
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi

say() { echo "$*" | tee -a "$OUTB.driver.log"; }
on_exit() { echo "=== ab_samehost ended $(date -Is) ===" >>"$OUTB.driver.log"; }
trap on_exit EXIT
# The file the verdicts will be read from must still be what this invocation registered.
if ! python3 "$X/ab_samehost.py" prereg "$OUTB.prereg.check.json" >/dev/null 2>&1 ||
    ! cmp -s "$OUTB.prereg.json" "$OUTB.prereg.check.json"; then
    say "REFUSED: $OUTB.prereg.json differs from this invocation's pre-registration (edited after launch?)"
    exit 2
fi
rm -f "$OUTB.prereg.check.json"
say "=== ab_samehost $(date -Is) TAG=$TAG blocks: $BLOCKS REPS=$REPS DUR=$DUR ==="
say "    prereg sha256 $(sha256sum "$OUTB.prereg.json" | cut -c1-64)  ($OUTB.prereg.json)"
for v in A B C; do [ -n "${!v:-}" ] && say "    $v=${!v}"; done
n=0
for L in $BLOCKS; do
    n=$((n + 1))
    sha=${!L}
    for l in "${CELL_LINES[@]}"; do
        [[ $l == BLOCKS* ]] && continue
        IFS='|' read -r scen size extra slug <<<"$l"
        out="$OUTB.b${n}_${sha:0:8}_${slug}.txt"
        say "--- block $n arm $L ${sha:0:8} $scen $size '$extra' ($(date +%T))"
        FRAMEWORKS=tickle SCEN=$scen SIZE=$size DUR=$DUR REPS=$REPS SHA=$sha CLI_ARGS="$extra" OUT="$out" PREFLIGHT=0 \
            "$X/s6_transport_cells.sh" >"$out.log" 2>&1
        rc=$?
        cnt=$(grep -c '^ *arm=ON RESULT: .*framework=tickle' "$out" 2>/dev/null)
        say "    exit $rc, ${cnt:-no output file, so no} arm=ON RESULT lines (client and server)"
    done
done
python3 "$X/ab_samehost.py" summary "$OUTB.prereg.json" "$OUTB" >"$OUTB.summary.txt" 2>&1
rc=$?
grep -E '^(VERDICT|OVERALL)' "$OUTB.summary.txt" | tee -a "$OUTB.driver.log"
say "=== ALL_DONE $(date -Is) summary rc=$rc ($OUTB.summary.txt) ==="

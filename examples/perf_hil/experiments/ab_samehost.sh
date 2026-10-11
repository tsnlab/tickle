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
# GATE V2 (the default since 2026-10-11; the reading is ab_samehost.py's header and gate_v2.py's): a fourth arm P = A's
# commit rebuilt with P_EXTRA_CFLAGS (default TICKLE_PLACEMENT_CFLAGS, -falign-functions=128, gate_v2_lib.sh), blocks
# A P B B P A (A P B C C B P A); per-arm flags - A and the sentinel TICKLE_EXTRA_CFLAGS (default, when UNSET, the
# layout-stable TICKLE_LAYOUT_CFLAGS, -falign-functions=64; set and empty: the plain build), B and C B_EXTRA_CFLAGS /
# C_EXTRA_CFLAGS (default A's: the gate's own validation sets B = A's commit under another alignment), P P_EXTRA_CFLAGS;
# REFUSED when P's flags equal A's. Every TickLE process is counted (INSN_COUNT=1, s6_witness_check.sh), and a cell
# whose RESULT lines carry no insn=ok at all stops the run VOID (the counter is broken, not the change). PRIMARY is
# executed instructions per sample / round trip (default every cell, client and server, user and kernel); WITNESS
# (required) holds what PRIMARY named under v1. GATE=v1 is the old A B B A run and reading, with one TICKLE_EXTRA_CFLAGS
# for every arm (default the layout-stable build too) and INSN_COUNT=0 unless set.
#
# Usage (A/B/C may be any ref the PC can resolve; they are fixed to full SHAs, which must be on origin):
#   A=origin/main B=<sha> [C=<sha>] TAG=<name> CELLS="scen:size:extra args;..." [PRIMARY="cell:metric,..."] \
#   WITNESS="cell:metric,..." CONTROL="cell:metric,..." TREATMENT="cell:ARMS:expr;..." [SECONDARY=...] \
#   [DERIVED="name=expr;..."] [COMPARE="B-A,C-A"] [SENTINEL=<ref>] [REPS=5] [DUR=10] [TICKLE_EXTRA_CFLAGS=<flags>] \
#   [P_EXTRA_CFLAGS=<flags>] [B_EXTRA_CFLAGS=<flags>] [GATE=v2|v1] examples/perf_hil/experiments/ab_samehost.sh
#   TICKLE_EXTRA_CFLAGS: compiler flags for every arm's (and the sentinel's) TickLE core and bench, checked per cell
#   against the build's own log (below, where it is read).
#   SENTINEL: the CONTROL cells are run at this commit (usually A) in every block, after the block's own cells, and
#   the controls are read from those runs only (ab_samehost.py's header: a control the change cannot compile into,
#   judged on block means).
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
    SECONDARY="${SECONDARY:-}" TREATMENT="${TREATMENT:-}" DERIVED="${DERIVED:-}" COMPARE="${COMPARE:-}" \
    SENTINEL="${SENTINEL:-}" CONTROL_SE_FLOOR="${CONTROL_SE_FLOOR:-}" WITNESS="${WITNESS:-}" GATE="${GATE:-v2}"
export RIG_LOCK_SCOPE=hil
case "$GATE" in v1 | v2) ;; *) echo "GATE='$GATE': v2 (default) or v1" >&2; exit 2 ;; esac
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$X/gate_v2_lib.sh"
# Gate v2 A1: UNSET is the layout-stable build (this must run before anything reads the variable).
layout_cflags_default
# TICKLE_EXTRA_CFLAGS (2026-10-11): compiler flags for the TickLE core and bench of EVERY arm and the sentinel (a
# placement control: -falign-functions=64 compiles the same code differently without changing what it computes).
# s6_transport_cells.sh -> s6_witness_check.sh builds each cell under `bash -x` and checks the flags in that build's own
# log (build_flags_check.sh); this driver then refuses to go on past a cell whose file does not carry, for both its ON
# and OFF arm, the line "arm <ARM> TICKLE_EXTRA_CFLAGS='<flags>' reached core and bench" - an arm must report its
# treatment, and empty is checked too (the plain prefix). Recorded in the pre-registration when not empty.
# norm_flags <value>: whitespace-normalised, and refused unless it is plain compiler-flag text.
norm_flags() {
    local w
    read -ra w <<<"$1"
    case "${w[*]}" in
    *[!A-Za-z0-9=_\ .-]*) echo "build flags may hold only [A-Za-z0-9=_ .-]: '${w[*]}'" >&2; return 1 ;;
    esac
    printf '%s' "${w[*]}"
}
TICKLE_EXTRA_CFLAGS=$(norm_flags "$TICKLE_EXTRA_CFLAGS") || exit 2
export TICKLE_EXTRA_CFLAGS
# Gate v2 per-arm flags (A3): P perturbs placement and nothing else; B/C default to A's. Exported, so the
# pre-registration rebuilt under the lock sees what this invocation registered.
if [ "$GATE" = v2 ]; then
    P_EXTRA_CFLAGS=$(norm_flags "${P_EXTRA_CFLAGS-$TICKLE_PLACEMENT_CFLAGS}") || exit 2
    B_EXTRA_CFLAGS=$(norm_flags "${B_EXTRA_CFLAGS-$TICKLE_EXTRA_CFLAGS}") || exit 2
    C_EXTRA_CFLAGS=$(norm_flags "${C_EXTRA_CFLAGS-$TICKLE_EXTRA_CFLAGS}") || exit 2
    if [ "$P_EXTRA_CFLAGS" = "$TICKLE_EXTRA_CFLAGS" ]; then
        echo "REFUSED: P_EXTRA_CFLAGS equals A's flags ('$TICKLE_EXTRA_CFLAGS'), so P would not perturb placement" >&2
        exit 2
    fi
    # Counting is what v2 decides on; it cannot be switched off under it.
    if [ "${INSN_COUNT:-1}" != 1 ]; then echo "REFUSED: GATE=v2 decides on instruction counts: INSN_COUNT=1" >&2; exit 2; fi
    INSN_COUNT=1
    export P_EXTRA_CFLAGS B_EXTRA_CFLAGS C_EXTRA_CFLAGS
else
    for v in P_EXTRA_CFLAGS B_EXTRA_CFLAGS C_EXTRA_CFLAGS; do
        [ -n "${!v+set}" ] && { echo "REFUSED: $v is a gate v2 arm's flags; GATE=v1 builds every arm alike" >&2; exit 2; }
    done
    INSN_COUNT=${INSN_COUNT:-0}
fi
export INSN_COUNT

if [ -z "${OUTB:-}" ]; then
    # Fixed here, before the lock, so the run measures the commits named at launch even if a branch moves meanwhile.
    git -C "$REPO" fetch -q origin || { echo "REFUSED: git fetch origin failed" >&2; exit 2; }
    for v in A B C SENTINEL; do
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
# SENTINEL|scen|size|extra|slug|sha: the control cells, run at the sentinel's commit in every block (ab_samehost.py).
mapfile -t SENT_LINES < <(printf '%s\n' "${CELL_LINES[@]}" | sed -n 's/^SENTINEL|//p')
# ARM|<letter>|<sha>|<file token>|<flags>: each block's arm is built from the pre-registration, not the environment.
declare -A ARM_SHA ARM_TOK ARM_FLAGS
while IFS='|' read -r letter asha atok aflags; do
    ARM_SHA[$letter]=$asha ARM_TOK[$letter]=$atok ARM_FLAGS[$letter]=$aflags
done < <(printf '%s\n' "${CELL_LINES[@]}" | sed -n 's/^ARM|//p')
PRE_GATE=$(printf '%s\n' "${CELL_LINES[@]}" | sed -n 's/^GATE|//p')
mapfile -t CELL_LINES < <(printf '%s\n' "${CELL_LINES[@]}" | grep -vE '^(SENTINEL|ARM|GATE)\|')
[ -n "$BLOCKS" ] || { echo "REFUSED: no cell list from $OUTB.prereg.json" >&2; exit 2; }
[ "$PRE_GATE" = "$GATE" ] || { echo "REFUSED: $OUTB.prereg.json is gate '$PRE_GATE', this invocation GATE=$GATE" >&2; exit 2; }
for L in $BLOCKS; do
    [ -n "${ARM_SHA[$L]:-}" ] || { echo "REFUSED: block arm $L has no ARM line in $OUTB.prereg.json" >&2; exit 2; }
done
SHAS=$(for v in A B C; do [ -n "${!v:-}" ] && echo "${!v}"; done)

if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    ncell=$((${#CELL_LINES[@]} - 1))
    nblk=$(echo "$BLOCKS" | wc -w)
    # ~30 s of build and setup per s6 call, and each repetition runs the ON and the OFF arm for about DUR + 16 s each
    # (warm-up/cool-down, server stop, ssh) - calibrated on ab_frag_fastpath's latency cells, 2026-10-06 (4.5 min per
    # cell at REPS=5 DUR=10). Gate v2's perf stat around each process adds well under a second a repetition.
    nsent=${#SENT_LINES[@]}
    est=$((nblk * (ncell + nsent) * (30 + REPS * 2 * (DUR + 16)) / 60))
    echo "ESTIMATE: $nblk blocks x ($ncell cells + $nsent sentinel) x REPS=$REPS at DUR=$DUR s -> about $est min on the" \
        "rig, plus the lock wait"
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
        for sha in $(for s in $SHAS ${SENTINEL:-}; do echo "$s"; done | sort -u); do
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
for v in A B C SENTINEL; do [ -n "${!v:-}" ] && say "    $v=${!v}"; done
if [ "$GATE" = v2 ]; then
    say "    GATE=v2: blocks $BLOCKS, every TickLE process counted (INSN_COUNT=$INSN_COUNT); per-arm flags:"
    for L in $(tr ' ' '\n' <<<"$BLOCKS" | awk 'NF && !seen[$0]++'); do
        say "      arm $L ${ARM_SHA[$L]:0:8} TICKLE_EXTRA_CFLAGS='${ARM_FLAGS[$L]}'"
    done
    say "    sentinel (if any): A's flags '$TICKLE_EXTRA_CFLAGS'"
else
    say "    GATE=v1: INSN_COUNT=$INSN_COUNT"
fi
say "    TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS' (TickLE core and bench, every arm and the sentinel; empty: the plain build)"
# flags_reached <cell file> <cnt> <flags>: stop the run (exit 4, VOID) when a cell that measured anything does not show,
# for both of its arms, that its build was given <flags> - that arm's own (s6_witness_check.sh's verified line) - or when
# its build was refused for not showing them. A cell that failed for another reason is left to the summary (NO VERDICT).
flags_reached() {
    local f=$1 c=$2 want=$3 arm
    if grep -q 'FATAL build flags' "$f" 2>/dev/null; then
        say "VOID: $f - a build did not show TICKLE_EXTRA_CFLAGS='$want': $(grep -m1 'FATAL build flags' "$f")"
        say "    refusing to measure further arms that may not carry the treatment"
        exit 4
    fi
    [ "${c:-0}" = 0 ] && return 0
    for arm in ON OFF; do
        if ! grep -qF "  arm $arm TICKLE_EXTRA_CFLAGS='$want' reached core and bench (" "$f"; then
            say "VOID: $f measured, but its $arm arm's build does not echo TICKLE_EXTRA_CFLAGS='$want'"
            say "    an arm must report its treatment; refusing to read this run as the arms it was asked for"
            exit 4
        fi
    done
    say "    build flags: ON and OFF show TICKLE_EXTRA_CFLAGS='$want'; ON $(grep -m1 -F "  arm ON TICKLE_EXTRA_CFLAGS=" "$f" | sed 's/.*reached core and bench //')"
}
# insn_counted <cell file> <cnt>: under INSN_COUNT=1, says how many of the cell's RESULT lines carry insn=ok, and stops
# the run (exit 4, VOID) when a cell that measured anything has none at all - the instrument is broken, and every later
# cell would be read as "no counts". Single failures are left to the reader, which VOIDs those repetitions.
insn_counted() {
    local f=$1 c=$2 nres nok
    { [ "$INSN_COUNT" = 1 ] && [ "${c:-0}" != 0 ]; } || return 0
    nres=$(grep -cE '^ *arm=(ON|OFF) RESULT: ' "$f")
    nok=$(grep -E '^ *arm=(ON|OFF) RESULT: ' "$f" | grep -c ' insn=ok')
    say "    insn: $nok of $nres RESULT lines (client and server, ON and OFF) counted"
    if [ "$nok" = 0 ]; then
        say "VOID: $f measured, but no RESULT line carries instruction counts ($(grep -m1 -oE 'insn=fail:[a-z_]+' "$f" || echo 'no insn field'))"
        say "    gate v2 decides on them; refusing to measure further cells that the reader could not judge"
        exit 4
    fi
}
n=0
for L in $BLOCKS; do
    n=$((n + 1))
    sha=${ARM_SHA[$L]} tok=${ARM_TOK[$L]} fl=${ARM_FLAGS[$L]}
    for l in "${CELL_LINES[@]}"; do
        [[ $l == BLOCKS* ]] && continue
        IFS='|' read -r scen size extra slug <<<"$l"
        out="$OUTB.b${n}_${tok}_${slug}.txt"
        say "--- block $n arm $L ${sha:0:8} $scen $size '$extra' flags '$fl' ($(date +%T))"
        # A cell's extra args go to CLI_ARGS. The measured window does NOT: s6_transport_cells.sh appends its own
        # WINDOW_ARGS after CLI_ARGS, so a -W/-C given here is overridden (ab_frag_fastpath.sh, 2026-10-06). Set
        # WINDOW_ARGS in the environment for a whole run instead.
        FRAMEWORKS=tickle SCEN=$scen SIZE=$size DUR=$DUR REPS=$REPS SHA=$sha CLI_ARGS="$extra" OUT="$out" PREFLIGHT=0 \
            TICKLE_EXTRA_CFLAGS="$fl" INSN_COUNT="$INSN_COUNT" "$X/s6_transport_cells.sh" >"$out.log" 2>&1
        rc=$?
        cnt=$(grep -c '^ *arm=ON RESULT: .*framework=tickle' "$out" 2>/dev/null)
        say "    exit $rc, ${cnt:-no output file, so no} arm=ON RESULT lines (client and server)"
        # A cell that produced nothing says why in its own file; said here too, so the log does not read "exit 0".
        [ "${cnt:-0}" = 0 ] && say "    CELL FAILED: $(grep -m2 -E 'FAILED to run|FATAL|error:' "$out" "$out.log" 2>/dev/null | tr '\n' ' ')"
        flags_reached "$out" "$cnt" "$fl"
        insn_counted "$out" "$cnt"
    done
    for l in "${SENT_LINES[@]}"; do
        IFS='|' read -r scen size extra slug ssha <<<"$l"
        out="$OUTB.b${n}_${tok}_sentinel${ssha:0:8}_${slug}.txt"
        say "--- block $n arm $L sentinel ${ssha:0:8} $scen $size '$extra' flags '$TICKLE_EXTRA_CFLAGS' ($(date +%T))"
        # The sentinel is one instrument in every block: A's flags, and counted like the arms around it (its controls
        # are not read from the counts, but every process of a block carries the same instrument).
        FRAMEWORKS=tickle SCEN=$scen SIZE=$size DUR=$DUR REPS=$REPS SHA=$ssha CLI_ARGS="$extra" OUT="$out" PREFLIGHT=0 \
            TICKLE_EXTRA_CFLAGS="$TICKLE_EXTRA_CFLAGS" INSN_COUNT="$INSN_COUNT" "$X/s6_transport_cells.sh" >"$out.log" 2>&1
        rc=$?
        cnt=$(grep -c '^ *arm=ON RESULT: .*framework=tickle' "$out" 2>/dev/null)
        say "    exit $rc, ${cnt:-no output file, so no} arm=ON RESULT lines (client and server)"
        [ "${cnt:-0}" = 0 ] && say "    CELL FAILED: $(grep -m2 -E 'FAILED to run|FATAL|error:' "$out" "$out.log" 2>/dev/null | tr '\n' ' ')"
        flags_reached "$out" "$cnt" "$TICKLE_EXTRA_CFLAGS"
    done
done
python3 "$X/ab_samehost.py" summary "$OUTB.prereg.json" "$OUTB" >"$OUTB.summary.txt" 2>&1
rc=$?
grep -E '^(VERDICT|OVERALL)' "$OUTB.summary.txt" | tee -a "$OUTB.driver.log"
say "=== ALL_DONE $(date -Is) summary rc=$rc ($OUTB.summary.txt) ==="

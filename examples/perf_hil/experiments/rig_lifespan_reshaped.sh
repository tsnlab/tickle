#!/usr/bin/env bash
# RESULTS row 47 (LIFESPAN 100 ms expiry) re-measured on the rig with the reshaped TickLE lifespan_expiry harness
# (written 2026-10-20, before any of its runs).
#
# WHY. Until 2026-10-20 TickLE's lifespan_expiry server slept -p before creating a VOLATILE Subscriber, so its "lost"
# was what had been published before it joined, whatever the lifespan: on the PC the old harness lost 48 at -T 0.1 and
# 48 at -T 1000 (lifespan_pc_check.sh, OLD_CANNOT_FAIL). Row 47's TickLE "works" (S, 659013e9) came from that harness,
# and so did ROADMAP's "harness residuals" for scenario 9 (COMPARISON to-do 6: pauses differ per arm) and LIFESPAN
# (to-do 8: the pause=1.5 count bimodal): both were the join time, not LIFESPAN. The reshaped server matches, stalls
# consumption for -p, then takes and drops what has outlived -T, as the CycloneDDS/FastDDS twins do.
#
# HOW TO READ IT, written before running and enforced in read_cells() below. Arms, REPS each, framework order rotated
# per rep, -i 0.02 -n 250: every framework at -T 0.1 and -p 1.0 / 1.5 / 2.0; TickLE also at -T 1000 -p 1.0 (no
# lifespan to bite) and -T 0.1 -p 0.05 (within the lifespan). want(p) = round((p - 0.1) / 0.02) = 45 / 70 / 95.
#   VOID (whole session)  a CycloneDDS rep outside want +/- 5 (it read exactly 45/70/95 before: it is the control this
#                         change cannot touch), or any run without a server RESULT line.
#   TickLE PASS per pause every rep within want +/- 5, stalled=1, queue_overflow=0, lost == expired; else FAIL.
#   TickLE controls PASS  lost 0 in every rep at -T 1000 -p 1.0 and at -T 0.1 -p 0.05; else FAIL (a harness defect).
#   FastDDS               reported, not judged (39 / 64.7 / 88.7 before; nothing about it changed).
#   Row 47's TickLE cell reads "works" only if every TickLE verdict is PASS and the session is not VOID.
#
# REFUSALS (each prints why, then the DONE line): SHA unset, not 40 hex, not on origin/main after a fetch; this checkout
# not at SHA or with local changes under examples/perf_hil; under the lock, netem or tbf on either Pi's eth0 (before
# and after); under the lock, not enough time before NOT_AFTER (default 07:30) for 1.5x the estimate.
#
# THE RIG. The lock (hil) is taken ONCE, inside this detached process, around builds and every run. Nothing here
# shapes a link. Builds happen on the Pis (aarch64 only); nothing of theirs reaches an x86 build.
#
# ESTIMATE. Checkout + three framework builds on both Pis in parallel ~6 min; 3 pauses x 3 frameworks x REPS 5 + 2 x
# REPS TickLE controls = 55 runs at ~12 s = ~11 min; ~17 min in all. EST_MIN 20, 1.5x = 30 min.
#
# Usage (from a detached worktree at SHA, which must be on origin/main; results in ~/rig_results_safe):
#   git -C /home/semih/tickle fetch origin && SHA=$(git -C /home/semih/tickle rev-parse origin/main)
#   git -C /home/semih/tickle worktree add --detach /home/semih/tickle_runner_lifespan "$SHA"
#   cd /home/semih/tickle_runner_lifespan && SHA=$SHA setsid nohup examples/perf_hil/experiments/rig_lifespan_reshaped.sh \
#     > ~/rig_results_safe/lifespan_launch.log 2>&1 < /dev/null &
# The last line is always "=== LIFESPAN DONE rc=<n> ... ===". DRY_RUN=1 makes the refusals and prints the plan only.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PH/../.." && pwd)"
SAFE=$HOME/rig_results_safe
REPS=${REPS:-5}
EST_MIN=${EST_MIN:-20}
NOT_AFTER=${NOT_AFTER:-07:30}
DRY_RUN=${DRY_RUN:-0}
SHA=${SHA:-}
FWS=(tickle cyclonedds fastdds)
say() { echo "$(date -Is) $*"; }

if [ "${LIFESPAN_LOCKED:-0}" != 1 ]; then
    # The one EXIT trap of the outer instance: the DONE line, whatever happened.
    trap 'echo "=== LIFESPAN DONE rc=$? sha ${SHA:-unset} $(date -Is) ==="' EXIT
    case "$SHA" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;;
    *)
        say "REFUSED: set SHA to the full id of a commit on origin/main (got '${SHA}')"
        exit 2
        ;;
    esac
    [ ${#SHA} = 40 ] || {
        say "REFUSED: SHA must be the full 40-hex id, not '${SHA}'"
        exit 2
    }
    git -C "$REPO" fetch -q origin main || {
        say "REFUSED: git fetch origin main failed - could not check SHA"
        exit 1
    }
    git -C "$REPO" merge-base --is-ancestor "$SHA" origin/main 2>/dev/null || {
        say "REFUSED: $SHA is not on origin/main (the Pis fetch it)"
        exit 1
    }
    head=$(git -C "$REPO" rev-parse HEAD)
    [ "$head" = "$SHA" ] || {
        say "REFUSED: this checkout ($REPO) is at $head, not $SHA - run from a detached worktree at SHA"
        exit 1
    }
    git -C "$REPO" diff --quiet HEAD -- examples/perf_hil || {
        say "REFUSED: $REPO has local changes under examples/perf_hil - the harness would not be $SHA's"
        exit 1
    }
    grep -q 'stalled=%d' "$PH/tickle/lifespan_expiry/server.c" || {
        say "REFUSED: $SHA's TickLE lifespan_expiry server is not the reshaped one (no stalled= field)"
        exit 1
    }
    case "$REPS" in '' | *[!0-9]*)
        say "REFUSED: REPS='$REPS'"
        exit 2
        ;;
    esac
    say "=== row 47 LIFESPAN, reshaped harness: sha ${SHA:0:8}, $REPS reps an arm ==="
    say "estimate ~${EST_MIN} min (1.5x $((EST_MIN * 3 / 2)) min); must finish before $NOT_AFTER; uptime: $(uptime)"
    if [ "$DRY_RUN" = 1 ]; then
        say "plan: build ${FWS[*]} lifespan_expiry on both Pis; per rep, rotated: -T 0.1 -p 1.0/1.5/2.0 each framework;"
        say "      TickLE -T 1000 -p 1.0 and -T 0.1 -p 0.05"
        say "DRY_RUN=1: no lock, nothing run"
        exit 0
    fi
    say "--- taking the rig lock (hil), once (wait at most ${RIG_LOCK_WAIT:-3600} s) ---"
    LIFESPAN_LOCKED=1 SHA=$SHA REPS=$REPS RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-3600} \
        "$PH/rig_lock.sh" "$HERE/rig_lifespan_reshaped.sh" "$@"
    exit $?
fi

# ---- under the lock (a child of the outer instance; no trap of its own: it applies nothing to the Pis) -----------
# shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$HERE/pi_checkout.sh"
say "--- rig lock held: $(tr '\n' ' ' </tmp/tickle-hil.lock 2>/dev/null) ---"
K=$HOME/.ssh/tickle_ci_ed25519
HOSTS=(10.1.1.214 10.1.1.213)
ssh_h() {
    local h=$1
    shift
    ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@" </dev/null
}
unshaped() { # $1 when
    local h q
    for h in "${HOSTS[@]}"; do
        if ! q=$(ssh_h "$h" "tc qdisc show dev eth0" 2>&1); then
            say "REFUSED ($1): could not read the qdisc on $h (ssh failed): $q"
            return 1
        fi
        case "$q" in *netem* | *tbf*)
            say "REFUSED ($1): $h eth0 is shaped with the lock held: $(tr '\n' ' ' <<<"$q")"
            return 1
            ;;
        esac
        say "link $h eth0 unshaped ($1): $(tr '\n' ' ' <<<"$q" | cut -c1-100)"
    done
}
unshaped "before" || exit 1
now=$(date +%s)
deadline=$(date -d "$NOT_AFTER" +%s) || {
    say "REFUSED: NOT_AFTER='$NOT_AFTER' is not a time"
    exit 2
}
[ "$deadline" -gt "$now" ] || deadline=$((deadline + 86400))
if [ $((now + EST_MIN * 90)) -gt "$deadline" ]; then
    say "REFUSED: 1.5x the estimate ($((EST_MIN * 3 / 2)) min) from now passes $NOT_AFTER"
    exit 1
fi
mkdir -p "$SAFE"
OUT=$SAFE/lifespan_reshaped_${SHA:0:8}_$(date +%Y%m%d-%H%M%S).txt
: >"$OUT"
say "results -> $OUT"

say "--- checkout ${SHA:0:8} and build lifespan_expiry (${FWS[*]}) on both Pis ---"
pids=()
for h in "${HOSTS[@]}"; do
    ssh_h "$h" "set -e; cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log
cd examples/perf_hil && for fw in ${FWS[*]}; do rm -f \$fw/lifespan_expiry/client \$fw/lifespan_expiry/server
(cd \$fw && ./build.sh lifespan_expiry >/tmp/lsreshaped_build_\$fw.log 2>&1) || { echo BUILD FAILED \$fw; tail -5 /tmp/lsreshaped_build_\$fw.log; exit 1; }
[ -x \$fw/lifespan_expiry/client ] && [ -x \$fw/lifespan_expiry/server ] || { echo MISSING \$fw; exit 1; }; done
echo built \$(git rev-parse --short HEAD) on \$(hostname)" &
    pids+=($!)
done
bad=0
for p in "${pids[@]}"; do wait "$p" || bad=1; done
[ "$bad" = 0 ] || {
    say "REFUSED: checkout or build failed on a Pi"
    exit 1
}

# One run: the server's RESULT fields after "scenario=lifespan_expiry", or nothing.
run_cell() { # $1 fw $2 lifespan $3 pause
    (cd "$PH/$1" && PRE_CLIENT_SLEEP=0 timeout 120 ./run_scenario.sh lifespan_expiry -i 0.02 -T "$2" -n 250 -p "$3" \
        2>/dev/null | grep -a '^RESULT:' | grep -a 'role=server' | tail -1 |
        sed -E 's/RESULT: framework=[a-z]+ scenario=[a-z_]+ //') || true
}
for rep in $(seq 1 "$REPS"); do
    # rotate the framework order per rep
    off=$(((rep - 1) % ${#FWS[@]}))
    order=("${FWS[@]:off}" "${FWS[@]:0:off}")
    for pause in 1.0 1.5 2.0; do
        for fw in "${order[@]}"; do
            res=$(run_cell "$fw" 0.1 "$pause")
            echo "cell $fw T0.1 p$pause rep$rep | ${res:-NO RESULT LINE}" | tee -a "$OUT"
        done
    done
    for spec in "1000 1.0" "0.1 0.05"; do
        read -r life pause <<<"$spec"
        res=$(run_cell tickle "$life" "$pause")
        echo "cell tickle T$life p$pause rep$rep | ${res:-NO RESULT LINE}" | tee -a "$OUT"
    done
done
rc=0
unshaped "after" || rc=9

# ---- the reading, as pre-registered above ----
field() { grep -o " $1=[0-9]*" <<<" $2" | head -1 | grep -o '[0-9]*$'; }
void=0
declare -A verdict
while IFS= read -r line; do
    key=${line%% rep*}
    key=${key#cell }
    res=${line#*| }
    if [ "$res" = "NO RESULT LINE" ]; then
        say "VOID run: $line"
        void=1
        continue
    fi
    fw=${key%% *}
    rest=${key#* }
    life=${rest%% *}
    life=${life#T}
    pause=${rest##* p}
    lost=$(field lost "$res")
    case "$fw $life" in
    "cyclonedds 0.1")
        want=$(awk -v p="$pause" 'BEGIN { printf "%d", (p - 0.1) / 0.02 + 0.5 }')
        if [ $((lost - want)) -gt 5 ] || [ $((want - lost)) -gt 5 ]; then
            say "VOID: control CycloneDDS lost $lost, want $want +/- 5: $line"
            void=1
        fi
        ;;
    "tickle 0.1" | "tickle 1000")
        st=$(field stalled "$res")
        ov=$(field queue_overflow "$res")
        ex=$(field expired "$res")
        ok=1
        [ "$st" = 1 ] && [ "$ov" = 0 ] || ok=0
        if [ "$life" = 1000 ] || [ "$pause" = 0.05 ]; then
            [ "$lost" = 0 ] || ok=0
        else
            want=$(awk -v p="$pause" 'BEGIN { printf "%d", (p - 0.1) / 0.02 + 0.5 }')
            if [ $((lost - want)) -gt 5 ] || [ $((want - lost)) -gt 5 ] || [ "$lost" != "$ex" ]; then ok=0; fi
        fi
        [ "$ok" = 1 ] || {
            verdict["$key"]=FAIL
            say "FAIL: $line"
        }
        [ -n "${verdict[$key]:-}" ] || verdict["$key"]=PASS
        ;;
    esac
done < <(grep '^cell ' "$OUT")
for k in "${!verdict[@]}"; do echo "VERDICT $k: ${verdict[$k]}" | tee -a "$OUT"; done
if [ "$void" = 1 ]; then
    echo "SESSION: VOID" | tee -a "$OUT"
    exit 3
fi
if printf '%s\n' "${verdict[@]}" | grep -q FAIL; then
    echo "SESSION: TickLE FAILS - row 47's TickLE cell does not read works" | tee -a "$OUT"
    exit 4
fi
echo "SESSION: PASS - row 47's TickLE cell reads works (reshaped harness, ${SHA:0:8})" | tee -a "$OUT"
exit "$rc"

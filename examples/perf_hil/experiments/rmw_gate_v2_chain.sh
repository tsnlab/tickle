#!/usr/bin/env bash
# rmw_gate_v2_chain.sh - one rmw_tickle commit against another on the rig, by gate v2 (the user's decision of 2026-10-11;
# gate_v2.py's docstring is the rule, rmw_gate_v2.py's the rmw reading of it). The rmw counterpart of
# campaign_ab_chain.sh GATE=v2; rmw_lease86_rig_chain.sh was the A B B A template.
#
# ARMS AND BLOCKS. Three arms in mirrored blocks A P B B P A, each block one invocation of the harness at one SHA and one
# flag set, both Pis rebuilt per block:
#   A  the parent, built with TICKLE_EXTRA_CFLAGS (default TICKLE_LAYOUT_CFLAGS, -falign-functions=64 + NOPAD: layout-stable);
#   P  A' - A's commit built with P_EXTRA_CFLAGS (default TICKLE_PLACEMENT_CFLAGS, -falign-functions=128 + NOPAD): every function
#      moved by a multiple of 64 bytes and nothing else, so |P - A| is each metric's placement floor;
#   B  the change, built with B_EXTRA_CFLAGS (default A's flags; only the gate's own validation sets it otherwise).
# HARNESS=rtt      rmw_crosshost_rtt.sh with RMW_LIST=rmw_tickle and INSN_COUNT=1 (ping/pong round trips; MSGS, WAITS,
#                  POLL_SLEEPS, IDLE_S, ... pass through from the environment as the harness reads them)
# HARNESS=keepall  rmw_keepall_rig.sh with ARMS=tickle@head and INSN_COUNT=1 (perf_test RELIABLE KEEP_ALL at max rate;
#                  CELLS / TOPICS / LOSSES / DUR / EQUAL_BOUND pass through)
#
# A BLOCK COUNTS ONLY IF its own output shows its treatment, checked here after it ends; otherwise the chain stops VOID:
#   - the harness echoed TICKLE_EXTRA_CFLAGS='<this arm's flags>' (the value the harness used, not the one sent);
#   - it printed one "build flags verified:" line carrying those flags for every build on its "--- builds to verify:
#     ... ---" line on each of the two Pis (rmw_build_flags_check.py read each build's compile_commands.json);
#   - it printed INSN_COUNT=1 (every process counted).
# THE READING (pre-registered, implemented in rmw_gate_v2.py and gate_v2.py, checked by rmw_gate_v2.py --selftest):
#   PRIMARY  executed instructions per round trip (rtt: ping and pong, user and kernel) or per sample (keepall:
#            publisher and subscriber) - PRIMARY= narrows it;
#   WITNESS  rtt: rtt_avg_ms, pong.cpu_ns; keepall: rate, pub/sub cpu_us_per_sample - WITNESS= replaces it;
#   one Bonferroni Student t threshold over all deciding comparisons; WORSE only beyond it AND beyond |P - A|;
#   NO VERDICT without counts, without P, or with a pre-registered item that selects nothing; keepall: a correctness
#   failure (loss, publisher error) in B is WORSE.
# THE RIG LOCK (hil) is taken once, here, inside the detached process, for the whole chain: per-block locking let a
# queued campaign cut in between blocks (2026-10-05). Each harness then sees RIG_LOCK_HELD_HIL=1.
#
# USAGE (detached; A and B must be reachable from the Pis' origin, i.e. pushed - an ab/<name> branch included):
#   TAG=<name>; setsid nohup env HARNESS=rtt A=<parent sha> B=<change sha> TAG=$TAG \
#     examples/perf_hil/experiments/rmw_gate_v2_chain.sh > $HOME/rig_results_safe/rmw_gate_v2_$TAG.launch.log 2>&1 < /dev/null &
# Outputs in OUTDIR (default $HOME/rig_results_safe/rmw_gate_v2_<TAG>): <TAG>_b<n>_<arm>_<sha8>.{txt,log} (keepall:
# also .runs/), <TAG>_compare.txt (the reader's table and "GATE: ..." line), and <TAG>.done, written however it ends.
# DURATION per block (estimates; the rig's rmw builds have not been timed under a flag change): a flag or SHA change
#   rebuilds every stage from its sources. rtt: rmw_tickle ~1 min, builtin_interfaces + rcl_interfaces ~3-5 min,
#   pingpong ~1-2 min, then rows at ~25 s each (default MSGS 2 x 2 QoS x WAITS x REPS 3 = 12 rows, ~5 min): ~12 min a
#   block, ~75 min a chain. keepall: performance_test rebuilt from scratch ~10-15 min, then 4 cells x REPS 3 x ~32 s
#   (~7 min): ~20 min a block, ~2 h a chain. Schedule the 1.5x wake-up at launch (CLAUDE.md, Long-running work).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$HERE/gate_v2_lib.sh"
export RIG_LOCK_SCOPE=hil
export RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi

HARNESS=${HARNESS:?HARNESS=rtt or keepall}
A=${A:?A=<sha> the parent}; B=${B:?B=<sha> the change}; TAG=${TAG:?TAG=<name>}
REPS=${REPS:-3}
case "$HARNESS" in rtt | keepall) ;; *) echo "HARNESS must be rtt or keepall" >&2; exit 2 ;; esac
case "$TAG" in *[!A-Za-z0-9_.-]*) echo "TAG may hold only [A-Za-z0-9_.-]" >&2; exit 2 ;; esac
layout_cflags_default
P_EXTRA_CFLAGS=${P_EXTRA_CFLAGS-$TICKLE_PLACEMENT_CFLAGS}
B_EXTRA_CFLAGS=${B_EXTRA_CFLAGS-$TICKLE_EXTRA_CFLAGS}
flags_of() { case "$1" in A) echo "$TICKLE_EXTRA_CFLAGS" ;; P) echo "$P_EXTRA_CFLAGS" ;; B) echo "$B_EXTRA_CFLAGS" ;; esac; }
sha_of() { case "$1" in A | P) echo "$A" ;; B) echo "$B" ;; esac; }
for arm in A P B; do
    case "$(flags_of $arm)" in *[!A-Za-z0-9=_\ -]*) echo "arm $arm flags may hold only [A-Za-z0-9=_ -]" >&2; exit 2 ;; esac
done
if [ "$P_EXTRA_CFLAGS" = "$TICKLE_EXTRA_CFLAGS" ]; then
    echo "REFUSED: P_EXTRA_CFLAGS equals A's flags ('$TICKLE_EXTRA_CFLAGS'), so P would not perturb placement" >&2
    exit 2
fi
OUTDIR=${OUTDIR:-$HOME/rig_results_safe/rmw_gate_v2_$TAG}
case "$OUTDIR" in *[[:space:]]*) echo "OUTDIR may not contain white space" >&2; exit 2 ;; esac
mkdir -p "$OUTDIR" || exit 2
DONE="$OUTDIR/$TAG.done" CMP="$OUTDIR/${TAG}_compare.txt"
rm -f "$DONE"
# One EXIT trap: a chain that ends any other way than through its verdict still leaves a .done saying so.
# shellcheck disable=SC2329 # invoked by the EXIT trap
on_exit() { [ -f "$DONE" ] || echo "=== chain ended $(date -Is) without a verdict (see $OUTDIR) ===" >"$DONE"; }
trap on_exit EXIT
void() { echo "=== chain VOID $(date -Is): $* ===" | tee "$DONE" "$CMP"; exit 1; }

ARMS="A P B B P A"
echo "=== rmw_gate_v2_chain $(date -Is) HARNESS=$HARNESS TAG=$TAG A=$A B=$B REPS=$REPS blocks: $ARMS" \
    "flags A='$TICKLE_EXTRA_CFLAGS' P='$P_EXTRA_CFLAGS' B='$B_EXTRA_CFLAGS' harness=$(git -C "$REPO" rev-parse --short HEAD) ==="
declare -A FILES=([A]="" [P]="" [B]="")
n=0
for arm in $ARMS; do
    n=$((n + 1))
    sha=$(sha_of "$arm")
    fl=$(flags_of "$arm")
    stem="$OUTDIR/${TAG}_b${n}_${arm}_${sha:0:8}"
    echo "--- block $n arm $arm ${sha:0:8} TICKLE_EXTRA_CFLAGS='$fl' $(date -Is)"
    if [ "$HARNESS" = rtt ]; then
        env TICKLE_EXTRA_CFLAGS="$fl" SHA="$sha" RMW_LIST=rmw_tickle TICKLE_VARIANTS= INSN_COUNT=1 REPS="$REPS" \
            OUT="$stem.txt" "$HERE/rmw_crosshost_rtt.sh" >"$stem.log" 2>&1
        rc=$?
        data="$stem.txt"
    else
        env TICKLE_EXTRA_CFLAGS="$fl" HEAD_SHA="$sha" ARMS=tickle@head INSN_COUNT=1 REPS="$REPS" \
            OUT="$stem" "$HERE/rmw_keepall_rig.sh" >"$stem.log" 2>&1
        rc=$?
        data="$stem.runs"
    fi
    rec="$stem.txt" # both harnesses write their record lines there
    echo "    block $n ended $(date -Is), exit $rc"
    awk -v s="TICKLE_EXTRA_CFLAGS='$fl' (every " 'index($0, s) == 1 { f = 1 } END { exit !f }' "$rec" 2>/dev/null ||
        void "block $n ($arm) did not echo TICKLE_EXTRA_CFLAGS='$fl' (see $stem.log)"
    nb=$(sed -n 's/^--- builds to verify: \(.*\) ---$/\1/p' "$rec" | head -1 | wc -w)
    nver=$(awk -v s="TICKLE_EXTRA_CFLAGS='$fl' (" 'index($0, "build flags verified: ") == 1 && index($0, s) { c++ } END { print c + 0 }' "$rec")
    if [ "$nb" -eq 0 ] || [ "$nver" -ne $((2 * nb)) ]; then
        void "block $n ($arm) has $nver 'build flags verified' lines for $nb builds on 2 Pis (see $stem.log)"
    fi
    grep -q '^INSN_COUNT=1 (' "$rec" || void "block $n ($arm) did not count instructions (no INSN_COUNT=1 line)"
    [ -e "$data" ] || void "block $n ($arm) left no $data"
    echo "    block $n verified: $nb builds x 2 Pis carry '$fl', INSN_COUNT=1"
    FILES[$arm]="${FILES[$arm]} $data"
done
# shellcheck disable=SC2086 # the file lists are word lists on purpose (OUTDIR has no white space, checked above)
python3 "$HERE/rmw_gate_v2.py" "$HARNESS" --a ${FILES[A]} --p ${FILES[P]} --b ${FILES[B]} \
    ${PRIMARY:+--primary "$PRIMARY"} ${WITNESS:+--witness "$WITNESS"} >"$CMP" 2>&1
echo "=== chain done $(date -Is): $(tail -1 "$CMP") ===" | tee "$DONE"

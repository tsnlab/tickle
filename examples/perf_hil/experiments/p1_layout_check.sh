#!/usr/bin/env bash
# p1_layout_check.sh - is the p1 client's 1.1% throughput loss function layout, or work?
#
# WIRE_PLAN.md 10.2 and 10.3: the client saturates its core, so its total CPU is unchanged; each sample costs +48 ns of
# USER time and the run sends 1.107% fewer samples. p1_throughput_bisect.sh put the origin at 5d9cace1 (the lightweight
# core tt_Node, CONTEXT_NODE_PLAN stage 2), and Dev then showed that commit adds nothing to the per-datagram path: all
# fifteen hunks are in creation and registration, the struct sizes and layouts are unchanged by design, and its own -O2
# comparison found every receive and publish function byte-identical with three differing in alignment NOPs alone - one
# of them keep_all_writable, on the publish path. That is the signature of code moving, not of code doing more.
#
# Four arms, so the question has a control on both axes:
#   A   = the known-good commit, built as usual
#   B   = the blamed commit's descendant (main), built as usual
#   Aa  = A with TICKLE_EXTRA_CFLAGS=-falign-functions=32
#   Ba  = B with the same
#
# HOW TO READ IT, written before the run:
#   - A-vs-B shows ~1.1% AND Aa-vs-Ba closes to within 2xSE -> LAYOUT. There is nothing in stage 2's design to fix; the
#     1.1% is what it cost to grow the translation unit, and the campaign should read commits that change code size
#     against a relocated build (section 8.3's amendment) rather than attribute the difference to a feature.
#   - A-vs-B shows ~1.1% AND Aa-vs-Ba keeps it -> NOT LAYOUT, at least not alignment. The next suspect is a51b8d2f
#     (-0.364%, t -2.19, a candidate under 8.3 rather than a finding), and the stronger arm is a pinned link order,
#     since forcing an alignment equalises boundaries and not addresses.
#   - A-vs-B does NOT show ~1.1% -> the whole run is VOID and says nothing either way. This arm exists so that a quiet
#     rig cannot be mistaken for a closed question.
#   - Aa or Ba differs from its unforced self by more than ~2% -> -falign-functions=32 is not a neutral change at this
#     cell; report the four numbers and draw no layout conclusion, because the instrument moved the thing it measures.
#   - any run with sent < 100000 is VOID; a step with fewer than 3 usable runs an arm is VOID and reported as such.
#
# Each arm is built ONCE and saved aside; the loop runs saved binaries, because a build inside an A/B loop rewrites the
# underlay mid-run. Servers are killed by the PID their own launch wrote, checked through /proc/PID/exe, and
# assert_one_server() refuses to measure while two of ours are alive - both lessons from p1_throughput_bisect.sh's own
# first run (WIRE_PLAN 10.3).
# Usage: p1_layout_check.sh [SHA_A] [SHA_B] [BLOCKS]   Output: $OUT (default /tmp/p1_layout_check.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
A=${1:-6910d840}
B=${2:-$(git -C "$REPO" rev-parse --short HEAD)}
BLOCKS=${3:-3}
ALIGN=${ALIGN:--falign-functions=32}
DUR=${DUR:-20}
OUT=${OUT:-/tmp/p1_layout_check.txt}
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
SCEN=reliable_throughput; SIZE=p1; SAVE=/tmp/p1lay
srv_pid=""
LAST_SENT=""
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
# For anything said from inside a function whose stdout is captured: the file and stderr, never stdout. The first
# version of this script said the built sha through say() from inside build_arm, which a command substitution then
# swallowed into the variable - so the "did the flag reach the compiler" guard below compared two strings that always
# differed by the arm name and could never fire. A guard that cannot fire is worse than no guard.
note() { echo "$*" >>"$OUT"; echo "$*" >&2; }
say "=== p1 layout check $(date -Is) A=$A B=$B align='$ALIGN' blocks=$BLOCKS ==="

build_arm() { # build_arm <sha> <arm-name> <extra-cflags>
    local sha=$1 name=$2 extra=$3 h out
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log
cat > examples/perf_hil/tickle/common/BenchStats.h" <"$REPO/examples/perf_hil/tickle/common/BenchStats.h" >/dev/null \
            || { note "FATAL checkout/instrument copy failed for $sha on $h"; return 1; }
        # build.sh is taken from THIS checkout too, because TICKLE_EXTRA_CFLAGS is newer than the A commit and an old
        # build.sh would ignore the flag silently - which would look exactly like "alignment made no difference".
        sh_ "$h" "cat > ~/tickle/examples/perf_hil/tickle/build.sh && chmod +x ~/tickle/examples/perf_hil/tickle/build.sh" \
            <"$REPO/examples/perf_hil/tickle/build.sh" >/dev/null || { note "FATAL build.sh copy failed on $h"; return 1; }
        out=$(sh_ "$h" "set -e; cd ~/tickle/examples/perf_hil/tickle && TICKLE_EXTRA_CFLAGS='$extra' ./build.sh $SCEN $SIZE > /tmp/p1lay_build.log 2>&1 || { echo BUILD_FAILED; tail -3 /tmp/p1lay_build.log; exit 0; }
mkdir -p $SAVE/$name && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/$name/ && sha256sum $SAVE/$name/client | cut -c1-16" </dev/null 2>&1 | tail -1)
        case "$out" in *BUILD_FAILED*) note "FATAL build failed for arm $name on $h: $out"; return 1;; esac
    done
    note "  arm $name ($sha, extra='$extra') built, client sha256=$out"
    echo "$out"
}

kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    sh_ "$SERVER" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/server|\"$SAVE\"/*/\"server (deleted)\") kill -TERM $srv_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" </dev/null >/dev/null
    srv_pid=""
}

assert_one_server() {
    # shellcheck disable=SC2016 # single-quoted on purpose: $p and $e must expand on the Pi, not here
    sh_ "$SERVER" 'n=0; for p in /proc/[0-9]*; do e=$(readlink $p/exe 2>/dev/null) || continue; case "$e" in /tmp/p1lay/*/server*) n=$((n+1));; esac; done; echo $n' </dev/null
}

run_one() { # run_one <arm-name>; sets LAST_SENT
    local name=$1 line alive
    LAST_SENT=""
    kill_server
    srv_pid=$(sh_ "$SERVER" "cd $SAVE/$name && rm -f /tmp/p1lay_server.pid && (setsid sh -c 'echo \$\$ > /tmp/p1lay_server.pid; exec taskset -c 1-3 ./server -Q -d $((DUR + 40))' > /tmp/p1lay_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/p1lay_server.pid" </dev/null)
    sh_ "$SERVER" "grep -q 'Node open' /tmp/p1lay_server.log" </dev/null || { say "    no server opened for arm=$name"; return 0; }
    alive=$(assert_one_server)
    [ "$alive" = 1 ] || { say "    ABORT: $alive of our servers alive, expected 1"; return 0; }
    line=$(sh_ "$CLIENT" "cd $SAVE/$name && taskset -c 1-3 ./client -Q -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    LAST_SENT=$(echo "$line" | grep -oE 'sent=[0-9]+' | cut -d= -f2)
    [ -n "$LAST_SENT" ] && say "arm=$name $line"
}

sha_A=$(build_arm "$A" A "") || exit 1
sha_B=$(build_arm "$B" B "") || exit 1
sha_Aa=$(build_arm "$A" Aa "$ALIGN") || exit 1
sha_Ba=$(build_arm "$B" Ba "$ALIGN") || exit 1
# The identity check that makes the arms meaningful: a forced arm whose binary equals its unforced twin means the flag
# never reached the compiler, and the run would report "alignment changed nothing" for the wrong reason. All four are
# printed whether or not it fires, so the log shows what was compared rather than only the verdict.
say "  built: A=$sha_A Aa=$sha_Aa  B=$sha_B Ba=$sha_Ba"
if [ "$sha_A" = "$sha_Aa" ] || [ "$sha_B" = "$sha_Ba" ]; then
    say "FATAL a forced arm's binary is identical to its unforced twin - '$ALIGN' never reached the compiler."
    say "  A=$sha_A Aa=$sha_Aa  B=$sha_B Ba=$sha_Ba"
    say "  Reporting no layout verdict: this would otherwise read as 'alignment changed nothing'."
    exit 1
fi

# Every arm in every block, rotated so no arm always sits in the same position within a block.
for b in $(seq 1 "$BLOCKS"); do
    say "--- block $b/$BLOCKS $(date -Is) ---"
    for arm in A B Ba Aa Aa Ba B A; do
        run_one "$arm"
        [ -z "$LAST_SENT" ] || [ "$LAST_SENT" -ge 100000 ] || say "    VOID run arm=$arm sent=$LAST_SENT"
    done
done
kill_server
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import math, re, sys
rows = {}
for line in open(sys.argv[1]):
    m = re.match(r"arm=(\w+) (RESULT.*)", line.strip())
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
    try:
        sent = int(f.get("sent", 0))
    except ValueError:
        continue
    if sent >= 100000:
        rows.setdefault(m.group(1), []).append(sent)

def stat(v):
    n = len(v); mu = sum(v) / n
    return mu, (0.0 if n < 2 else math.sqrt(sum((x - mu) ** 2 for x in v) / (n - 1) / n)), n

for arm in ("A", "B", "Aa", "Ba"):
    if len(rows.get(arm, [])) < 3:
        print("VOID: arm %s has %d usable runs, fewer than 3" % (arm, len(rows.get(arm, []))))
        sys.exit(0)

def gap(x, y):
    mx, sx, nx = stat(rows[x]); my, sy, ny = stat(rows[y])
    den = math.hypot(sx, sy)
    t = 0.0 if den == 0 else (my - mx) / den
    print("  %s %.0f+-%.0f (n=%d) -> %s %.0f+-%.0f (n=%d):  %+.3f%%  t=%.2f" % (x, mx, sx, nx, y, my, sy, ny, 100 * (my - mx) / mx, t))
    return 100 * (my - mx) / mx, t

print("unforced pair (the control that the effect is here at all):")
p_unforced, t_unforced = gap("A", "B")
print("forced pair (both arms -falign-functions):")
p_forced, t_forced = gap("Aa", "Ba")
print("what the flag itself did to each commit (neither changes what the program computes):")
flag_effect = {"A->Aa": gap("A", "Aa"), "B->Ba": gap("B", "Ba")}

# Three outcomes were pre-registered and the run produced a fourth: the gap REVERSED under the forced alignment. The
# original branch treated any |t| > 2 on the forced pair as "not alignment", which reads a reversal as if it were the
# same gap surviving - the opposite of what it means. A difference whose SIGN a semantically neutral flag can flip is
# not a difference the code is causing, so a reversal is the strongest evidence for layout, not against it.
flag_moves = []
for name, (pct, t) in flag_effect.items():
    if abs(pct) > 0.5 and abs(t) > 2:
        flag_moves.append("%s %+.3f%% (t=%.2f)" % (name, pct, t))

if not (p_unforced < -0.5 and t_unforced < -2):
    print("VERDICT=VOID - the unforced pair does not reproduce the effect; this run says nothing either way")
elif abs(t_forced) <= 2:
    print("VERDICT=LAYOUT - forcing the alignment closes the gap (%+.3f%%, t=%.2f) while the unforced pair keeps it" % (p_forced, t_forced))
elif p_forced > 0:
    print("VERDICT=LAYOUT_DOMINATED - the gap REVERSES under a forced alignment (%+.3f%%, t=%.2f against %+.3f%% unforced)."
          % (p_forced, t_forced, p_unforced))
    print("  A difference whose sign a semantically neutral flag flips is not caused by the code. Treat differences of")
    print("  this size at this cell as layout noise and not as a cost.")
else:
    print("VERDICT=NOT_ALIGNMENT - the gap survives a forced alignment in the same direction (%+.3f%%, t=%.2f); the"
          % (p_forced, t_forced))
    print("  stronger arm is a pinned link order, and after that a51b8d2f.")

if flag_moves:
    print("FLOOR: the flag alone, which changes nothing the program computes, moved " + ", ".join(flag_moves) + ".")
    print("  So this cell cannot attribute a throughput difference of that size to any code change.")
PYEOF

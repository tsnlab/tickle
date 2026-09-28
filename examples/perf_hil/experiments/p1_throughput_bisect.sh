#!/usr/bin/env bash
# p1_throughput_bisect.sh - finds the commit where the p1 RELIABLE client's throughput fell, by bisecting the commits
# that touch core between a known-good build and HEAD.
#
# Why throughput and not CPU. p1_cpu_attribute.sh (2026-09-28, /tmp/p1_cpu_attribute.txt) settled what WIRE_PLAN
# section 10 had been carrying as a CPU regression: the client saturates its core in both arms - 19.992 s of CPU in a
# 20.000 s run, identical to within t=1.3 - so total CPU cannot rise. What changed is that each sample costs about 48 ns
# more in USER time (860.3 -> 908.7 ns/sample) while kernel time per sample barely moved (+10.8 ns), so fewer samples
# fit in the same 20 s: sent fell 1.107%. "CPU per sample" rose only because its denominator shrank.
#
# That makes `sent` the instrument: it moved with t = -10.8 at n=8, where cpu_s per sample is the same information
# divided by a constant, and it needs no per-thread breakdown (the client is one thread).
#
# HOW TO READ IT, written before the run:
#   - the bisect converges on one commit -> that commit's diff to core is where the ~48 ns per sample went, and it is
#     the answer WIRE_PLAN section 10 has been missing.
#   - two or more steps regress partially and none accounts for the whole 1.1% -> the cost is spread over several
#     commits. The output keeps every step's number so that is visible rather than hidden by the bisect's verdict.
#   - a candidate measures BETTER than the baseline -> either the range is not monotone or the rig has drifted; the
#     baseline is re-measured in every step precisely so drift cannot be mistaken for a finding.
#   - a candidate that does not build is SKIPped, named, and the bisect moves to its neighbour.
#   - any run with sent < 100000 is VOID and the step is re-run once; twice VOID and the step is reported as VOID.
#
# A harness defect this file already cost, kept so it is not repeated: run_one() used to echo the sample count, so every
# call ran in a command substitution - a SUBSHELL - and its assignment to srv_pid never reached the parent. kill_server()
# then always saw an empty PID, every arm's server survived into the next arm, and several servers ended up bound to
# 8282 at once, writing over each other in one log. The clients stalled at sent=256 and the first step voided. It is the
# same class as the three defects in WIRE_PLAN 10.1: state set where it cannot be seen. The count now comes back in a
# global, and assert_one_server() checks the rig for exactly one server of ours by /proc/PID/exe before each run, so an
# accumulation says so at once instead of arriving as a void.
# Usage: p1_throughput_bisect.sh [GOOD_SHA] [BAD_SHA]   Output: $OUT (default /tmp/p1_throughput_bisect.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
GOOD=${1:-6910d840}
BAD=${2:-$(git -C "$REPO" rev-parse --short HEAD)}
REPS=${REPS:-2}   # ABBA blocks per step: 2 blocks = 4 runs an arm
DUR=${DUR:-20}
OUT=${OUT:-/tmp/p1_throughput_bisect.txt}
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
SCEN=reliable_throughput; SIZE=p1; SAVE=/tmp/p1bis
srv_pid=""
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }

mapfile -t CANDS < <(git -C "$REPO" rev-list --reverse "$GOOD..$BAD" -- src include)
say "=== p1 throughput bisect $(date -Is) good=$GOOD bad=$BAD candidates=${#CANDS[@]} reps=$REPS ==="

build_into() { # build_into <sha> <dir-name>; echoes the client's sha256 or BUILD_FAILED
    local sha=$1 name=$2 h out
    for h in "$CLIENT" "$SERVER"; do
        out=$(sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log
cat > examples/perf_hil/tickle/common/BenchStats.h" <"$REPO/examples/perf_hil/tickle/common/BenchStats.h" 2>&1)
        out=$(sh_ "$h" "set -e; cd ~/tickle/examples/perf_hil/tickle && ./build.sh $SCEN $SIZE > /tmp/p1bis_build.log 2>&1 || { echo BUILD_FAILED; exit 0; }
mkdir -p $SAVE/$name && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/$name/ && sha256sum $SAVE/$name/client | cut -c1-16" </dev/null 2>&1 | tail -1)
        case "$out" in *BUILD_FAILED*) echo BUILD_FAILED; return 0;; esac
    done
    echo "$out"
}

kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    sh_ "$SERVER" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/server) kill -TERM $srv_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" </dev/null >/dev/null
    srv_pid=""
}

LAST_SENT=""
assert_one_server() { # how many of our own server binaries are alive on the server host, by exe, not by pattern
    # shellcheck disable=SC2016 # single-quoted on purpose: $p and $e must expand on the Pi, not here
    sh_ "$SERVER" 'n=0; for p in /proc/[0-9]*; do e=$(readlink $p/exe 2>/dev/null) || continue; case "$e" in /tmp/p1bis/*/server) n=$((n+1));; esac; done; echo $n' </dev/null
}

run_one() { # run_one <dir-name>; sets LAST_SENT (empty when the run produced nothing)
    local name=$1 line alive
    LAST_SENT=""
    kill_server
    srv_pid=$(sh_ "$SERVER" "cd $SAVE/$name && rm -f /tmp/p1bis_server.pid && (setsid sh -c 'echo \$\$ > /tmp/p1bis_server.pid; exec taskset -c 1-3 ./server -Q -d $((DUR + 40))' > /tmp/p1bis_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/p1bis_server.pid" </dev/null)
    sh_ "$SERVER" "grep -q 'Node open' /tmp/p1bis_server.log" </dev/null || { say "    no server opened for arm=$name"; return 0; }
    alive=$(assert_one_server)
    if [ "$alive" != 1 ]; then
        say "    ABORT: $alive of our servers alive on $SERVER, expected 1 - a previous arm's server survived"
        return 0
    fi
    line=$(sh_ "$CLIENT" "cd $SAVE/$name && taskset -c 1-3 ./client -Q -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    LAST_SENT=$(echo "$line" | grep -oE 'sent=[0-9]+' | cut -d= -f2)
}

# Returns 0 (regressed) / 1 (not) / 2 (void), and says the numbers either way. The baseline is re-measured inside every
# step, interleaved ABBA with the candidate, so rig drift lands on both arms rather than on the verdict.
compare_step() {
    local name=$1 a_vals=() c_vals=() arm
    for _ in $(seq 1 "$REPS"); do
        for arm in base "$name" "$name" base; do
            run_one "$arm"
            if [ -z "$LAST_SENT" ] || [ "$LAST_SENT" -lt 100000 ]; then say "    VOID run arm=$arm sent=${LAST_SENT:-none}"; continue; fi
            if [ "$arm" = base ]; then a_vals+=("$LAST_SENT"); else c_vals+=("$LAST_SENT"); fi
        done
    done
    if [ "${#a_vals[@]}" -lt 2 ] || [ "${#c_vals[@]}" -lt 2 ]; then say "    step VOID (usable runs: base ${#a_vals[@]}, cand ${#c_vals[@]})"; return 2; fi
    python3 - "${a_vals[*]}" "${c_vals[*]}" <<'PYEOF'
import math, sys
def st(v):
    n=len(v); mu=sum(v)/n
    return mu, (0.0 if n<2 else math.sqrt(sum((x-mu)**2 for x in v)/(n-1)/n)), n
a=[float(x) for x in sys.argv[1].split()]; c=[float(x) for x in sys.argv[2].split()]
ma,sa,na=st(a); mc,sc,nc=st(c)
den=math.hypot(sa,sc); t=(mc-ma)/den if den else 0.0
pct=100*(mc-ma)/ma
print("    base %.0f+-%.0f (n=%d)  cand %.0f+-%.0f (n=%d)  %+.3f%%  t=%.2f" % (ma,sa,na,mc,sc,nc,pct,t))
# regressed: the candidate sends measurably fewer samples. 0.4% is well below the 1.107% the whole range costs and well
# above the 0.07% SE of a single arm, so it separates "this commit is part of it" from run-to-run noise.
print("VERDICT=%s" % ("REGRESSED" if (pct < -0.4 and t < -2) else "clean"))
PYEOF
}

base_sha=$(build_into "$GOOD" base)
say "baseline $GOOD built, client sha256=$base_sha"
[ "$base_sha" = BUILD_FAILED ] && { say "FATAL baseline does not build"; exit 1; }

lo=0; hi=${#CANDS[@]}
while [ $((hi - lo)) -gt 1 ]; do
    mid=$(( (lo + hi) / 2 ))
    sha=$(git -C "$REPO" rev-parse --short "${CANDS[$((mid - 1))]}")
    subject=$(git -C "$REPO" log -1 --format=%s "${CANDS[$((mid - 1))]}" | cut -c1-70)
    say "--- step: candidate $mid/${#CANDS[@]} $sha  $subject"
    cand_sha=$(build_into "$sha" cand)
    if [ "$cand_sha" = BUILD_FAILED ]; then
        say "    SKIP (does not build); treating as clean and moving on"
        lo=$mid; continue
    fi
    say "    built, client sha256=$cand_sha"
    # One retry on a void step, as the reading above pre-registered. A void step must not fall through to "clean":
    # that silently moves the bisect on a step that measured nothing, which is what the first run of this script did.
    out=$(compare_step cand); say "$out"
    case "$out" in *VERDICT=*) : ;; *)
        say "    step VOID - retrying once"
        out=$(compare_step cand); say "$out" ;;
    esac
    case "$out" in
        *VERDICT=REGRESSED*) hi=$mid;;
        *VERDICT=clean*)     lo=$mid;;
        *) say "    step VOID twice - stopping rather than guessing. Resume with GOOD=${CANDS[$((lo))]:-$GOOD} BAD=$BAD"
           exit 2;;
    esac
done
kill_server
if [ "$hi" -le "${#CANDS[@]}" ] && [ "$hi" -gt 0 ]; then
    first=$(git -C "$REPO" rev-parse --short "${CANDS[$((hi - 1))]}")
    say "=== first regressing commit among those touching core: $first"
    git -C "$REPO" log -1 --format='    %h %ad %s' --date=format:'%m-%d %H:%M' "${CANDS[$((hi - 1))]}" | tee -a "$OUT"
    git -C "$REPO" show --stat --format='' "${CANDS[$((hi - 1))]}" -- src include | tee -a "$OUT"
else
    say "=== no candidate in the range regressed by the rule; the cost is not in src/ or include/, or it is spread"
fi
say "=== done $(date -Is) ==="

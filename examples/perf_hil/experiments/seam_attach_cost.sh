#!/usr/bin/env bash
# seam_attach_cost.sh - is TickLE's halved cross-host throughput the transport seam retrying a failed attach?
#
# THE OBSERVATION. Cell 1 (reliable_throughput p1, -d 5, 3 reps), same rig, same harness, same day:
#     9dbffd40   114.62 Mbps   5.311 cpu_s_per_Msample     (before the shared-memory work)
#     d4413383    52.88 Mbps  11.104 cpu_s_per_Msample     (after it)
#     cyclonedds  92.84 -> 93.62,  6.870 -> 6.860          <- control, flat on both metrics
# CycloneDDS being unchanged on the same runs rules out the rig, the load and the harness. Only TickLE moved, ~2x on
# both metrics, which is two orders above WIRE_PLAN 10.4's ~1% throughput floor.
#
# THE HYPOTHESIS, which is a READ of the code and is exactly what this script exists to doubt. peer_segment()
# (src/tickle.c:629) caches an entry only on a SUCCESSFUL attach; when tt_segment_attach() returns NULL nothing is
# written to the entry, so the next send recomputes the name and calls it again. On a two-host link no segment can ever
# exist, so every datagram pays an open() that fails ENOENT - about 87,000 failed syscalls/s at the observed rate. The
# RESULT lines agree that every datagram takes that path: tx_udp_unattached=436722 of tx_udp=436728. The function's own
# comment says a peer "costs one attempt rather than a subscription to its lifecycle", which is the intended behaviour
# and is not what the code does.
#
# TWO ARMS, ONE COMMIT. Both arms are HEAD; they differ only by -Dtt_SEGMENT_ENABLED=0, which compiles the segment
# attempt out and leaves the counting #else in place. Same commit both sides means today's other 608 lines of src/ are
# held constant, so this isolates the seam rather than the day.
#
# HOW TO READ IT, written before the run, by SIGN and not by magnitude:
#   - ON does NOT reproduce ~53 Mbps            -> VOID. The whole run says nothing either way; something other than
#                                                  the thing under test changed, and a quiet rig must not read as a
#                                                  closed question. Checked FIRST, before any comparison.
#   - OFF > ON beyond 2xSE and beyond ~1%       -> FINDING: the per-datagram retry is the cost. Consequences, stated
#                                                  now so they are not negotiated after seeing the number: stage 1's
#                                                  p1-p4 figures are not measurable until it is fixed, and
#                                                  COMPARISON.md 2.7's cell-8 TickLE row (measured on 54c27bbc, after
#                                                  the seam landed) is an artefact - including its "a cell we lose to
#                                                  a DDS implementation" claim.
#   - OFF within 2xSE of ON                     -> NOT THE SEAM. The halving is elsewhere in 9dbffd40..d4413383 and
#                                                  the next step is a bisect over those commits, not a fix here.
#   - OFF < ON beyond 2xSE                      -> UNEXPECTED. Report the numbers and draw no conclusion; a segment
#                                                  path that is faster switched off than compiled out is a third bug.
#   - any run with sent < 100000, or an arm with fewer than 3 usable runs -> that arm is VOID and said so explicitly.
#
# Both scenarios are run because cell 8 is the one already published: cell 1 is the observation above, cell 8 is
# COMPARISON.md 2.7's table.
#
# Discipline carried over from p1_layout_check.sh, each of which is there because it went wrong once: arms are built
# ONCE and saved aside (a build inside an A/B loop rewrites the underlay mid-run); servers are killed by the PID their
# own launch wrote and verified through /proc/PID/exe, never by a pattern; a forced arm whose binary is identical to
# its twin means the flag never reached the compiler and is FATAL rather than "the flag changed nothing".
#
# BUILD CHECK (2026-10-10). The stage 1 rig run at a1e6bd94 printed a gcc caret line as "client sha256=" for BOTH
# arms and then "FATAL ... identical to ON": the remote command ended `echo BUILD_FAILED; tail -5 log` and the capture
# kept only `| tail -1`, so BUILD_FAILED was cut off and the last line of gcc's error (a caret under a function name)
# became the "hash" of both arms. The build had failed because this script overwrote the commit's BenchStats.h with
# the launching checkout's copy, which was older than the commit's client/server ("too many arguments to
# bench_stats_set_shm_diagnostics"). So now: the remote side prints tagged lines (BUILD_RC, DIAG, TAIL, SHA256) and
# classify_build reads ONLY those. A failed build, a build with no well-formed SHA256 line, and a build whose bench
# sources (examples/) emitted a diagnostic are each their own VOID reason, distinct from "identical binaries". Core
# warnings (src/, platform/) are listed with file:line and do not void: the same set appears at the commit's parent
# (it differs between arms only by what each compiles out); STRICT_CORE_DIAG=1 voids on them too. The overlay is now
# opt-in (BENCH_STATS_OVERLAY=1) for commits older than the instrumentation they are measured with.
# Positive control, no rig: seam_attach_cost.sh --self-test   (fixtures that must VOID, and clean ones that must parse)
# Usage: seam_attach_cost.sh [SHA] [BLOCKS]    Output: $OUT (default /tmp/seam_attach_cost.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

# classify_build <remote output>: prints exactly one line, "OK <hash16>" or "VOID <reason>: <detail>", and then any
# "COREDIAG <line>" lines. Reads only the tagged lines the remote build command prints, never "the last line".
classify_build() {
    local text=$1 line rc="" hash="" nhash=0 bench=() core=() tailv=()
    while IFS= read -r line; do
        case "$line" in
        "BUILD_RC "*) rc=${line#BUILD_RC } ;;
        "SHA256 "*)
            nhash=$((nhash + 1))
            hash=${line#SHA256 }
            ;;
        "DIAG "*)
            line=${line#DIAG }
            case "$line" in */examples/*) bench+=("$line") ;; *) core+=("$line") ;; esac
            ;;
        "TAIL "*) tailv+=("${line#TAIL }") ;;
        esac
    done <<<"$text"
    local d
    if [ -z "$rc" ]; then
        echo "VOID not_checkable: no BUILD_RC line - the remote build command did not run to its report"
    elif [ "$rc" != 0 ]; then
        echo "VOID build_failed: build.sh exit $rc; first diagnostic: ${bench[0]:-${core[0]:-${tailv[0]:-none}}}"
    elif [ "${#bench[@]}" -gt 0 ]; then
        echo "VOID bench_diagnostics: ${#bench[@]} diagnostic line(s) in the bench's own sources; first: ${bench[0]}"
    elif [ "${STRICT_CORE_DIAG:-0}" = 1 ] && [ "${#core[@]}" -gt 0 ]; then
        echo "VOID core_diagnostics: ${#core[@]} line(s), STRICT_CORE_DIAG=1; first: ${core[0]}"
    elif [ "$nhash" != 1 ] || ! [[ $hash =~ ^[0-9a-f]{16}$ ]]; then
        echo "VOID not_checkable: expected one 'SHA256 <16 hex>' line, got $nhash (last value '$hash')"
    else
        echo "OK $hash"
    fi
    for d in "${core[@]}"; do echo "COREDIAG $d"; done
}

# remote_build_cmd <scen> <arm-name> <extra-cflags> <size> <save-dir> <log>: the shell text run on a Pi, and by
# --self-test locally against stub trees. Every line the caller reads is tagged; the build log stays where it was
# written and only its diagnostics travel. The outputs are removed first, so a binary left by an earlier build can
# never pass for this one's (the self-test's e2e_no_binary case failed on exactly that before the rm).
remote_build_cmd() {
    local scen=$1 name=$2 extra=$3 size=$4 save=$5 log=$6
    cat <<EOS
cd ~/tickle/examples/perf_hil/tickle || exit 0
rm -f ${scen}_${size}/client ${scen}_${size}/server
rc=0; TICKLE_EXTRA_CFLAGS='$extra' ./build.sh $scen $size > $log 2>&1 || rc=\$?
echo "BUILD_RC \$rc"
grep -E ': (fatal error|error|warning|note): ' $log | head -60 | sed 's/^/DIAG /'
if [ "\$rc" != 0 ]; then tail -5 $log | sed 's/^/TAIL /'; exit 0; fi
mkdir -p $save/$scen/$name && cp ${scen}_${size}/client ${scen}_${size}/server $save/$scen/$name/ && echo "SHA256 \$(sha256sum $save/$scen/$name/client | cut -c1-16)"
EOS
}

self_test() {
    local fail=0 got
    check() { # check <name> <expected prefix> <text>
        got=$(classify_build "$3" | head -1)
        case "$got" in "$2"*) echo "PASS $1: $got" ;; *)
            echo "FAIL $1: expected '$2...', got '$got'"
            fail=1
            ;;
        esac
    }
    local caret='      |                    ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~'
    local err='/home/ci/tickle/examples/perf_hil/tickle/reliable_throughput/server.c:385:5: error: too many arguments to function'
    local note='/home/ci/tickle/examples/perf_hil/tickle/common/BenchStats.h:401:20: note: declared here'
    local corew="/home/ci/tickle/src/tickle.c:6130:18: warning: unused variable 'min_ack' [-Wunused-variable]"
    # The 2026-10-10 failure as the OLD command reported it: one caret line and nothing else. Must not parse as a hash.
    check old_format_caret "VOID not_checkable" "$caret"
    # The same failure through the new command: rc 1, the error, the tail ending in the caret.
    check build_failed "VOID build_failed" "BUILD_RC 1
DIAG $err
DIAG $note
TAIL $note
TAIL $caret"
    # A failed build that still printed a hash (cannot happen by construction; the rc must win anyway).
    check failed_with_hash "VOID build_failed" "BUILD_RC 2
SHA256 0123456789abcdef"
    check bench_warning "VOID bench_diagnostics" "BUILD_RC 0
DIAG /home/ci/tickle/examples/perf_hil/tickle/reliable_throughput/client.c:10:5: warning: implicit declaration
SHA256 0123456789abcdef"
    check no_hash "VOID not_checkable" "BUILD_RC 0"
    check caret_as_hash "VOID not_checkable" "BUILD_RC 0
SHA256 $caret"
    check two_hashes "VOID not_checkable" "BUILD_RC 0
SHA256 0123456789abcdef
SHA256 fedcba9876543210"
    # Clean ones that must parse. Core warnings are reported and do not void unless STRICT_CORE_DIAG=1.
    check clean "OK 0123456789abcdef" "BUILD_RC 0
SHA256 0123456789abcdef"
    check clean_core_warning "OK 0123456789abcdef" "BUILD_RC 0
DIAG $corew
SHA256 0123456789abcdef"
    STRICT_CORE_DIAG=1 check strict_core_warning "VOID core_diagnostics" "BUILD_RC 0
DIAG $corew
SHA256 0123456789abcdef"
    got=$(classify_build "BUILD_RC 0
DIAG $corew
SHA256 0123456789abcdef" | grep -c '^COREDIAG .*tickle.c:6130:18: warning')
    if [ "$got" = 1 ]; then echo "PASS core_warning_listed"; else
        echo "FAIL core_warning_listed: $got"
        fail=1
    fi
    # The real remote command, run locally against stub build.sh trees (no rig, no ssh, no TickLE process).
    local t stub hash_expect
    t=$(mktemp -d) || return 1
    mkdir -p "$t/tickle/examples/perf_hil/tickle"
    stub="$t/tickle/examples/perf_hil/tickle/build.sh"
    e2e() { # e2e <name> <expected prefix> <stub body>
        printf '#!/bin/sh\n%s\n' "$3" >"$stub"
        chmod +x "$stub"
        got=$(HOME="$t" bash -c "$(remote_build_cmd reliable_throughput ON '-Dx=1' p1 "$t/save" "$t/build.log")" 2>&1)
        check "$1" "$2" "$got"
    }
    e2e e2e_build_failed "VOID build_failed" "echo '$err' >&2; echo '$note' >&2; echo '$caret' >&2; exit 1"
    e2e e2e_bench_warning "VOID bench_diagnostics" "echo '/x/examples/perf_hil/tickle/common/BenchStats.h:1:1: warning: w' >&2
mkdir -p reliable_throughput_p1; echo c > reliable_throughput_p1/client; echo s > reliable_throughput_p1/server"
    e2e e2e_no_binary "VOID not_checkable" "exit 0"
    # The stub writes the flags it was handed into the client, so the expected hash also proves they arrived.
    hash_expect=$(printf 'c%s\n' '-Dx=1' | sha256sum | cut -c1-16)
    e2e e2e_clean "OK $hash_expect" "echo \"$corew\" >&2
mkdir -p reliable_throughput_p1; echo \"c\$TICKLE_EXTRA_CFLAGS\" > reliable_throughput_p1/client; echo s > reliable_throughput_p1/server"
    rm -rf "$t"
    if [ "$fail" = 0 ]; then echo "SELF_TEST PASS"; else echo "SELF_TEST FAIL"; fi
    return "$fail"
}
if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${1:-$(git -C "$REPO" rev-parse --short HEAD)}
BLOCKS=${2:-3}
OFF_FLAG=${OFF_FLAG:--Dtt_SEGMENT_ENABLED=0}
DUR=${DUR:-5}
SIZE=${SIZE:-p1}
SCENS=${SCENS:-"reliable_throughput best_effort_throughput"}
OUT=${OUT:-/tmp/seam_attach_cost.txt}
# What the ON arm is expected to reproduce. Before the fix this guarded against measuring a situation that had
# changed under us: ON had to come back near the 52.88/53.97 that prompted the run, or the comparison meant nothing.
# After the fix the question is the opposite one - whether ON has caught up with OFF - so the reference moves with
# it, and is passed in rather than edited, so the script keeps working for both questions.
REF_RELIABLE=${REF_RELIABLE:-52.88}
REF_BEST_EFFORT=${REF_BEST_EFFORT:-53.97}
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
SAVE=/tmp/seamcost
srv_pid=""
LAST_SENT=""
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
# Said to the file and stderr, never stdout: build_arm's stdout is captured into a variable.
note() { echo "$*" >>"$OUT"; echo "$*" >&2; }
say "  reference: ON expected near reliable=$REF_RELIABLE best_effort=$REF_BEST_EFFORT"
say "=== seam attach cost $(date -Is) sha=$SHA off='$OFF_FLAG' dur=$DUR blocks=$BLOCKS scens='$SCENS' ==="

ARM_HASH=""
ARM_VOID=""
build_arm() { # build_arm <scen> <arm-name> <extra-cflags>; sets ARM_HASH (client hash) or ARM_VOID (the reason)
    local scen=$1 name=$2 extra=$3 h out verdict d
    ARM_HASH=""
    ARM_VOID=""
    for h in "$CLIENT" "$SERVER"; do
        # The commit's own BenchStats.h unless asked: overlaying the launching checkout's copy onto another commit is
        # what broke the a1e6bd94 build (its server.c calls a function that copy declares with one argument fewer).
        if [ "${BENCH_STATS_OVERLAY:-0}" = 1 ]; then
            sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cat > examples/perf_hil/tickle/common/BenchStats.h" <"$REPO/examples/perf_hil/tickle/common/BenchStats.h" >/dev/null
        else
            sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log" </dev/null >/dev/null
        fi || {
            ARM_VOID="checkout_failed: on $h"
            return 1
        }
        out=$(sh_ "$h" "$(remote_build_cmd "$scen" "$name" "$extra" "$SIZE" "$SAVE" /tmp/seam_build.log)" </dev/null 2>&1)
        verdict=$(classify_build "$out")
        while IFS= read -r d; do note "    $scen arm $name on $h core diagnostic: $d"; done < <(printf '%s\n' "$verdict" | sed -n 's/^COREDIAG //p')
        case "$verdict" in
        "OK "*) ;;
        *)
            ARM_VOID="$(printf '%s\n' "$verdict" | head -1 | sed 's/^VOID //') (arm $name, on $h)"
            printf '%s\n' "$out" | sed 's/^/    remote: /' >>"$OUT"
            return 1
            ;;
        esac
        # Both hosts build the same tree; the client host's hash is the one compared between arms.
        [ "$h" = "$CLIENT" ] && ARM_HASH=$(printf '%s\n' "$verdict" | head -1 | cut -d' ' -f2)
    done
    note "  $scen arm $name (extra='$extra') built, client sha256=$ARM_HASH"
}

kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    sh_ "$SERVER" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/*/server|\"$SAVE\"/*/*/\"server (deleted)\") kill -TERM $srv_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" </dev/null >/dev/null
    srv_pid=""
}

assert_one_server() {
    # shellcheck disable=SC2016 # single-quoted on purpose: $p and $e expand on the Pi, not here
    sh_ "$SERVER" 'n=0; for p in /proc/[0-9]*; do e=$(readlink $p/exe 2>/dev/null) || continue; case "$e" in /tmp/seamcost/*/*/server*) n=$((n+1));; esac; done; echo $n' </dev/null
}

run_one() { # run_one <scen> <arm-name>; sets LAST_SENT
    local scen=$1 name=$2 line alive
    LAST_SENT=""
    kill_server
    srv_pid=$(sh_ "$SERVER" "cd $SAVE/$scen/$name && rm -f /tmp/seam_server.pid && (setsid sh -c 'echo \$\$ > /tmp/seam_server.pid; exec taskset -c 1-3 ./server -Q -d $((DUR + 40))' > /tmp/seam_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/seam_server.pid" </dev/null)
    sh_ "$SERVER" "grep -q 'Node open' /tmp/seam_server.log" </dev/null || { say "    no server opened for $scen arm=$name"; return 0; }
    alive=$(assert_one_server)
    [ "$alive" = 1 ] || { say "    ABORT: $alive of our servers alive, expected 1"; return 0; }
    line=$(sh_ "$CLIENT" "cd $SAVE/$scen/$name && taskset -c 1-3 ./client -Q -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    LAST_SENT=$(echo "$line" | grep -oE '(^| )sent=[0-9]+' | head -1 | cut -d= -f2)
    [ -n "$LAST_SENT" ] && say "scen=$scen arm=$name $line"
}

for scen in $SCENS; do
    say "### $scen ###"
    sha_on=""
    sha_off=""
    if build_arm "$scen" ON ""; then sha_on=$ARM_HASH; fi
    [ -z "$ARM_VOID" ] && { if build_arm "$scen" OFF "$OFF_FLAG"; then sha_off=$ARM_HASH; fi; }
    if [ -n "$ARM_VOID" ]; then
        # Its own reason, never "identical binaries": nothing was built that could be compared.
        say "VERDICT=VOID - build failed / not checkable: $ARM_VOID"
        say "  No binary pair to compare for $scen; the remote's tagged report is in $OUT."
        exit 1
    fi
    say "  built: ON=$sha_on OFF=$sha_off"
    if [ "$sha_on" = "$sha_off" ]; then
        say "VERDICT=VOID - identical binaries: the OFF arm's client equals ON's."
        say "FATAL $scen: the OFF arm's binary is identical to ON - '$OFF_FLAG' never reached the compiler."
        say "  Reporting no verdict for this scenario: it would otherwise read as 'the seam costs nothing'."
        exit 1
    fi
    for b in $(seq 1 "$BLOCKS"); do
        say "--- $scen block $b/$BLOCKS $(date -Is) ---"
        for arm in ON OFF OFF ON; do
            run_one "$scen" "$arm"
            [ -z "$LAST_SENT" ] || [ "$LAST_SENT" -ge 100000 ] || say "    VOID run $scen arm=$arm sent=$LAST_SENT"
        done
    done
    kill_server
done
say "=== done $(date -Is) ==="

REF_RELIABLE="$REF_RELIABLE" REF_BEST_EFFORT="$REF_BEST_EFFORT" python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import math, re, sys, collections
rows = collections.defaultdict(lambda: collections.defaultdict(list))
for line in open(sys.argv[1]):
    m = re.match(r"scen=(\S+) arm=(\w+) (RESULT.*)", line.strip())
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(3).split() if "=" in kv)
    try:
        sent = int(f.get("sent", 0))
    except ValueError:
        continue
    if sent < 100000:
        continue
    for k in ("send_mbps", "cpu_s_per_Msample", "sched_cpu_s_per_Msample"):
        if k in f:
            try:
                rows[m.group(1)][(m.group(2), k)].append(float(f[k]))
            except ValueError:
                pass

def stat(v):
    n = len(v); mu = sum(v) / n
    return mu, (0.0 if n < 2 else math.sqrt(sum((x - mu) ** 2 for x in v) / (n - 1) / n)), n

# The observation this run has to reproduce before it may compare anything.
import os
OBSERVED_ON_MBPS = {"reliable_throughput": float(os.environ.get("REF_RELIABLE", "52.88")),
                    "best_effort_throughput": float(os.environ.get("REF_BEST_EFFORT", "53.97"))}

for scen, d in rows.items():
    print("\n=== %s ===" % scen)
    on = d.get(("ON", "send_mbps"), [])
    off = d.get(("OFF", "send_mbps"), [])
    if len(on) < 3 or len(off) < 3:
        print("VOID: ON has %d usable runs, OFF has %d; fewer than 3 in an arm" % (len(on), len(off)))
        continue
    mon, son, non = stat(on)
    moff, soff, noff = stat(off)
    # Checked first: the ON arm must land near the figure that prompted the run.
    # A reference of 0 means "no prior ON figure for this cell, so there is nothing to reproduce" - the guard is
    # skipped rather than dividing by zero. It exists to catch a changed situation, and a cell measured for the first
    # time has no situation to have changed.
    ref = OBSERVED_ON_MBPS.get(scen) or None
    if ref is not None and abs(mon - ref) / ref > 0.15:
        print("VERDICT=VOID - ON came back %.2f Mbps against the %.2f that prompted this run (>15%% apart)." % (mon, ref))
        print("  Something other than the thing under test changed; this run says nothing either way.")
        continue
    print("  send_mbps   ON %.2f+-%.2f (n=%d)   OFF %.2f+-%.2f (n=%d)" % (mon, son, non, moff, soff, noff))
    for k in ("cpu_s_per_Msample", "sched_cpu_s_per_Msample"):
        a, b = d.get(("ON", k), []), d.get(("OFF", k), [])
        if len(a) >= 3 and len(b) >= 3:
            ma, sa, _ = stat(a); mb, sb, _ = stat(b)
            print("  %-22s ON %.3f+-%.3f   OFF %.3f+-%.3f   %+.1f%%" % (k, ma, sa, mb, sb, 100 * (mb - ma) / ma))
    den = math.hypot(son, soff)
    t = 0.0 if den == 0 else (moff - mon) / den
    pct = 100 * (moff - mon) / mon
    print("  OFF vs ON: %+.2f%%  t=%.2f" % (pct, t))
    if abs(t) <= 2 or abs(pct) <= 1.0:
        print("VERDICT=NO_DIFFERENCE - the two arms are within the floor of each other (%+.2f%%, t=%.2f)." % (pct, t))
        print("  Whatever OFF_FLAG removed does not cost measurably at this cell.")
    elif pct > 0:
        print("VERDICT=FINDING - what OFF_FLAG removes costs %+.2f%% at this cell (t=%.2f)." % (pct, t))
        print("  What that means depends on the flag, and the run's own header says which was passed:")
        print("    -Dtt_SEGMENT_ENABLED=0                  -> the module's total cost where no peer can be same-host")
        print("    -Dtt_SEGMENT_ATTACH_RETRY_SENDS=<large> -> the negative cache's re-ask interval alone")
        print("  Subtracting the second from the first is the structural cost of the seam itself.")
    else:
        print("VERDICT=UNEXPECTED - OFF is SLOWER than ON (%+.2f%%, t=%.2f). No conclusion drawn." % (pct, t))
        print("  A segment path faster compiled in than compiled out is a third thing, and wants its own look.")
PYEOF

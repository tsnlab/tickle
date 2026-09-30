#!/usr/bin/env bash
# lazy_segment_lifecycle.sh - does the shared-memory segment really come into being only when a same-host peer
# exists, survive that peer leaving while the writer keeps running, and come back when one returns?
#
# WHY THIS IS A SCRIPT OF ITS OWN. run_scenario.sh cannot answer it for two reasons, neither of which is worth
# changing in a file every campaign cell depends on:
#   - it keeps only `^RESULT:` lines from both sides, so the "Node <id> traffic:" line - the ONLY place the
#     segment counters appear - has never once been read in a perf run. The counters exist since 8cf8cdc9 and
#     ff57253a and are unreachable from every existing harness.
#   - it stops the reader with `pkill -INT -x server`, a name match. This script signals a pid it captured at
#     launch and checks /proc/<pid>/exe before signalling, because a name match will one day hit the wrong
#     process and the run that suffers will not say so.
# The counters print from node_destroy_locked(), so every process here is stopped with SIGINT and allowed to
# destroy its context. A SIGKILL anywhere in this script produces a run with NO counters at all, which is why a
# missing traffic line is reported as "could not look", never as a zero.
#
# THE ARMS, AND WHAT EACH OUTCOME MEANS - written down before running, because the interesting results here are
# absences and an absence is the easiest thing in the world to explain away afterwards.
#
#   X  control, cross-host: writer on one Pi, reader on the other. No same-host peer can exist.
#        expect  created=0  same_host_peers=0  tx_shm=0  tx_udp>0
#        created>0 here means creation is NOT deferred - the whole claim of 8cf8cdc9 fails, and every "no cost
#        when unused" statement in COMPARISON.md has to come out. This is the arm that can refute the feature.
#        tx_shm>0 here would be worse: a segment reached a peer that is not on this host.
#
#   S  same-host: writer and reader both on the same Pi.
#        expect  created>=1  same_host_peers>=1  tx_shm>0
#        created=0 with same_host_peers>=1 means the peer was seen and the segment was not built: lazy creation
#        deferred forever, which reads in a throughput cell as "shared memory gives us nothing".
#        created>=1 with tx_shm=0 sends the reader to shm_full_dropped and shm_gave_up, not to the ring size.
#
#   R  release while the writer runs: same-host, but the reader is SIGINTed at RELEASE_AT and the writer keeps
#      publishing to the end. This is exactly what ff57253a is about - release_own_segment() unmapping a segment
#      while the context still lives, where move_id() can renumber a context whose peer table does not follow.
#        expect  created>=1  released>=1  same_host_peers=0 at exit  tx_shm>0 AND tx_udp>0
#        tx_udp=0 after a release means the writer had nowhere to go and the samples died with the segment.
#        released=0 means the segment outlived its only user - the leak ff57253a was written to prevent.
#        a crash, or a missing RESULT line, is the move_id hazard reappearing and is a core bug, not a metric.
#
#   A  re-attach: as R, then a SECOND reader starts at REATTACH_AT and the writer runs on to the end.
#        expect  created>=2  released>=1  same_host_peers>=1 at exit  tx_shm>0
#        created=1 means the writer never rebuilt a segment for the new peer: the first departure disabled the
#        path for the rest of the process's life. That is invisible in any steady-state cell and is the reason
#        this arm exists.
#
# VOID, not failure: any arm whose reader reports received=0 measured nothing and cannot speak to any of the
# above - it is void and says so. Same for an arm whose traffic line could not be read.
#
# Usage: lazy_segment_lifecycle.sh          Output: $OUT (default /tmp/lazy_segment_lifecycle.txt)
#   ARMS="X S R A"  DUR=20  RELEASE_AT=8  REATTACH_AT=14  PAYLOAD=p1  SCEN=best_effort_throughput
#
# LSL_SELFTEST=1 runs everything from "read the logs" onward against logs already sitting in $DIAG, touching no
# host and taking no lock. It exists so the verdicts above can be shown to DECIDE - fed a passing log and a log
# with one counter changed, they must disagree - because criteria that have only ever seen one outcome are not
# criteria. It is the same code path, not a copy of it: a copy drifts from the original and then tests itself.
set -uo pipefail
SELFTEST=${LSL_SELFTEST:-0}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [ "$SELFTEST" != 1 ]; then
    export RIG_LOCK_SCOPE=hil
    if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
fi

ARMS=${ARMS:-"X S R A"}
DUR=${DUR:-20}
RELEASE_AT=${RELEASE_AT:-8}
REATTACH_AT=${REATTACH_AT:-14}
PAYLOAD=${PAYLOAD:-p1}
SCEN=${SCEN:-best_effort_throughput}
# Extra flags for the examples, so a QoS can be varied without a second copy of this harness. CLI_ARGS="-Q" runs
# the writer under HISTORY.KEEP_ALL, where core refuses a write rather than discarding it - the one setting under
# which loss is not permitted, and therefore the only one that can say whether the shared-memory ring's
# drop-on-full participates in that refusal or bypasses it. Keep arm X in any such run: it is the same QoS over
# UDP with no segment, so loss there separates "KEEP_ALL or the reader's window" from "the segment".
CLI_ARGS=${CLI_ARGS:-}
SRV_ARGS=${SRV_ARGS:-}
OUT=${OUT:-/tmp/lazy_segment_lifecycle.txt}
DIAG=${DIAG:-/tmp/lazy_segment_diag}
K=$HOME/.ssh/tickle_ci_ed25519
HOST_A=${HOST_A:-10.1.1.213}   # writer's host in every arm
HOST_B=${HOST_B:-10.1.1.214}   # the other host, used only by arm X
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }

: >"$OUT"; mkdir -p "$DIAG"
say() { echo "$*" | tee -a "$OUT"; }

# Stop a process by the pid captured at its launch, never by name, and only after /proc says it is the binary we
# started. A pid that is already gone is reported: the reader exiting early on its own changes what the arm means.
stop_pid() {
    local host=$1 pid=$2 want=$3 label=$4
    [ -n "$pid" ] || { say "  !! $label: no pid was captured at launch, so nothing was signalled"; return 1; }
    local seen
    seen=$(sh_ "$host" "readlink /proc/$pid/exe 2>/dev/null" </dev/null)
    case "$seen" in
    *"$want") sh_ "$host" "kill -INT $pid" </dev/null && return 0 ;;
    "")  say "  !! $label: pid $pid on $host is already gone - it exited before this script stopped it"; return 1 ;;
    *)   say "  !! $label: pid $pid on $host is '$seen', not *$want - NOT signalled"; return 1 ;;
    esac
}

launch() {  # launch <host> <dir> <role> <args> <logfile> <pidfile>; echoes the pid
    local host=$1 dir=$2 role=$3 args=$4 log=$5 pidf=$6
    sh_ "$host" "cd ~/$dir && rm -f $pidf
(setsid sh -c 'echo \$\$ >$pidf; exec taskset -c 1-3 stdbuf -oL ./$role $args' >$log 2>&1 </dev/null &)
sleep 1; cat $pidf" </dev/null
}

fetch_log() {  # fetch_log <host> <remote log> <local name>; whole log, both streams, nothing filtered
    local host=$1 rlog=$2 name=$3
    if ! sh_ "$host" "cat $rlog" </dev/null >"$DIAG/$name" 2>/dev/null; then
        say "  !! could not read $host:$rlog - this arm has NO evidence, which is not the same as a zero"
        return 1
    fi
}

# Pull the counters out of the writer's traffic line. A field that is absent is reported as absent ("-"), never
# as 0: "the counter did not print" and "the counter printed zero" are the two answers this whole script is
# trying to tell apart, and a default of 0 would silently merge them into the interesting one.
field() { grep -oE "(^| )$2=[0-9]+" "$1" 2>/dev/null | tail -1 | cut -d= -f2 | grep -E '^[0-9]+$' || echo -; }

say "=== lazy segment lifecycle $(date -Is) arms='$ARMS' scen=$SCEN payload=$PAYLOAD dur=${DUR}s cli_args='$CLI_ARGS' srv_args='$SRV_ARGS' ==="
say "    writer host $HOST_A, other host $HOST_B, release at ${RELEASE_AT}s, re-attach at ${REATTACH_AT}s"

# Both hosts build from origin/main and the SHA is printed, because the counters being read here arrived in
# ff57253a and a host sitting on an older main would report their absence as a finding about the feature.
for h in $([ "$SELFTEST" = 1 ] || echo "$HOST_A" "$HOST_B"); do
    b=$(sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard origin/main
cd examples/perf_hil/tickle && ./build.sh $SCEN $PAYLOAD >/tmp/lsl_build.log 2>&1 || { echo BUILD_FAILED; cat /tmp/lsl_build.log; exit 0; }
echo \"sha=\$(git -C ~/tickle rev-parse --short HEAD) bin=\$(sha256sum ${SCEN}_${PAYLOAD}/client | cut -c1-16)\"" </dev/null 2>&1)
    case "$b" in
    *BUILD_FAILED*) say "FATAL $h build failed:"; say "$b"; exit 1 ;;
    *) say "  $h $b" ;;
    esac
done
REMOTE=tickle/examples/perf_hil/tickle/${SCEN}_${PAYLOAD}

for ARM in $ARMS; do
    say "### arm $ARM ###"
    case $ARM in
    X) RHOST=$HOST_B ;;
    *) RHOST=$HOST_A ;;
    esac
    wl="arm${ARM}_writer.log"; rl="arm${ARM}_reader.log"
    if [ "$SELFTEST" = 1 ]; then
        say "  selftest: reading $DIAG/$wl and $DIAG/$rl, no host touched"
    else
    rpid=$(launch "$RHOST" "$REMOTE" server "-d $((DUR + 10)) $SRV_ARGS" /tmp/lsl_r1.log /tmp/lsl_r1.pid)
    wpid=$(launch "$HOST_A" "$REMOTE" client "-d $DUR $CLI_ARGS" /tmp/lsl_w.log /tmp/lsl_w.pid)
    say "  reader pid $rpid on $RHOST, writer pid $wpid on $HOST_A"
    r2pid=""
    case $ARM in
    R|A)
        sleep "$RELEASE_AT"
        stop_pid "$RHOST" "$rpid" "/server" "reader (release)" && say "  reader SIGINTed at ${RELEASE_AT}s, writer still running"
        ;;
    esac
    if [ "$ARM" = A ]; then
        sleep $((REATTACH_AT - RELEASE_AT))
        r2pid=$(launch "$RHOST" "$REMOTE" server "-d $((DUR + 10)) $SRV_ARGS" /tmp/lsl_r2.log /tmp/lsl_r2.pid)
        say "  second reader pid $r2pid started at ${REATTACH_AT}s"
    fi
    # The writer ends on its own -d. Wait for it rather than signalling it, so its destroy is the ordinary path.
    end=$(( $(date +%s) + DUR + 30 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        [ -z "$(sh_ "$HOST_A" "readlink /proc/$wpid/exe 2>/dev/null" </dev/null)" ] && break
        sleep 2
    done
    if [ -n "$(sh_ "$HOST_A" "readlink /proc/$wpid/exe 2>/dev/null" </dev/null)" ]; then
        say "  !! writer $wpid outlived ${DUR}s+30s and was SIGINTed; its -d did not end it"
        stop_pid "$HOST_A" "$wpid" "/client" "writer (overrun)"
        sleep 3
    fi
    case $ARM in
    X|S) stop_pid "$RHOST" "$rpid" "/server" "reader (end)" ;;
    esac
    [ -n "$r2pid" ] && stop_pid "$RHOST" "$r2pid" "/server" "second reader (end)"
    sleep 3

    fetch_log "$HOST_A" /tmp/lsl_w.log "$wl" || continue
    fetch_log "$RHOST" /tmp/lsl_r1.log "$rl" || true
    [ -n "$r2pid" ] && fetch_log "$RHOST" /tmp/lsl_r2.log "arm${ARM}_reader2.log"
    fi
    if [ ! -f "$DIAG/$wl" ]; then
        say "  !! no writer log at $DIAG/$wl - nothing to judge (this is 'could not look', not a zero)"
        continue
    fi

    if ! grep -q 'traffic:' "$DIAG/$wl"; then
        say "  VOID arm $ARM: the writer printed no traffic line, so no counter can be read. Its context did not"
        say "       reach node_destroy_locked() - look for a crash or a SIGKILL, not for a segment story."
        grep -v '^RESULT' "$DIAG/$wl" | tail -8 | sed 's/^/    | /' | tee -a "$OUT"
        continue
    fi
    created=$(field "$DIAG/$wl" shm_segments_created)
    released=$(field "$DIAG/$wl" shm_segments_released)
    peers=$(field "$DIAG/$wl" shm_same_host_peers)
    txshm=$(field "$DIAG/$wl" tx_shm)
    txudp=$(field "$DIAG/$wl" tx_udp)
    full=$(field "$DIAG/$wl" shm_full_dropped)
    gave=$(field "$DIAG/$wl" shm_gave_up)
    # tickle's server prints recv=, zenoh-pico's prints received=. The first version of this looked only for
    # received= and so read "-" from every tickle run, which VOIDed four arms whose counters were in fact exactly
    # what they should have been. The selftest did not catch it because its fixture was written from the same wrong
    # assumption as the code - a fixture invented by the author cannot contradict the author. The fixtures are now
    # taken from a real run (LSL_SELFTEST=1 against a $DIAG that a real run filled), which is the only version of
    # this test that can fail for the right reason.
    recv=$(field "$DIAG/$rl" recv)
    [ "$recv" = - ] && recv=$(field "$DIAG/$rl" received)
    say "  arm=$ARM created=$created released=$released same_host_peers=$peers tx_shm=$txshm tx_udp=$txudp shm_full_dropped=$full shm_gave_up=$gave reader_received=$recv"
    if [ "$recv" = "-" ] || [ "${recv:-0}" = 0 ]; then
        say "  VOID arm $ARM: the reader received nothing, so this arm measured no delivery and its counters"
        say "       describe a writer talking to itself."
        continue
    fi
    verdict=PASS; why=""
    case $ARM in
    X) [ "$created" = 0 ] || { verdict=FAIL; why="$why created=$created must be 0 with no same-host peer;"; }
       [ "$peers" = 0 ]   || { verdict=FAIL; why="$why same_host_peers=$peers must be 0 across hosts;"; }
       [ "$txshm" = 0 ]   || { verdict=FAIL; why="$why tx_shm=$txshm must be 0 across hosts;"; }
       [ "${txudp:-0}" -gt 0 ] 2>/dev/null || { verdict=FAIL; why="$why tx_udp=$txudp must be >0;"; } ;;
    S) [ "${created:-0}" -ge 1 ] 2>/dev/null || { verdict=FAIL; why="$why created=$created must be >=1 same-host;"; }
       [ "${peers:-0}" -ge 1 ] 2>/dev/null   || { verdict=FAIL; why="$why same_host_peers=$peers must be >=1;"; }
       [ "${txshm:-0}" -gt 0 ] 2>/dev/null   || { verdict=FAIL; why="$why tx_shm=$txshm must be >0 same-host;"; } ;;
    R) [ "${created:-0}" -ge 1 ] 2>/dev/null  || { verdict=FAIL; why="$why created=$created must be >=1;"; }
       [ "${released:-0}" -ge 1 ] 2>/dev/null || { verdict=FAIL; why="$why released=$released must be >=1 after the peer left;"; }
       [ "$peers" = 0 ]                       || { verdict=FAIL; why="$why same_host_peers=$peers must be back to 0;"; }
       [ "${txshm:-0}" -gt 0 ] 2>/dev/null    || { verdict=FAIL; why="$why tx_shm=$txshm must be >0 before the release;"; }
       [ "${txudp:-0}" -gt 0 ] 2>/dev/null    || { verdict=FAIL; why="$why tx_udp=$txudp must be >0 after it;"; } ;;
    A) [ "${created:-0}" -ge 2 ] 2>/dev/null  || { verdict=FAIL; why="$why created=$created must be >=2: a segment was not rebuilt for the new peer;"; }
       [ "${released:-0}" -ge 1 ] 2>/dev/null || { verdict=FAIL; why="$why released=$released must be >=1;"; }
       [ "${peers:-0}" -ge 1 ] 2>/dev/null    || { verdict=FAIL; why="$why same_host_peers=$peers must be >=1 at exit;"; } ;;
    esac
    say "  $verdict arm $ARM${why:+ -$why}"
done
say "=== done $(date -Is); per-arm logs in $DIAG ==="

#!/usr/bin/env bash
# Orchestrates one TickLE-native HIL scenario across the two rpis (rpi#1=client, rpi#2=server),
# mirroring ../cyclonedds/run_scenario.sh's own pattern (same '</dev/null'+';' fix for the SSH
# backgrounding hang, same $CLIENT_ARGS forwarded to both sides). No LD_LIBRARY_PATH/vendor URI
# needed - TickLE links statically (libtickle.a via pkg-config, build.sh) and has no discovery
# config knob equivalent to CycloneDDS's SPDPInterval.
set -euo pipefail

# Rig mutual exclusion (examples/perf_hil/rig_lock.sh): re-exec under the lock unless an outer
# scope (a sweep, or CI's run_perf.sh) already holds it - see that script's own header for the
# CI-vs-manual collision this prevents.
if [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi

SCENARIO="${1:?usage: run_scenario.sh <scenario> [client_args...]}"
shift
CLIENT_ARGS="${*:--d 10}"
# Overridable, not just for tuning (2026-09-21, real bug found the hard way): every other
# scenario's own server.c is a passive matcher (client can't usefully start until it exists), so a
# fixed pre-client sleep is a safe default everywhere - except history_depth_burst_loss, whose
# server.c does its OWN internal `-p` stall *before ever creating its Subscriber at all*. With the
# default 3s here, that scenario's subscriber was becoming discoverable (server's own internal
# pause completing) *before* the client had even started publishing (client only starts after this
# sleep, plus its own 2s discovery margin) - the eviction window under test never actually existed,
# silently producing 0 loss regardless of `-p`. `PRE_CLIENT_SLEEP=0` restores the real race: both
# sides launch together, and server.c's own `-p` is what creates the whole delay, same as the
# CycloneDDS/FastDDS twins achieve structurally via their own writer's match-wait blocking on a
# reader that doesn't yet exist - TickLE's own Publisher has no such match-wait to lean on (publish()
# is unconditional broadcast, verified directly in tickle.c), so this has to be an explicit,
# scenario-aware orchestration choice instead.
PRE_CLIENT_SLEEP="${PRE_CLIENT_SLEEP:-3}"

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
# Overridable so a same-host cell can put both processes on one Pi (SHM_PLAN.md's S6: the campaign has only cross-host
# cells, and the shared-memory module's whole case is the same-host tier). Defaults unchanged, so every existing cell runs
# exactly as before.
RPI_CLIENT="${RPI_CLIENT:-10.1.1.214}"
RPI_SERVER="${RPI_SERVER:-10.1.1.213}"
REMOTE_DIR="tickle/examples/perf_hil/tickle/$SCENARIO"

# -n (2026-10-11): no remote command here reads stdin, and ssh without -n reads it anyway. with_insn fetched the counts
# with an ssh inside `... <<<"$client_out"`, so ssh swallowed the RESULT line the sed after it was to print, and gate
# v2's first rig chains (X1-X3) read every run as "no RESULT line". rig_dryrun.sh's stand-in ssh now drains stdin as
# the real client does, so a dry run fails on this too.
ssh_run() {
    local host="$1"
    shift
    # shellcheck disable=SC2029
    ssh -n -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=5 "ci@$host" "$@"
}

# Pinned away from CPU0 (2026-09-23, measured). Both Pis handle eth0's interrupt, IRQ 108,
# entirely on CPU0 - 404 and 405 million interrupts there against zero on CPU1 through CPU3 - and
# a sender that the scheduler happens to place on CPU0 shares that core with the interrupt
# handler. Measured over 12 reps of reliable_throughput at 0% loss, with nothing pinned: sender on
# CPU0 gave 94.5-94.6 Mbit/s (n=3), sender anywhere else gave 110.8-112.3 (n=9), no overlap, and
# 3 of 12 is the 1-in-4 a four-core machine gives when nothing pins anything. That made every
# single-run figure a coin flip reading about 15% low a quarter of the time.
#
# This removes an artifact from the measurement rather than changing the machine: the NIC
# interrupt stays where the hardware puts it, so these numbers still describe the real platform.
# Applied to all three frameworks' harnesses, not just TickLE's - pinning only ours would hand
# TickLE the fast mode every run while leaving CycloneDDS and FastDDS on the coin flip, which
# would bias the comparison in our favour by about 15% a quarter of the time. A partial fix here
# is worse than none.
# Not pinned by default (2026-10-05, the user's rule): pinning is not how software is run, and the comparison must not
# rest on it. PIN="taskset -c 1-3" reproduces the earlier pinned runs, kept away from core 0 (the Pi's interrupt core),
# for the labelled note beside the unpinned headline.
PIN=${PIN:-}

# Both remote processes are stopped by the PID their launch recorded, checked against /proc/<pid>/exe, never by a name
# pattern - and from an EXIT trap, so a run that ends early stops them too (2026-10-07). This used to be
# `pkill -INT -x server` after the client returned, which a run killed by campaign_sweep.sh's `timeout 180`, or ended
# by `set -e` when the client printed no RESULT line, never reached: the server ran on (the DDS latency servers have no
# lifetime cap at all), and so did the remote client, which the local ssh's death does not stop. On 2026-10-07 such a
# client (c12, TickLE, giving every ping up at 500 ms after its server's cap expired) was the "leftover" that voided
# the next rows across c12 and c16.
SERVER_PID_FILE="/tmp/tickle_${SCENARIO}_server.pid"
CLIENT_PID_FILE="/tmp/tickle_${SCENARIO}_client.pid"
# INSN_COUNT=1 (gate v2 A2, 2026-10-11; experiments/gate_v2_lib.sh): each process runs under perf stat counting its
# own instructions:u and instructions:k, and its RESULT line gets insn_u= insn_k= insn=ok (or insn=fail:<why>) appended.
# For TickLE-vs-TickLE A/Bs, where both arms carry the same instrument; a cross-vendor campaign leaves it off, so no
# framework runs under an instrument the others do not. The recorded PID is then perf's, and the bench its child -
# stop_remote signals the child and waits for perf to write its counts.
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../experiments" && pwd)/gate_v2_lib.sh"
INSN_COUNT="${INSN_COUNT:-0}"
SERVER_INSN="/tmp/tickle_${SCENARIO}_server.insn"
CLIENT_INSN="/tmp/tickle_${SCENARIO}_client.insn"
SERVER_PRE="" CLIENT_PRE=""
if [ "$INSN_COUNT" = 1 ]; then
    SERVER_PRE=$(insn_prefix "$SERVER_INSN")
    CLIENT_PRE=$(insn_prefix "$CLIENT_INSN")
fi
# stop_remote <host> <pid file> <binary>: SIGINT (each bench's own clean exit, which prints its RESULT line), wait up
# to 10 s for it to go, then SIGKILL. Does nothing if neither the PID nor its child is that binary. When the PID is
# perf's (INSN_COUNT=1), waits up to 5 s more for perf itself to exit, so its counts are written.
stop_remote() {
    # shellcheck disable=SC2029 # $2, $3 and $REMOTE_DIR are expanded here on purpose; \$p on the rpi
    ssh_run "$1" "p=\$(cat '$2' 2>/dev/null) || exit 0
        $(remote_child_of_perf)
        t=\$(child_exe \$p '*/$REMOTE_DIR/$3') || exit 0
        is_it() { case \$(readlink /proc/\$t/exe 2>/dev/null) in */$REMOTE_DIR/$3) return 0 ;; esac; return 1; }
        kill -INT \$t
        gone=0
        for _ in \$(seq 1 100); do is_it || { gone=1; break; }; sleep 0.1; done
        [ \$gone = 1 ] || { kill -KILL \$t && echo \"$3 \$t ignored SIGINT for 10 s: SIGKILL\" >&2; }
        if [ \$t != \$p ]; then for _ in \$(seq 1 50); do [ -d /proc/\$p ] || break; sleep 0.1; done; fi" || true
}
# with_insn <host> <insn file>: appends the process's counts to each RESULT line on stdin (INSN_COUNT=1 only).
with_insn() {
    if [ "$INSN_COUNT" != 1 ]; then
        cat
        return 0
    fi
    local counts fields
    # shellcheck disable=SC2029 # $2 is expanded here on purpose
    counts=$(ssh_run "$1" "cat '$2' 2>/dev/null; rm -f '$2'" || true)
    fields=$(insn_fields "$counts")
    sed "s/\$/ $fields/"
}
# shellcheck disable=SC2329 # invoked by the EXIT trap below
stop_both() {
    stop_remote "$RPI_CLIENT" "$CLIENT_PID_FILE" client
    stop_remote "$RPI_SERVER" "$SERVER_PID_FILE" server
}
trap stop_both EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

ssh_run "$RPI_SERVER" "rm -f $SERVER_INSN; cd ~/$REMOTE_DIR; nohup $PIN $SERVER_PRE ./server $CLIENT_ARGS > /tmp/tickle_${SCENARIO}_server.log 2>&1 < /dev/null & echo \$! > $SERVER_PID_FILE"
sleep "$PRE_CLIENT_SLEEP"
client_out=$(ssh_run "$RPI_CLIENT" "rm -f $CLIENT_INSN; cd ~/$REMOTE_DIR && echo \$\$ > $CLIENT_PID_FILE && exec $PIN $CLIENT_PRE ./client $CLIENT_ARGS" | grep '^RESULT:')
with_insn "$RPI_CLIENT" "$CLIENT_INSN" <<<"$client_out"
# SIGINT the server right after the client finishes, then read its log - not just a fixed sleep
# (2026-09-21, real bug found the hard way): some scenarios' own server.c only prints its own
# RESULT line once interrupted or once its own (possibly much longer than the client's) internal
# deadline elapses - durability_late_join/server.c's own `-d` (ack wait, default 40s) is the clear
# example. A bare `sleep 1; cat log` raced that and found no RESULT line yet, and `pipefail` then
# aborted the whole script (the real symptom this fixes) before ever reaching the stop below.
stop_remote "$RPI_SERVER" "$SERVER_PID_FILE" server
server_out=$(ssh_run "$RPI_SERVER" "cat /tmp/tickle_${SCENARIO}_server.log" | grep '^RESULT:')
with_insn "$RPI_SERVER" "$SERVER_INSN" <<<"$server_out"

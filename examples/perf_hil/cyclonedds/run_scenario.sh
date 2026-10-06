#!/usr/bin/env bash
# Orchestrates one CycloneDDS HIL scenario across the two rpis (rpi#1=client, rpi#2=server),
# mirroring .github/scripts/run_perf.sh's own run_paired_test() pattern for TickLE's own HIL
# binaries, standalone (not touching that established script - COMPARISON.md's own design note:
# this whole exercise is a separate track from TickLE Dev's own core/rmw_tickle work).
#
# LD_LIBRARY_PATH set explicitly rather than relying on the built binary's own rpath - a real,
# confirmed glibc ld.so quirk found the hard way: libddsc.so.0 carries its own DT_RUNPATH, which
# shadows the executable's DT_RPATH for libddsc's *own* transitive dependencies (libiceoryx_
# binding_c.so) - no rpath flag combination on the executable itself reliably covers this, but
# LD_LIBRARY_PATH always wins regardless of that shadowing rule.
#
# CYCLONEDDS_URI shortens SPDPInterval from its own 30s default - a real, confirmed root cause
# (not tuned blind): CycloneDDS's default participant-discovery announce interval is 30s, and each
# participant only otherwise announces once at its own startup - common.h's wait_for_*_match()
# helpers make every scenario correctly *wait* for a real match instead of guessing a sleep
# duration, but without this, an unlucky timing miss on the one-shot startup announce would still
# mean waiting out the full 30s before the next one, not a code bug on either end.
set -euo pipefail

# Rig mutual exclusion (examples/perf_hil/rig_lock.sh): re-exec under the lock unless an outer
# scope (a sweep, or CI's run_perf.sh) already holds it - see that script's own header for the
# CI-vs-manual collision this prevents.
if [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi

SCENARIO="${1:?usage: run_scenario.sh <scenario> [client_args...]}"
shift
CLIENT_ARGS="${*:--i 0.1 -d 10}"

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
# Overridable so a same-host cell can put both processes on one Pi (SHM_PLAN.md's S6: the campaign has only cross-host
# cells, and the shared-memory module's whole case is the same-host tier). Defaults unchanged, so every existing cell runs
# exactly as before.
RPI_CLIENT="${RPI_CLIENT:-10.1.1.214}"
RPI_SERVER="${RPI_SERVER:-10.1.1.213}"
LIB_PATH="/opt/ros/jazzy/lib/aarch64-linux-gnu"
# Pinned to eth0, the rig's own wired test link (2026-09-23, a measured incident): unpinned,
# CycloneDDS picks its interface from the default route, which on both rpis is wlan0 on the shared
# lab network - so the rig's DDS scenarios discovered over that network on the default domain 0,
# where this project's own same-host benchmark was also running. A comparison run aborted twice
# with "Data consistency violated ... Received sample id 100 Prev. sample id : 7146", which is a
# second publisher on the topic, not a middleware defect. The TickLE scenarios never had this: they
# pin _tt_CONFIG.broadcast to 192.168.10.255 in every client.c/server.c.
CDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>'
REMOTE_DIR="tickle/examples/perf_hil/cyclonedds/$SCENARIO"

ssh_run() {
    local host="$1"
    shift
    # shellcheck disable=SC2029
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=5 "ci@$host" "$@"
}

# </dev/null on the backgrounded remote process, and ';' (not '&&') between cd and nohup (2026-09-20,
# both real hangs found the hard way, isolated one at a time via minimal repro over plain ssh): with
# '&&', `cd dir && nohup cmd &` never returns control to the local ssh client at all - reproduced with
# a plain `nohup sleep 30 &`, nothing CycloneDDS-specific about it - while the equivalent `cd dir;
# nohup cmd &` returns in well under a second. Without </dev/null, the backgrounded process inherits
# this ssh session's own stdin, so ssh never sees every fd close and never returns either. Skipping
# either fix makes run_scenario.sh stall forever on this exact line (not a CycloneDDS bug at all, easy
# to mistake for one mid-debugging since sleep 2/the client step below never even get reached).
#
# $CLIENT_ARGS forwarded to the server too (2026-09-20, real bug found the hard way): this used to
# start every scenario's server with zero args, so `run_scenario.sh durability_late_join -D` gave the
# CLIENT a durable (TRANSIENT_LOCAL) reader QoS while the SERVER's writer stayed VOLATILE - a genuine
# RxO incompatibility (a reader can't require more durability than a writer offers), which correctly,
# deterministically never matches - looked exactly like a discovery bug from the client's own "timed
# out waiting for a match" but wasn't one. Every other scenario's server.c only recognizes -d (used as
# its own safety-cap, a sensible value to share with the client's -d) and silently ignores anything
# else, so forwarding the same args here is safe generally, not just for this one scenario.
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
SERVER_PID_FILE="/tmp/cdds_${SCENARIO}_server.pid"
CLIENT_PID_FILE="/tmp/cdds_${SCENARIO}_client.pid"
# stop_remote <host> <pid file> <binary>: SIGINT (each bench's own clean exit, which prints its RESULT line), wait up
# to 10 s for it to go, then SIGKILL. Does nothing if the PID is no longer that binary.
stop_remote() {
    # shellcheck disable=SC2029 # $2, $3 and $REMOTE_DIR are expanded here on purpose; \$p on the rpi
    ssh_run "$1" "p=\$(cat '$2' 2>/dev/null) || exit 0
        is_it() { case \$(readlink /proc/\$p/exe 2>/dev/null) in */$REMOTE_DIR/$3) return 0 ;; esac; return 1; }
        is_it || exit 0
        kill -INT \$p
        for _ in \$(seq 1 100); do is_it || exit 0; sleep 0.1; done
        kill -KILL \$p && echo \"$3 \$p ignored SIGINT for 10 s: SIGKILL\" >&2" || true
}
# shellcheck disable=SC2329 # invoked by the EXIT trap below
stop_both() {
    stop_remote "$RPI_CLIENT" "$CLIENT_PID_FILE" client
    stop_remote "$RPI_SERVER" "$SERVER_PID_FILE" server
}
trap stop_both EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

ssh_run "$RPI_SERVER" "export LD_LIBRARY_PATH=$LIB_PATH; export CYCLONEDDS_URI='$CDDS_URI'; cd ~/$REMOTE_DIR; nohup $PIN ./server $CLIENT_ARGS > /tmp/cdds_${SCENARIO}_server.log 2>&1 < /dev/null & echo \$! > $SERVER_PID_FILE"
# 5s, not 2s (2026-09-20, real finding): a TRANSIENT_LOCAL reader's own match negotiation against a
# writer carrying a non-trivial durability_service history took meaningfully longer on this rig than
# a plain volatile match (~1.5-2s) - 2s left the client's own 15s match-wait budget too tight often
# enough to matter; 5s was reliable across repeated real runs.
sleep 5
ssh_run "$RPI_CLIENT" "export LD_LIBRARY_PATH=$LIB_PATH; export CYCLONEDDS_URI='$CDDS_URI'; cd ~/$REMOTE_DIR && echo \$\$ > $CLIENT_PID_FILE && exec $PIN ./client $CLIENT_ARGS" | grep '^RESULT:'
# Stop the server first (stop_remote: SIGINT, then wait for it to go), then read the log - not just stop it (2026-09-21,
# real gap found the hard way): this
# script never actually printed the server's own RESULT line at all before this fix - only the
# client's own line ever reached stdout, silently losing every server-side recv/loss number for
# every scenario whose server is the authoritative side (best_effort_throughput,
# reliable_throughput, ...) unless someone happened to read the remote log file by hand
# afterward. Matches examples/perf_hil/tickle/run_scenario.sh's own identical fix.
stop_remote "$RPI_SERVER" "$SERVER_PID_FILE" server
ssh_run "$RPI_SERVER" "cat /tmp/cdds_${SCENARIO}_server.log" | grep '^RESULT:'

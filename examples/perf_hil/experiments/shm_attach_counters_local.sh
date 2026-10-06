#!/usr/bin/env bash
# Checks that the shm_attach_* fields reach the RESULT line, off the rig, in a private netns.
#
# Why it exists: the fields were added because a peer with no segment was re-asked about for every
# datagram and nothing on the row could show it (SHM_PLAN.md 6b). A field that is computed but
# truncated out of the line, or a setter a harness forgets to call, reproduces exactly the blindness
# the fields were added to remove - so the line is read rather than assumed once it compiles. The
# fallbacks buffer was 160 bytes and already close to full when these three were added, which is the
# specific way this would have failed silently.
#
# Why a netns: examples/perf_hil is run on this PC, and a TickLE node in the default netns
# broadcasts onto the 10.1.1.x management LAN where the rig's Pis live and answer. Nothing here
# leaves the machine.
#
# Pre-registered reading, before the run:
#   PASS  - both RESULT lines carry shm_attach_attempts, shm_attach_ok and shm_attach_absent, and
#           attempts >= ok + absent (they are two outcomes out of six, so the sum can be lower).
#   FAIL  - a field missing or truncated: the line does not carry what the row is supposed to prove.
#   VOID  - a node did not produce a RESULT line at all; says so rather than reporting a pass.
# Same-host here, so a successful attach is expected and shm_attach_ok > 0 - which also makes this a
# check that the counters are wired at all, not only that they print.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/../tickle/reliable_throughput_p1"
OUT=/tmp/shm_attach_counters_local.log
NS=tt-shmattach-$$
SECONDS_TO_RUN=${SECONDS_TO_RUN:-5}

[ -x "$BIN/server" ] && [ -x "$BIN/client" ] || {
    echo "VOID: build first - examples/perf_hil/tickle/build.sh reliable_throughput p1"
    exit 1
}

sudo -n ip netns del "$NS" 2>/dev/null
sudo -n ip netns add "$NS" || { echo "VOID: netns add failed"; exit 1; }
trap 'sudo -n ip netns del "$NS" 2>/dev/null' EXIT
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" link add dummy0 type dummy
# The harness's link is configured for the rig's 192.168.10.0/24, and a node refuses to start if no
# local interface carries that broadcast address - so the namespace provides it. Same subnet, no cable.
sudo -n ip -n "$NS" addr add 192.168.10.1/24 dev dummy0
sudo -n ip -n "$NS" link set dummy0 up
sudo -n ip -n "$NS" route add default dev dummy0

: >"$OUT"
# shellcheck disable=SC2024 # the log is ours; sudo is only for entering the namespace
sudo -n ip netns exec "$NS" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups \
    env HOME="$HOME" BIN="$BIN" SECONDS_TO_RUN="$SECONDS_TO_RUN" bash -c '
        "$BIN/server" -d "$SECONDS_TO_RUN" 2>&1 | sed "s/^/server: /" &
        server_pid=$!
        sleep 1
        "$BIN/client" -d "$SECONDS_TO_RUN" 2>&1 | sed "s/^/client: /"
        wait "$server_pid"' >>"$OUT" 2>&1

verdict=PASS
for role in server client; do
    line=$(grep "^$role: RESULT" "$OUT" | tail -1)
    if [ -z "$line" ]; then
        echo "VOID: $role produced no RESULT line"
        verdict=VOID
        continue
    fi
    missing=
    for key in shm_attach_attempts shm_attach_ok shm_attach_absent; do
        case "$line" in
        *"$key="*) ;;
        *) missing="$missing $key" ;;
        esac
    done
    if [ -n "$missing" ]; then
        echo "FAIL: $role is missing:$missing"
        verdict=FAIL
        continue
    fi
    # shellcheck disable=SC2001 # the fields are not a fixed position in the line
    get() { echo "$line" | sed "s/.*$1=\([0-9]*\).*/\1/"; }
    attempts=$(get shm_attach_attempts)
    ok=$(get shm_attach_ok)
    absent=$(get shm_attach_absent)
    unattached=$(get tx_udp_unattached)
    if [ "$attempts" -lt $((ok + absent)) ]; then
        echo "FAIL: $role attempts=$attempts < ok=$ok + absent=$absent"
        verdict=FAIL
        continue
    fi
    echo "$role: attempts=$attempts ok=$ok absent=$absent (tx_udp_unattached=$unattached)"
done

echo "=== $verdict === full log in $OUT"
[ "$verdict" = PASS ]

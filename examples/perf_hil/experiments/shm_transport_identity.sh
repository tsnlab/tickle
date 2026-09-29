#!/usr/bin/env bash
# shm_transport_identity.sh - SHM_PLAN.md's S2: which transport actually carried the samples, asserted per payload shape.
#
# Why this exists before the segment does. If shared memory silently falls back to loopback UDP, every functional test
# still passes and proves nothing about the module - so the transport has to be asserted, not assumed, and the assertion
# has to be per shape. A seam that wired tt_send while leaving the iov and batch paths on UDP would report "shm" for a
# small sample while fragmented ones went over loopback, which is why one number for the whole run is not enough.
#
# Two shapes, both same-host (two processes, one network namespace - which is what the module is for):
#   p1  64 B    one datagram
#   p4  2788 B  more than one datagram, so DATA_FRAG and the batch send path (TICKLE_P4_PATH=frag)
# The mapping from a shape to a send function is tests/test_transport_seam.c's business at unit level; this script's
# business is end to end.
#
# HOW TO READ IT, written before the run:
#   EXPECT=udp (stage 0, today - no segment exists):
#     - PASS needs tx_shm == 0 and tx_udp > 0 on both shapes. That is a real assertion about the seam being wired
#       without changing behaviour, and it is what proves the counters work BEFORE anything depends on them.
#   EXPECT=shm (stage 1 onwards):
#     - PASS needs tx_shm > 0 AND every remaining UDP datagram accounted for by a named reason:
#       tx_udp == tx_udp_broadcast + tx_udp_oversize + tx_udp_unattached + tx_udp_full.
#     - NOT tx_udp == 0, which is unachievable and was wrong in the first version of this script. A segment carries
#       unicast datagrams to a known peer: the name is computed from that peer's (address, port, context id), so a
#       broadcast destination has no name to compute and cannot go over a segment even in principle. Announces and
#       summaries are broadcast, so a running context always has some tx_udp (found by Dev, 2026-09-29, before the first
#       run rather than in it).
#     - The repair is an exact assertion rather than a weaker one. "tx_udp small and flat" would need a threshold
#       nobody can derive; "every UDP datagram has a named reason" needs none, and it is strictly stronger - it fails on
#       an UNEXPLAINED fallback, which is the most valuable failure this script could report and the one a threshold
#       would hide. A shape that did not go over the segment then shows up as a large unexplained remainder, which is
#       the partial-seam signature in its exact form.
#     - If the RESULT line carries no by-reason counters, the shape is VOID and says so: the assertion cannot be made,
#       and a pass that skipped it would be the thing this whole script exists to prevent.
#   Always, whatever EXPECT is:
#     - the external control: tx_udp against wire_tx_packets, the kernel's own /proc/net/dev count for the
#       interface - not the sum, because a datagram carried by the segment never reaches the interface, so tx_shm is
#       confirmed by its ABSENCE there. They agreed within 1 packet on the first run where the data direction crossed
#       (tx_udp=4547, wire_tx_packets=4548, tx_shm=98303). A drift beyond the tolerance below means a seam
#       counter is wrong, and the run says so rather than reporting a transport verdict from a broken instrument.
#     - a run whose sent= is below MIN_SENT is VOID: too few samples to have exercised the shape.
#   And the control that this script can fail at all: run it with EXPECT=shm today. It must FAIL on both shapes, because
#   no segment exists. A script that passes both expectations is asserting nothing.
# Usage: shm_transport_identity.sh            assert stage 0 (EXPECT=udp)
#        EXPECT=shm shm_transport_identity.sh assert the segment carries it (stage 1+), and today the control arm
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
EXPECT=${EXPECT:-udp}
SHAPES=${SHAPES:-"p1 p4"}
DUR=${DUR:-4}
MIN_SENT=${MIN_SENT:-1000}
# The interface count includes ARP and whatever leaves between the seam's last increment and the counter read, so the
# comparison is a tolerance rather than an equality. 0.5% or 50 packets, whichever is larger.
TOL_PCT=${TOL_PCT:-0.5}
TOL_ABS=${TOL_ABS:-50}
OUT=${OUT:-/tmp/shm_transport_identity.txt}
NS1=shmida$$; NS2=shmidb$$
IF1=shmid1; IF2=shmid2
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
fails=0

# shellcheck disable=SC2329 # invoked by the trap below, not by name
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
}
trap cleanup EXIT

say "=== S2 transport identity $(date -Is) EXPECT=$EXPECT shapes='$SHAPES' ==="

# TWO namespaces joined by a veth, one process in each - and they are still the SAME HOST for the module's purposes,
# because a network namespace isolates the network and not /dev/shm, which is what g8's host registry and a segment both
# live in. (An assumption to re-verify at stage 1 rather than to trust: if the segment ever keys on something netns does
# isolate, this environment stops representing a same-host pair and the script has to change.)
#
# Why not one namespace with both addresses in it, which is the more obvious reading of "same host": the external control
# then cannot work, and the control is what showed it. (The figures in this paragraph are from stage 0, when tx_shm was
# structurally zero and the control compared the SUM - they are a record of how this environment was chosen, not of the
# rule above, which now compares tx_udp alone.) With both addresses local the kernel routes .1 -> .2 through
# loopback and the veth never sees the traffic (first run: tx_udp+tx_shm=561119 against wire_tx_packets=28305 on the
# veth); pointing BENCH_IFACE at lo instead swapped the problem for another, because lo carries BOTH processes' datagrams
# and one process's counter cannot be compared against a shared interface (second run: 300907 against 405305). One
# process per interface restores the 1:1 comparison that agreed within 2 packets on a 947k-datagram run.
for ns in "$NS1" "$NS2"; do
    sudo -n ip netns add "$ns" || { say "FATAL cannot create namespace $ns"; exit 1; }
done
sudo -n ip link add "$IF1" type veth peer name "$IF2" || { say "FATAL cannot create the veth pair"; exit 1; }
sudo -n ip link set "$IF1" netns "$NS1"
sudo -n ip link set "$IF2" netns "$NS2"
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev "$IF1"
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev "$IF2"
sudo -n ip -n "$NS1" link set lo up
sudo -n ip -n "$NS2" link set lo up
sudo -n ip -n "$NS1" link set "$IF1" up
sudo -n ip -n "$NS2" link set "$IF2" up

PIDFILE=/tmp/shmid_srv.pid
# Precomputed rather than $(id -u) inline: the ids go into a command string, and an unquoted substitution there is a
# word-splitting finding as well as a habit worth not having.
OUR_UID=$(id -u)
OUR_GID=$(id -g)
srv_pid=""

kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    case "$(readlink "/proc/$srv_pid/exe" 2>/dev/null)" in
        # Plain kill, no sudo: the server runs as this user (setpriv, above), and sudo here permits only `ip` - a
        # `sudo kill` is refused outright, which is why the first version's guard correctly reported two servers alive
        # while the kill had silently done nothing.
        */reliable_throughput_*/server | */reliable_throughput_*/"server (deleted)") kill -TERM "$srv_pid" 2>/dev/null ;;
    esac
    for _ in 1 2 3 4 5; do
        [ -d "/proc/$srv_pid" ] || break
        sleep 1
    done
    srv_pid=""
}

# How many of our own harness servers are alive, by /proc/PID/exe rather than by a pattern - so an accumulation says so
# at once instead of arriving later as a limping run.
# Readable because the harness processes run as THIS user inside the namespace, not as root: `sudo ip netns exec` enters
# the namespace and `setpriv` immediately drops back to our own uid. The first version left them as root, and readlink on
# another user's /proc/PID/exe is refused - which is not "no server" but "cannot see", and the guard then voided every
# shape while a server was running. A check that cannot see its subject answers about itself. Running them as root and
# checking under sudo is not available here: this machine's sudoers permits `ip` and nothing else, and `sudo sh -c` was
# refused outright.
count_our_servers() {
    local n=0 exe
    for proc in /proc/[0-9]*; do
        exe=$(readlink "$proc/exe" 2>/dev/null) || continue
        case "$exe" in */reliable_throughput_*/server*) n=$((n + 1)) ;; esac
    done
    echo "$n"
}

field() { echo "$1" | grep -oE "(^| )$2=[0-9]+" | tail -1 | cut -d= -f2; }

for shape in $SHAPES; do
    d="$REPO/examples/perf_hil/tickle/reliable_throughput_$shape"
    (cd "$REPO/examples/perf_hil/tickle" && TICKLE_P4_PATH=frag ./build.sh reliable_throughput "$shape" >/dev/null 2>&1) \
        || { say "$shape: FAIL(build)"; fails=$((fails + 1)); continue; }
    # shellcheck disable=SC2024 # the redirect is this user's, which is what is wanted: the log is ours, not root's
    kill_server
    # The PID the server's own launch writes, killed through /proc/PID/exe and never by a pattern (CLAUDE.md rule 2).
    # Without this the previous shape's server is still alive when the next shape starts - p1's server outlives p1's
    # client by DUR+8 seconds - and p4's client then meets a server publishing a different payload size on the same
    # topic. It does not fail; it limps: 192 samples in four seconds against 436,687 when the shape is run alone, which
    # the MIN_SENT guard reported as VOID rather than as a wrong number. Same defect as p1_client_syscalls.sh's, written
    # a second time by the same person, which is why the guard below asserts the count rather than trusting the kill.
    # shellcheck disable=SC2024 # the redirect is this user's, which is what is wanted: the log is ours, not root's
    (sudo -n ip netns exec "$NS2" setsid setpriv --reuid="$OUR_UID" --regid="$OUR_GID" --clear-groups sh -c "echo \$\$ > $PIDFILE; exec env BENCH_IFACE=$IF2 $d/server -Q -d $((DUR + 8))" >"/tmp/shmid_srv_$shape.log" 2>&1 &)
    sleep 2
    srv_pid=$(cat "$PIDFILE" 2>/dev/null)
    alive=$(count_our_servers)
    if [ "${alive:-0}" != 1 ]; then
        say "$shape: VOID($alive of our servers alive, expected 1 - a previous shape's server survived)"
        fails=$((fails + 1)); continue
    fi
    # The client's output is kept, not only its RESULT line. The publisher lives on this side, so
    # its diagnostics - "Publisher peer registered" above all - are what distinguish "the segment
    # was not attached" from "this datagram was never a segment candidate", and piping straight into
    # grep threw away the half of the evidence the by-reason counters point at.
    # shellcheck disable=SC2024 # the redirect is this user's, which is what is wanted: the log is ours, not root's
    sudo -n ip netns exec "$NS1" setpriv --reuid="$OUR_UID" --regid="$OUR_GID" --clear-groups env \
        BENCH_IFACE="$IF1" "$d/client" -Q -d "$DUR" >"/tmp/shmid_cli_$shape.log" 2>&1
    line=$(grep '^RESULT' "/tmp/shmid_cli_$shape.log")
    sent=$(field "$line" sent)
    tx_udp=$(field "$line" tx_udp); tx_shm=$(field "$line" tx_shm)
    wire=$(field "$line" wire_tx_packets)
    path=$(echo "$line" | grep -oE 'sample_path=[a-z]+' | cut -d= -f2)
    if [ -z "${sent:-}" ] || [ "${sent:-0}" -lt "$MIN_SENT" ]; then
        say "$shape: VOID(sent=${sent:-none} below $MIN_SENT)"
        fails=$((fails + 1)); continue
    fi
    if [ -z "${tx_udp:-}" ] || [ -z "${tx_shm:-}" ]; then
        say "$shape: VOID(no tx_udp/tx_shm in the RESULT line - the harness did not set them)"
        fails=$((fails + 1)); continue
    fi
    # The external control first: a transport verdict from a broken counter is worse than no verdict.
    #
    # Against tx_udp, NOT tx_udp+tx_shm. wire_tx_packets is the kernel's count for the interface, and
    # a datagram that went over the segment never reaches it - that is the whole point of the
    # module. Comparing the sum was right while tx_shm was structurally zero and becomes an
    # automatic failure the moment the transport works: the first run with the data direction
    # crossing reported drift 98302 while tx_udp=4547 matched wire_tx_packets=4548 exactly.
    #
    # So this stays a real external control rather than becoming a tautology: it says the datagrams
    # we claim went over UDP are the ones the interface saw, and tx_shm is confirmed by their
    # ABSENCE from it - which is a stronger statement than the sum ever made.
    tol=$(python3 -c "print(max($TOL_ABS, $tx_udp * $TOL_PCT / 100.0))")
    drift=$(python3 -c "print(abs($tx_udp - ${wire:-0}))")
    if python3 -c "import sys; sys.exit(0 if $drift > $tol else 1)"; then
        say "$shape: FAIL(counter drift: tx_udp=$tx_udp against wire_tx_packets=${wire:-none}, drift $drift > tol $tol)"
        fails=$((fails + 1)); continue
    fi
    case "$EXPECT" in
        udp)
            if [ "$tx_shm" = 0 ] && [ "$tx_udp" -gt 0 ]; then
                say "$shape: PASS(sample_path=$path sent=$sent tx_udp=$tx_udp tx_shm=0 wire=$wire drift=$drift)"
            else
                say "$shape: FAIL(expected all udp: tx_udp=$tx_udp tx_shm=$tx_shm)"
                fails=$((fails + 1))
            fi
            ;;
        shm)
            named=0; missing=0
            # The RESULT-line names, not the core field names (segment_*_to_udp). They differ on purpose - see
            # BenchStats.h's bench_stats_set_fallbacks() - and the first version of this loop looked for the C names,
            # so every shape voided with "no by-reason counters" while the line carried all four.
            for reason in tx_udp_broadcast tx_udp_oversize tx_udp_unattached tx_udp_full; do
                reason_count=$(field "$line" "$reason")
                if [ -z "${reason_count:-}" ]; then missing=1; else named=$((named + reason_count)); fi
            done
            if [ "$missing" = 1 ]; then
                say "$shape: VOID(the RESULT line carries no by-reason fallback counters - the assertion cannot be made)"
                fails=$((fails + 1))
            elif [ "$tx_shm" -le 0 ]; then
                say "$shape: FAIL(nothing went over the segment: tx_shm=0 tx_udp=$tx_udp)"
                fails=$((fails + 1))
            elif [ "$tx_udp" != "$named" ]; then
                say "$shape: FAIL(unexplained fallback: tx_udp=$tx_udp against $named accounted for by named reasons)"
                fails=$((fails + 1))
            else
                say "$shape: PASS(sample_path=$path sent=$sent tx_shm=$tx_shm tx_udp=$tx_udp all named)"
            fi
            ;;
        *) say "FATAL EXPECT must be udp or shm"; exit 1 ;;
    esac
done

kill_server
say "=== $( [ "$fails" = 0 ] && echo "ALL PASS" || echo "$fails shape(s) failed or void" ) ==="
exit "$fails"

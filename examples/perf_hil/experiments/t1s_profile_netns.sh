#!/usr/bin/env bash
# t1s_profile_netns.sh - the T1S link profile (docs/DESIGN.md "Link profiles") taking effect between two nodes in
# private network namespaces over a veth shaped like a 10BASE-T1S bus: tc tbf at 10 Mbit/s and an interface MTU of
# 1000 on both ends. 2026-10-22, for network-profiles phase 0 items B and G. No rig, no default namespace: every node
# runs under `sudo -n ip netns exec`, and the namespaces are removed by the one EXIT trap.
#
# Three arms, each a Publisher node and a Subscriber node (t1s_profile_node.c, built here against this tree's
# libtickle.a), the Publisher sending 20 best-effort samples of 100 bytes every 10 ms for SECONDS to its one matched
# reader:
#   ethernet        the default profile: no batching, unicast to one peer, datagrams cut for 1500;
#   ethernet-batch  ETHERNET with the Publisher batching: the batch is cut for 1500 on a 1000-byte link;
#   t1s             the T1S profile: batching, broadcast, cut for the interface's 1000.
# Evidence, all from counters - the nodes' own RESULT lines (what the link resolved to, the datagrams each sent and
# received) and the sender namespace's kernel counters (/proc/net/snmp Ip FragCreates, /proc/net/netstat IpExt
# OutBcastPkts, the veth's tx_packets and tx_bytes). No packet capture.
#
# PRE-REGISTERED READING (the checks at the end, in code):
#   VOID  an arm whose Publisher never matched, published nothing, or whose Subscriber received under 95% of it.
#   t1s   link_mtu 1000 (the interface's), control_max 972, unicast_threshold 0, the Publisher batching; the
#         namespace fragmented nothing (FragCreates 0) - every datagram fit the link; it sent fewer than a third of
#         ethernet's datagrams (batching); its broadcasts exceed ethernet's by at least half its datagrams (broadcast,
#         not unicast to the one peer).
#   ethernet        link_mtu 1500, control_max 1472, threshold 2, no batching, FragCreates 0 (100-byte datagrams).
#   ethernet-batch  FragCreates > 0: the same batching cut for 1500 is IP-fragmented on this link - what t1s's cut
#                   avoids. Were it 0, the t1s FragCreates 0 would not show the MTU doing anything.
#   PASS when every arm is valid and every line above holds; FAIL otherwise, naming the line.
#
# First run (2026-10-22, ~/rig_results_safe/t1s_profile_netns_20261022-143124, tree 6ea67656 before its rebase
# + the nodes' shared
# endpoint name): PASS. 6000 samples each arm, received 6000 / 5988 / 6000. ethernet 6007 datagrams, 5 broadcasts,
# FragCreates 0; ethernet-batch 607 datagrams, FragCreates 600; t1s link_mtu 1000, control_max 972, 907 datagrams
# (762,484 bytes on the veth, 840 a packet), 908 broadcasts, FragCreates 0. Re-run on the restructured node
# (t1s_profile_netns_20261022-145449): PASS, the same counts within 100 bytes.
#
# Usage: [SECONDS=3] [OUT=prefix] t1s_profile_netns.sh   OUT defaults to ~/rig_results_safe/t1s_profile_netns_<stamp>
#   ($OUT.txt). Needs `sudo -n ip` (tc runs under `ip netns exec`). The summary always ends with
#   "=== t1s_profile_netns ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SECONDS_RUN=${SECONDS_RUN:-3}
OUT=${OUT:-$HOME/rig_results_safe/t1s_profile_netns_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$(dirname "$OUT")" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
WORK=$(mktemp -d "$HOME/tmp_build/t1s_profile_netns.XXXXXX" 2>/dev/null || mktemp -d /tmp/t1s_profile_netns.XXXXXX) || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
on_exit() {
    local rc=$? ns
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== t1s_profile_netns ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
say "=== t1s_profile_netns $(date -Is): SECONDS_RUN=$SECONDS_RUN tree=$(git -C "$REPO" rev-parse --short HEAD)$(git -C "$REPO" diff --quiet HEAD -- src include || echo +dirty) ==="

# The library and the node, from this tree.
make -C "$REPO/platform/linux" library >"$WORK/build.log" 2>&1 || {
    say "VOID  library did not build"
    tail -5 "$WORK/build.log"
    exit 1
}
gcc -O2 -Wall -Wextra -I"$REPO/include" -o "$WORK/node" "$HERE/t1s_profile_node.c" -L"$REPO/platform/linux" -ltickle \
    -lm -lpthread >>"$WORK/build.log" 2>&1 || {
    say "VOID  node did not build"
    tail -5 "$WORK/build.log"
    exit 1
}

# counter NS FILE TABLE FIELD: one kernel counter of a namespace, from /proc/net/snmp or /proc/net/netstat.
counter() {
    sudo -n ip netns exec "$1" cat "/proc/net/$2" | awk -v t="$3:" -v f="$4" '
        $1 == t && !seen { for (i = 2; i <= NF; i++) col[$i] = i; seen = 1; next }
        $1 == t && seen { print $(col[f]); exit }'
}
# vstat NS DEV FIELD: a veth statistic.
vstat() { sudo -n ip netns exec "$1" cat "/sys/class/net/$2/statistics/$3"; }

declare -A R
run_arm() { # $1 arm
    local arm=$1 id=$$ na nb va vb
    na="t1sp$id-$arm-a"
    nb="t1sp$id-$arm-b"
    va="t1s${id}a${#arm}"
    vb="t1s${id}b${#arm}"
    echo "$na" >>"$NS_LIST"
    echo "$nb" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$na" && sudo -n ip netns add "$nb" && sudo -n ip link add "$va" type veth peer name "$vb" &&
        sudo -n ip link set "$va" netns "$na" && sudo -n ip link set "$vb" netns "$nb" &&
        sudo -n ip -n "$na" link set "$va" mtu 1000 && sudo -n ip -n "$nb" link set "$vb" mtu 1000 &&
        sudo -n ip -n "$na" addr add 10.77.0.1/24 brd + dev "$va" && sudo -n ip -n "$nb" addr add 10.77.0.2/24 brd + dev "$vb" &&
        sudo -n ip -n "$na" link set lo up && sudo -n ip -n "$nb" link set lo up &&
        sudo -n ip -n "$na" link set "$va" up && sudo -n ip -n "$nb" link set "$vb" up &&
        sudo -n ip netns exec "$na" tc qdisc add dev "$va" root tbf rate 10mbit burst 16kb latency 100ms &&
        sudo -n ip netns exec "$nb" tc qdisc add dev "$vb" root tbf rate 10mbit burst 16kb latency 100ms; }; then
        say "VOID  $arm: no namespaces/veth"
        return 1
    fi
    local frag0 bcast0 pkts0 bytes0
    frag0=$(counter "$na" snmp Ip FragCreates)
    bcast0=$(counter "$na" netstat IpExt OutBcastPkts)
    pkts0=$(vstat "$na" "$va" tx_packets)
    bytes0=$(vstat "$na" "$va" tx_bytes)
    # shellcheck disable=SC2024 # the logs are this user's, written by this shell
    sudo -n ip netns exec "$nb" "$WORK/node" sub "$arm" 2 10.77.0.255 $((SECONDS_RUN + 4)) >"$WORK/$arm.sub.log" 2>&1 &
    local sp=$!
    sleep 0.5
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$na" "$WORK/node" pub "$arm" 1 10.77.0.255 "$SECONDS_RUN" >"$WORK/$arm.pub.log" 2>&1
    wait "$sp"
    R[$arm.frag]=$(($(counter "$na" snmp Ip FragCreates) - frag0))
    R[$arm.bcast]=$(($(counter "$na" netstat IpExt OutBcastPkts) - bcast0))
    R[$arm.pkts]=$(($(vstat "$na" "$va" tx_packets) - pkts0))
    R[$arm.bytes]=$(($(vstat "$na" "$va" tx_bytes) - bytes0))
    sudo -n ip netns del "$na"
    sudo -n ip netns del "$nb"
    local pub sub
    pub=$(grep '^RESULT role=pub' "$WORK/$arm.pub.log")
    sub=$(grep '^RESULT role=sub' "$WORK/$arm.sub.log")
    say "$arm pub: ${pub:-<no RESULT line>}"
    say "$arm sub: ${sub:-<no RESULT line>}"
    say "$arm kernel (sender namespace): FragCreates=${R[$arm.frag]} OutBcastPkts=${R[$arm.bcast]} veth tx_packets=${R[$arm.pkts]} tx_bytes=${R[$arm.bytes]}"
    local k
    for k in link_mtu control_max unicast_threshold pub_batch matched published tx_datagrams; do
        R[$arm.$k]=$(grep -o "$k=[0-9-]*" <<<"$pub" | cut -d= -f2)
    done
    R[$arm.received]=$(grep -o 'received=[0-9]*' <<<"$sub" | cut -d= -f2)
    [ -n "$pub" ] && [ -n "$sub" ]
}

verdict=PASS
check() { # description condition-status
    if [ "$2" = 0 ]; then
        say "  ok    $1"
    else
        say "  FAIL  $1"
        verdict=FAIL
    fi
}
for arm in ethernet ethernet-batch t1s; do
    run_arm "$arm" || verdict=VOID
done

say "--- reading"
for arm in ethernet ethernet-batch t1s; do
    m=${R[$arm.matched]:-0} p=${R[$arm.published]:-0} r=${R[$arm.received]:-0}
    if [ "$m" = 0 ] || [ "$p" = 0 ] || [ $((r * 100)) -lt $((p * 95)) ]; then
        say "  VOID  $arm: matched=$m published=$p received=$r"
        verdict=VOID
    else
        say "  valid $arm: matched=$m published=$p received=$r ($((r * 100 / p))%)"
    fi
done
if [ "$verdict" != VOID ]; then
    e=ethernet b=ethernet-batch t=t1s
    [ "${R[$t.link_mtu]}" = 1000 ]; check "t1s link_mtu 1000, the interface's (got ${R[$t.link_mtu]})" $?
    [ "${R[$t.control_max]}" = 972 ]; check "t1s control_max 972 (got ${R[$t.control_max]})" $?
    [ "${R[$t.unicast_threshold]}" = 0 ]; check "t1s unicast_threshold 0: always broadcast (got ${R[$t.unicast_threshold]})" $?
    [ "${R[$t.pub_batch]}" = 1 ]; check "t1s Publisher batches (got ${R[$t.pub_batch]})" $?
    [ "${R[$t.frag]}" = 0 ]; check "t1s namespace fragmented nothing (FragCreates ${R[$t.frag]})" $?
    [ $((R[$t.tx_datagrams] * 3)) -lt "${R[$e.tx_datagrams]}" ]
    check "t1s sent under a third of ethernet's datagrams (${R[$t.tx_datagrams]} vs ${R[$e.tx_datagrams]})" $?
    [ "${R[$t.bcast]}" -ge $((R[$e.bcast] + R[$t.tx_datagrams] / 2)) ]
    check "t1s broadcasts its data (OutBcastPkts ${R[$t.bcast]} vs ethernet ${R[$e.bcast]})" $?
    [ "${R[$e.link_mtu]}" = 1500 ] && [ "${R[$e.control_max]}" = 1472 ] && [ "${R[$e.unicast_threshold]}" = 2 ] &&
        [ "${R[$e.pub_batch]}" = 0 ]
    check "ethernet keeps 1500 / 1472 / threshold 2 / no batching" $?
    [ "${R[$e.frag]}" = 0 ]; check "ethernet fragmented nothing (FragCreates ${R[$e.frag]})" $?
    [ "${R[$b.frag]}" -gt 0 ]; check "ethernet-batch's 1500 cut is IP-fragmented on this link (FragCreates ${R[$b.frag]})" $?
fi
say "VERDICT=$verdict"
[ "$verdict" = PASS ]

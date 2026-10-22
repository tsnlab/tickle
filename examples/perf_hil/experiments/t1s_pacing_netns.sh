#!/usr/bin/env bash
# t1s_pacing_netns.sh - send pacing (docs/DESIGN.md "Send pacing") on a T1S link, between two nodes in private network
# namespaces over a veth shaped like a 10BASE-T1S bus: tc tbf at 10 Mbit/s (burst 16 kB, latency 100 ms - a queue of
# about 141 kB) on both ends. 2026-10-22, for network-profiles phase 0 item F; the namespaces, the shaper and the counters
# are t1s_profile_netns.sh's (item B), with the nodes built as rmw_tickle builds core (large samples). No rig, no default
# namespace: every node runs under `sudo -n ip netns exec`, and the namespaces are removed by the one EXIT trap.
#
# Four arms, each a Publisher node and a Subscriber node (t1s_pacing_node.c) on a T1S link for SECONDS_RUN:
#   off      pacing off (pacing_rate tt_PACING_OFF): a small stream (100 B every 10 ms, best effort) and a large one
#            (LARGE bytes, default 256 kB, RELIABLE, every 500 ms: 4.2 Mbit/s, under the shaper's 10);
#   on       the same with the T1S profile's pacing: 75% of 10 Mbit/s, a burst of 4 frames;
#   ctl-off  the small stream alone, pacing off - the control: nothing here fills a bucket, so pacing cannot change it;
#   ctl-on   the small stream alone, pacing on.
# Evidence, all from counters: the nodes' RESULT lines (the pacing rate and burst each applied, samples published and
# received, the small stream's one-way latency p50/p99/max against the host's one clock, pacing_waits, repairs) and the
# sender namespace's tbf statistics (`tc -s qdisc`: dropped at the end, backlog sampled every 50 ms). No packet capture.
#
# PRE-REGISTERED READING (the checks at the end, in code):
#   VOID  an arm without both RESULT lines; a Publisher not matched; a small stream published under 95% of its
#         SECONDS_RUN x 100 or received under MIN_SMALL (1000: p99 is then at least the 10th-worst sample); an arm not
#         given its treatment (on/ctl-on pacing_rate 7500000, off/ctl-off 0); the off arm not overloading the shaper
#         (no drop and a backlog under 64 kB - nothing for pacing to show); ctl-off dropping (the shaper drops
#         without the large stream - the setup is wrong).
#   PASS  when every arm is valid and every line holds:
#         on paces: pacing_waits > 0 (off: 0);
#         drops fall by ten times: dropped(on) <= dropped(off) / 10;
#         the backlog halves at least: max backlog(on) <= max backlog(off) / 2;
#         the small stream's p99 halves at least: p99(on) <= p99(off) / 2;
#         the control holds: |p99(ctl-on) - p99(ctl-off)| <= max(2 ms, 25% of ctl-off);
#         the large stream gets through: received(on) >= 90% of published(on), and >= received(off).
#   FAIL otherwise, naming the line.
#
# Runs (2026-10-22, tree 25003126, ~/rig_results_safe/t1s_pacing_netns_20261022-run*): run1 FAIL on the harness - the
# backlog was read as "84K", text, not bytes (qstat() now converts) - its other lines held. run2 PASS: off dropped
# 2,528 (backlog to 139,964 B), resent 5,047, small p99 108,302 us; on 0 dropped, 0 B queued, 0 resent, 6,211 waits,
# p99 214 us; 30 of 30 large samples in both; control p99 442 / 160 us. run3 PASS: off 2,496 dropped, p99 108,290 us;
# on 0, p99 675 us; control 213 / 601 us - the control's two arms swap order between runs: the 1 ms batch tick's phase.
# run4 (tree f0d1ce66, the node's date from tt_get_wall_ns()) PASS: off 2,505 dropped, backlog 140,992 B, p99 108,301 us;
# on 0, 0 B, p99 285 us, 6,210 waits; control 279 / 651 us.
#
# Usage: [SECONDS_RUN=15] [LARGE=262144] [OUT=prefix] t1s_pacing_netns.sh   OUT defaults to
#   ~/rig_results_safe/t1s_pacing_netns_<stamp> ($OUT.txt). Needs `sudo -n ip` (tc runs under `ip netns exec`). The
#   summary always ends with "=== t1s_pacing_netns ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SECONDS_RUN=${SECONDS_RUN:-15}
LARGE=${LARGE:-262144}
MIN_SMALL=1000
OUT=${OUT:-$HOME/rig_results_safe/t1s_pacing_netns_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$(dirname "$OUT")" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
WORK=$(mktemp -d "$HOME/tmp_build/t1s_pacing_netns.XXXXXX" 2>/dev/null || mktemp -d /tmp/t1s_pacing_netns.XXXXXX) || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
on_exit() {
    local rc=$? ns
    [ -n "${MON:-}" ] && kill "$MON" 2>/dev/null
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== t1s_pacing_netns ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
say "=== t1s_pacing_netns $(date -Is): SECONDS_RUN=$SECONDS_RUN LARGE=$LARGE tree=$(git -C "$REPO" rev-parse --short HEAD)$(git -C "$REPO" diff --quiet HEAD -- src include || echo +dirty) ==="

# The node, from this tree's sources, as rmw_tickle builds core.
nice -n 10 gcc -O2 -Wall -Wextra -I"$REPO/include" -I"$REPO/src" -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 \
    -o "$WORK/node" "$HERE/t1s_pacing_node.c" "$REPO/src/tickle.c" "$REPO/src/hal_linux.c" "$REPO/src/encoding.c" \
    "$REPO/src/log.c" -lm -lpthread >"$WORK/build.log" 2>&1 || {
    say "VOID  node did not build"
    tail -5 "$WORK/build.log"
    exit 1
}

# qstat NS DEV: "dropped backlog_bytes" of the device's root qdisc. tc prints the backlog as 123b, 84Kb or 1Mb (units
# of 1024; the first run of this harness read "84K" as text, 2026-10-22).
qstat() {
    sudo -n ip netns exec "$1" tc -s qdisc show dev "$2" | awk '
        /dropped/ { for (i = 1; i <= NF; i++) if ($i == "(dropped") { d = $(i + 1); sub(",", "", d) } }
        /backlog/ { for (i = 1; i <= NF; i++) if ($i == "backlog") {
            b = $(i + 1); m = 1
            if (b ~ /Kb$/) m = 1024; else if (b ~ /Mb$/) m = 1048576
            sub(/[KM]?b$/, "", b); b = b * m } }
        END { print (d == "" ? -1 : d), (b == "" ? -1 : b) }'
}

declare -A R
run_arm() { # $1 arm
    local arm=$1 id=$$ na nb va vb paced large
    na="t1spc$id-$arm-a"
    nb="t1spc$id-$arm-b"
    va="t1p${id}a${#arm}"
    vb="t1p${id}b${#arm}"
    paced=off
    case $arm in on | ctl-on) paced=on ;; esac
    large=$LARGE
    case $arm in ctl-*) large=0 ;; esac
    echo "$na" >>"$NS_LIST"
    echo "$nb" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$na" && sudo -n ip netns add "$nb" && sudo -n ip link add "$va" type veth peer name "$vb" &&
        sudo -n ip link set "$va" netns "$na" && sudo -n ip link set "$vb" netns "$nb" &&
        sudo -n ip -n "$na" addr add 10.78.0.1/24 brd + dev "$va" && sudo -n ip -n "$nb" addr add 10.78.0.2/24 brd + dev "$vb" &&
        sudo -n ip -n "$na" link set lo up && sudo -n ip -n "$nb" link set lo up &&
        sudo -n ip -n "$na" link set "$va" up && sudo -n ip -n "$nb" link set "$vb" up &&
        sudo -n ip netns exec "$na" tc qdisc add dev "$va" root tbf rate 10mbit burst 16kb latency 100ms &&
        sudo -n ip netns exec "$nb" tc qdisc add dev "$vb" root tbf rate 10mbit burst 16kb latency 100ms; }; then
        say "VOID  $arm: no namespaces/veth"
        return 1
    fi
    # shellcheck disable=SC2024 # the logs are this user's, written by this shell
    sudo -n ip netns exec "$nb" "$WORK/node" sub "$paced" 2 10.78.0.255 $((SECONDS_RUN + 8)) "$large" >"$WORK/$arm.sub.log" 2>&1 &
    local sp=$!
    sleep 0.5
    # The sender's shaper, every 50 ms while the Publisher runs: its largest backlog.
    (
        while :; do
            qstat "$na" "$va" >>"$WORK/$arm.qstat"
            sleep 0.05
        done
    ) &
    MON=$!
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$na" "$WORK/node" pub "$paced" 1 10.78.0.255 "$SECONDS_RUN" "$large" >"$WORK/$arm.pub.log" 2>&1
    kill "$MON" 2>/dev/null
    wait "$MON" 2>/dev/null
    MON=
    wait "$sp"
    local final
    final=$(qstat "$na" "$va")
    R[$arm.dropped]=${final%% *}
    R[$arm.backlog_max]=$(awk 'BEGIN { m = 0 } $2 > m { m = $2 } END { print m }' "$WORK/$arm.qstat")
    R[$arm.backlog_samples]=$(wc -l <"$WORK/$arm.qstat")
    sudo -n ip netns del "$na"
    sudo -n ip netns del "$nb"
    local pub sub k
    pub=$(grep '^RESULT role=pub' "$WORK/$arm.pub.log")
    sub=$(grep '^RESULT role=sub' "$WORK/$arm.sub.log")
    say "$arm pub: ${pub:-<no RESULT line>}"
    say "$arm sub: ${sub:-<no RESULT line>}"
    say "$arm tbf (sender): dropped=${R[$arm.dropped]} backlog_max=${R[$arm.backlog_max]} B over ${R[$arm.backlog_samples]} samples"
    for k in pacing_rate pacing_burst small_matched large_matched small_published large_published pacing_waits \
        repairs_deferred retransmitted; do
        R[$arm.$k]=$(grep -o " $k=[0-9]*" <<<"$pub" | cut -d= -f2)
    done
    for k in small_received large_received small_p50_us small_p99_us small_max_us; do
        R[$arm.$k]=$(grep -o " $k=[0-9]*" <<<"$sub" | cut -d= -f2)
    done
    [ -n "$pub" ] && [ -n "$sub" ]
}

verdict=PASS
check() { # description command...: the line holds when the command succeeds
    local what=$1
    shift
    if "$@"; then
        say "  ok    $what"
    else
        say "  FAIL  $what"
        verdict=FAIL
    fi
}
void() {
    say "  VOID  $1"
    verdict=VOID
}
ARMS="off on ctl-off ctl-on"
for arm in $ARMS; do
    run_arm "$arm" || verdict=VOID
done

say "--- reading"
for arm in $ARMS; do
    want=$((SECONDS_RUN * 100))
    sp=${R[$arm.small_published]:-0} sr=${R[$arm.small_received]:-0}
    if [ "${R[$arm.small_matched]:-0}" = 0 ]; then void "$arm: small stream not matched"; continue; fi
    case $arm in ctl-*) ;; *) [ "${R[$arm.large_matched]:-0}" = 0 ] && { void "$arm: large stream not matched"; continue; } ;; esac
    [ $((sp * 100)) -lt $((want * 95)) ] && { void "$arm: small published $sp of $want"; continue; }
    [ "$sr" -lt "$MIN_SMALL" ] && { void "$arm: small received $sr, under $MIN_SMALL"; continue; }
    rate=${R[$arm.pacing_rate]:-x}
    case $arm in
    on | ctl-on) [ "$rate" = 7500000 ] || { void "$arm: pacing_rate $rate, not the T1S 7500000"; continue; } ;;
    *) [ "$rate" = 0 ] || { void "$arm: pacing_rate $rate, not off"; continue; } ;;
    esac
    say "  valid $arm: small published=$sp received=$sr p50=${R[$arm.small_p50_us]} us p99=${R[$arm.small_p99_us]} us max=${R[$arm.small_max_us]} us; pacing_rate=$rate burst=${R[$arm.pacing_burst]}"
done
if [ "$verdict" != VOID ]; then
    if [ "${R[off.dropped]}" -le 0 ] && [ "${R[off.backlog_max]}" -lt 65536 ]; then
        void "off did not overload the shaper (dropped ${R[off.dropped]}, backlog_max ${R[off.backlog_max]} B)"
    fi
    if [ "${R[ctl-off.dropped]}" -gt 0 ]; then
        void "ctl-off dropped ${R[ctl-off.dropped]} without the large stream"
    fi
fi
if [ "$verdict" != VOID ]; then
    on_waits=${R[on.pacing_waits]} off_waits=${R[off.pacing_waits]}
    on_drop=${R[on.dropped]} off_drop=${R[off.dropped]}
    on_bl=${R[on.backlog_max]} off_bl=${R[off.backlog_max]}
    on_p99=${R[on.small_p99_us]} off_p99=${R[off.small_p99_us]}
    c_on=${R[ctl-on.small_p99_us]} c_off=${R[ctl-off.small_p99_us]}
    on_lr=${R[on.large_received]} on_lp=${R[on.large_published]} off_lr=${R[off.large_received]}
    off_lp=${R[off.large_published]}
    c_diff=$((c_on > c_off ? c_on - c_off : c_off - c_on))
    c_bound=$((c_off / 4 > 2000 ? c_off / 4 : 2000))
    check "on paces (pacing_waits $on_waits; off $off_waits)" test "$on_waits" -gt 0 -a "$off_waits" -eq 0
    check "tbf drops fall by ten times (on $on_drop vs off $off_drop)" test $((on_drop * 10)) -le "$off_drop"
    check "tbf backlog at least halves (max on $on_bl B vs off $off_bl B)" test $((on_bl * 2)) -le "$off_bl"
    check "small stream p99 at least halves (on $on_p99 us vs off $off_p99 us)" test $((on_p99 * 2)) -le "$off_p99"
    check "control: ctl-on p99 within $c_bound us of ctl-off ($c_on vs $c_off us)" test "$c_diff" -le "$c_bound"
    check "large stream gets through (on $on_lr of $on_lp; off $off_lr of $off_lp)" \
        test $((on_lr * 10)) -ge $((on_lp * 9)) -a "$on_lr" -ge "$off_lr"
fi
say "VERDICT=$verdict"
[ "$verdict" = PASS ]

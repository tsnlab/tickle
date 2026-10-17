#!/usr/bin/env bash
# Does ping_node's warm-up (2026-10-18) remove the start-up loss of the J run at 18f16b85, on this PC, in netns?
#
# BACKGROUND. 10 of 112 rmw_tickle J reps were VOID(loss), sent=95 recv=94: the pong heard the first ping before
# it had matched the ping's subscriber, and its reply, published with no matched reader, sent nothing (4fbfe459:
# dropped when BEST_EFFORT, kept in a history the VOLATILE ping reader is not owed when RELIABLE). Each lossy rep had
# the pong's tx_unmatched counter at 1, every other rep 0 (experiments/rmw_loss_attribution.py).
#
# ARMS, one build, interleaved per rep: "off" (ping_node --no-warmup: seq 1 at the match, as the J run ran) and "on"
# (the default: warm-up pings, seq 0, every 20 ms until one is answered, then seq 1). rmw_tickle, a pong already up
# for PONG_LEAD_S, a veth pair between two private netns (never the default netns), J's flags: -i 0.1, --wait poll
# --poll-sleep-us 50 --phase jitter --rtt-at callback, BEST_EFFORT and RELIABLE alternating. -d is DUR_S (default 2,
# ~20 round trips: the race is at the start, the window length does not enter it).
#
# HOW TO READ IT (enforced at the end, not only stated):
#   race seen     a rep whose pong counted tx_unmatched > 0. In "off" it should equal its loss (the rig's mechanism).
#   missing_seq   the seqs the ping's --stamps never recorded as answered (the rig run kept no per-seq record).
#   PASS          every "on" rep: RESULT present, recv == sent, a "WARMUP: warmup=on" line; and at least one "on" rep
#                 saw the race (pong tx_unmatched > 0) with no loss - the warm-up absorbed it, shown, not assumed.
#   FAIL          any "on" rep with recv < sent, or no RESULT, or no WARMUP line.
#   NOT SHOWN     no "on" rep saw the race: the arm cannot fail here, so this PC does not decide (no verdict on the
#                 fix; the rig's next J run does, with rmw_loss_attribution.py).
#   CONTROL       "off" reps with loss: each must have pong tx_unmatched == loss (the rig's mechanism reproduced). A loss
#                 in "off" with tx_unmatched 0 is a different loss and voids the run.
#
# Usage: rmw_warmup_check.sh <reps per arm> <install dir>   Results: $OUT (default /home/semih/tmp_build/rmwj/warmup_check.txt)
# Launch detached (CLAUDE.md "Long-running work"); one EXIT trap.
set -u
REPS=${1:?usage: rmw_warmup_check.sh <reps per arm> <install dir>}
INSTALL=${2:?usage: rmw_warmup_check.sh <reps per arm> <install dir>}
OUT=${OUT:-/home/semih/tmp_build/rmwj/warmup_check.txt}
DUR_S=${DUR_S:-2}
PONG_LEAD_S=${PONG_LEAD_S:-2}
PONG_TAIL_S=${PONG_TAIL_S:-4} # the pong outlives the ping by this much; each run lasts about lead + dur + tail
ROS=${ROS:-/opt/ros/lyrical}
NS1=wuc-ns1-$$
NS2=wuc-ns2-$$
WORK=$(mktemp -d /home/semih/tmp_build/wuc.XXXXXX)
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

[ -f "$INSTALL/setup.bash" ] || { echo "REFUSED: no $INSTALL/setup.bash" | tee "$OUT"; exit 64; }
[ -x "$INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong/ping_node" ] || { echo "REFUSED: no ping_node in $INSTALL" | tee "$OUT"; exit 64; }

netns_up() {
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add wuc1 netns "$NS1" type veth peer name wuc2 netns "$NS2" || exit 1
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev wuc1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev wuc2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set wuc1 up
    sudo -n ip -n "$NS2" link set wuc2 up
}

run_one() { # <arm on|off> <qos best_effort|reliable> <rep>
    local arm=$1 qos=$2 rep=$3 flag="" wflag=""
    [ "$qos" = reliable ] && flag="--reliable"
    [ "$arm" = off ] && wflag="--no-warmup"
    local env=". $ROS/setup.bash; . $INSTALL/setup.bash; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=$WORK/roshome RMW_TICKLE_EXECUTOR_POLL=1"
    local bin=$INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong
    local pl=$WORK/${arm}_${qos}_${rep}_pong.log gl=$WORK/${arm}_${qos}_${rep}_ping.log sf=$WORK/${arm}_${qos}_${rep}_stamps.txt
    # shellcheck disable=SC2024 # the log files are this shell's
    sudo -n ip netns exec "$NS2" bash -c "$env; exec timeout -s INT $((PONG_LEAD_S + DUR_S + PONG_TAIL_S)) $bin/pong_node $flag -m bench" >"$pl" 2>&1 &
    local pong=$!
    sleep "$PONG_LEAD_S"
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "$env; exec timeout -s INT $((DUR_S + 15)) $bin/ping_node -i 0.1 -d $DUR_S $flag $wflag --wait poll --poll-sleep-us 50 --phase jitter --rtt-at callback --stamps $sf -m bench" >"$gl" 2>&1
    wait "$pong"
    local sent recv unm wu missing
    sent=$(grep -oE '^RESULT: .* sent=[0-9]+' "$gl" | grep -oE 'sent=[0-9]+$' | cut -d= -f2)
    recv=$(grep -oE '^RESULT: .* recv=[0-9]+' "$gl" | grep -oE 'recv=[0-9]+$' | cut -d= -f2)
    unm=$(grep -oE 'tx_unmatched_dropped=[0-9]+ tx_unmatched_kept=[0-9]+' "$pl" | head -1 | awk -F'[= ]' '{print $2 + $4}')
    wu=$(grep -h '^WARMUP:' "$gl" | cut -d' ' -f2- | tr ' ' ',')
    # which seqs never came back: the ping's --stamps lists every answered seq (seq send_ns reply_ns)
    missing=$([ -n "$sent" ] && [ -f "$sf" ] && awk -v n="$sent" '$1 ~ /^[0-9]+$/ {got[$1] = 1} END {s = ""; for (i = 1; i <= n; i++) if (!(i in got)) s = s (s == "" ? "" : ",") i; print (s == "" ? "-" : s)}' "$sf")
    echo "arm=$arm qos=$qos rep=$rep sent=${sent:-none} recv=${recv:-none} missing_seq=${missing:-none} pong_unmatched=${unm:-none} warmup_line=${wu:-none}" | tee -a "$OUT"
}

{
    echo "=== rmw_warmup_check $(date -Is) reps=$REPS dur=${DUR_S}s install=$INSTALL work=$WORK"
    echo "uptime: $(uptime)"
} >"$OUT"
netns_up
for rep in $(seq 1 "$REPS"); do
    for qos in best_effort reliable; do
        if [ $((rep % 2)) = 0 ]; then run_one on "$qos" "$rep"; run_one off "$qos" "$rep"; else run_one off "$qos" "$rep"; run_one on "$qos" "$rep"; fi
    done
done

# The reading, from the rows above.
awk '
/^arm=/ {
    for (i = 1; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
    arm = f["arm"]; n[arm]++
    if (f["sent"] == "none" || f["recv"] == "none") { noresult[arm]++; next }
    lost = f["sent"] - f["recv"]
    if (f["pong_unmatched"] == "none") { nocounter[arm]++ }
    else if (f["pong_unmatched"] > 0) { race[arm]++ }
    if (arm == "on") {
        if (lost > 0) onlost++
        if ($0 !~ / warmup_line=warmup=on,/) nowarm++
        if (f["pong_unmatched"] > 0 && lost == 0) absorbed++
    } else if (lost > 0) {
        offlost++
        if (f["pong_unmatched"] != lost) offother++
    }
}
END {
    printf "summary: on reps=%d race_seen=%d lost=%d no_result=%d no_warmup_line=%d absorbed=%d | off reps=%d race_seen=%d lost=%d lost_not_unmatched=%d no_result=%d | no_counter on=%d off=%d\n",
        n["on"], race["on"], onlost, noresult["on"], nowarm, absorbed, n["off"], race["off"], offlost, offother, noresult["off"], nocounter["on"], nocounter["off"]
    if (offother > 0) { print "VERDICT: VOID (an off-arm loss not explained by the pong having no matched reader)"; exit }
    if (nocounter["on"] + nocounter["off"] > 0) { print "VERDICT: VOID (a pong log without tx_unmatched counters: could not look)"; exit }
    if (onlost > 0 || noresult["on"] > 0 || nowarm > 0) { print "VERDICT: FAIL"; exit }
    if (absorbed == 0) { print "VERDICT: NOT SHOWN (no on rep saw the race; this PC does not decide)"; exit }
    print "VERDICT: PASS"
}' "$OUT" | tee -a "$OUT"
echo "DONE" >>"$OUT"

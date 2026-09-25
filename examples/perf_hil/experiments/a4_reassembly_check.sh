#!/usr/bin/env bash
# Is c6's ~92 transmissions per sample the kernel's IP reassembly collapsing, or TickLE's
# retransmit path? (OPTIMIZATION_PLAN.md section 9a, hypothesis A4.)
#
# A1 and A2 are ruled out with controls at the unit level, and A3's premise is false: core does not
# discard a far-ahead sample and re-request it, it abandons the gap and jumps the watermark
# (test_reliable_pubsub.c:781). A4 is the only hypothesis left that satisfies both constraints the
# campaign already imposes - it exists only where the datagram is fragmented, which is the axis c5
# (1.00 pkt/sample) and c6 (103.27) differ on, and a receive-side collapse is not bounded by the
# 9.75% two-fragment loss arithmetic, so ~92 is reachable.
#
# Built from current main so it carries the idle-based server lifetime (0da3cad5). That matters for
# reading recv < sent: truncation is gone, so a shortfall now means abandonment or real loss.
#
# HOW TO READ IT, written before running:
#   Controls first. If either fails, NEITHER the A4 arm nor anything else here says anything.
#     A  P1 + 5% loss   - no fragmentation is possible at 76 bytes, so server ReasmReqds delta must
#                         be ~0 and amplification ~1.0. This is c5, reproduced. If it shows
#                         reassembly, the counter is measuring some other traffic on that host and
#                         every number below is contaminated.
#     B  P4 + no loss    - fragmentation with nothing lost. ReasmReqds ~= 2 per sample, ReasmFails
#                         ~0, amplification ~1.0. If ReasmFails is large here, fragmentation alone
#                         breaks this rig and the loss arm cannot be attributed to loss.
#   Then the cell:
#     C  P4 + 5% loss   - ReasmFails large relative to samples sent, with A and B clean
#                           -> A4 carries it. The mechanism is the kernel, not TickLE's retransmit
#                              path, and neither core file we have been reading is where it lives.
#                         ReasmFails ~0 while amplification stays ~50-90x
#                           -> A4 is wrong. The amplification is TickLE's and needs a new hypothesis.
#                         Anything in between is a contribution, not an explanation, and the share
#                         is ReasmFails against the retransmission count from the stats build.
#
#   jump_abandoned_seq (tt_RELIABLE_STATS, the ONLY place an abandoned RELIABLE sample is counted -
#   every production counter reads zero through an abandonment, TickLE Dev) says how much of any
#   recv < sent is core giving up on a gap rather than the sample never arriving.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PIN="taskset -c 1-3"
OUT=${OUT:-/tmp/a4_reassembly.txt}
SHA=$(git -C "$(dirname "${BASH_SOURCE[0]}")/../../.." rev-parse origin/main)
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
tc_set() { case "$1" in
    off) sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true ;;
    on)  sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss 5%" ;;
  esac; }
# Leftovers by /proc/PID/exe, never by a command-line pattern: the server runs as "./server ..." and
# its command line never contains the path, so a -f pattern matches nothing and they accumulate.
server_pids() {
    # shellcheck disable=SC2016  # $p must expand on the rig, not here
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/tickle/*/server) echo "$p";; esac; done'
}
kill_servers() { local p; p=$(server_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
trap 'tc_set off; kill_servers' EXIT
# Column indices are read from the header rather than hard-coded, so a kernel that adds a field
# cannot silently shift which counter is being reported.
snmp() {
    # shellcheck disable=SC2016
    sh_ "$SERVER" 'awk "/^Ip:/{if(++n==1){for(i=2;i<=NF;i++)h[i]=\$i}else{for(i=2;i<=NF;i++)printf \"%s=%s \",h[i],\$i}}" /proc/net/snmp'
}
get() { tr ' ' '\n' <<<"$1" | grep -E "^$2=" | cut -d= -f2; }

: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== A4 reassembly check, $(date -Is), repo $SHA ==="

say "--- building the stats variants on both rpis ---"
pids=()
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx
cd examples/perf_hil/tickle
for s in p1 p4; do TICKLE_RELIABLE_STATS=1 ./build.sh reliable_throughput \$s > /tmp/a4_build_\$s.log 2>&1 \
  || { echo \"BUILD FAILED \$s on \$(hostname)\"; tail -8 /tmp/a4_build_\$s.log; exit 1; }
done
echo \"built on \$(hostname) at \$(git -C ~/tickle rev-parse --short HEAD)\"" &
    pids+=($!)
done
bad=0; for p in "${pids[@]}"; do wait "$p" || bad=1; done
[ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }

arm() {
    local label="$1" shape="$2" loss="$3"
    local dir="tickle/examples/perf_hil/tickle/reliable_throughput_$shape"
    kill_servers
    [ -z "$(server_pids)" ] || { say "$label ABORT: a server is still running"; return 1; }
    tc_set "$loss"
    local before; before=$(snmp)
    sh_ "$SERVER" "cd ~/$dir; nohup $PIN ./server -d 5 -Q > /tmp/a4_server.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(server_pids | wc -l)" = 1 ] || { say "$label ABORT: not exactly one server"; return 1; }
    local cl; cl=$(sh_ "$CLIENT" "cd ~/$dir && $PIN ./client -d 5 -Q" 2>&1)
    kill_servers
    local after; after=$(snmp)
    local srv; srv=$(sh_ "$SERVER" "cat /tmp/a4_server.log")
    local rq ro rf rt
    rq=$(( $(get "$after" ReasmReqds) - $(get "$before" ReasmReqds) ))
    ro=$(( $(get "$after" ReasmOKs)   - $(get "$before" ReasmOKs) ))
    rf=$(( $(get "$after" ReasmFails) - $(get "$before" ReasmFails) ))
    rt=$(( $(get "$after" ReasmTimeout) - $(get "$before" ReasmTimeout) ))
    say ""
    say "### $label  shape=$shape loss=$loss"
    say "  server reassembly delta: ReasmReqds=$rq ReasmOKs=$ro ReasmFails=$rf ReasmTimeout=$rt"
    say "  client: $(grep '^RESULT:' <<<"$cl" | head -1)"
    say "  client reliable stats: $(grep -iE 'jump_abandoned|retransmit|rstat|RELIABLE STATS' <<<"$cl" | tr '\n' ' ')"
    say "  server: $(grep '^RESULT:' <<<"$srv" | head -1)"
    say "  server reliable stats: $(grep -iE 'jump_abandoned|retransmit|rstat|RELIABLE STATS' <<<"$srv" | tr '\n' ' ')"
}

arm "A-control-p1-loss  (c5: no fragmentation possible)" p1 on
arm "B-control-p4-noloss (fragmentation, nothing lost)"  p4 off
arm "C-cell-p4-loss      (c6: the one in question)"      p4 on
tc_set off
say ""
say "qdisc now: $(sh_ "$CLIENT" 'tc qdisc show dev eth0')"
say "=== done ==="

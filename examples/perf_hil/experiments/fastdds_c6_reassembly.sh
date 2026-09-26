#!/usr/bin/env bash
# Is FastDDS's c6 delivery failure (28-36% delivered at p4 + 5% loss, two runs) FastDDS as shipped,
# or this harness? Designed by TickLE Dev, 2026-09-26, after it could not be settled off-rig (the dev
# box has FastDDS 3.6 without the 2.x headers the harness is written against).
#
# Hypothesis: FastDDS's UDPv4 transport only fragments at the RTPS layer above maxMessageSize, which
# defaults to 65,500 B and is NOT set in fastdds_eth0_only.xml. So a 2,800 B sample leaves as one
# ~2,990 B UDP datagram that the OS splits into IP fragments - the path TickLE's ipfrag build took,
# where the A4 check measured 97.4% of reassemblies failing at 5% loss. CycloneDDS avoids it by
# fragmenting in RTPS. The harness runs FastDDS exactly as run_scenario.sh does (the environment is
# not re-written here: this script calls run_scenario.sh), i.e. as shipped.
#
# HOW TO READ IT, written before running (server-side /proc/net/snmp Ip: deltas per arm):
#   A  FastDDS p1 + 5% loss  - CONTROL. 76 B cannot fragment: ReasmReqds delta must be ~0. If not,
#      the counter is seeing other traffic on that host and nothing below counts.
#   B  FastDDS p4, no loss   - ReasmReqds about 2-3 per delivered sample with ReasmFails ~0 means
#      FastDDS does use OS IP fragmentation at p4. ReasmReqds ~0 means it fragments in RTPS itself,
#      the hypothesis is wrong, and the next suspect is its reliability timers (heartbeatPeriod
#      default 3 s, left alone by the harness).
#   C  FastDDS p4 + 5% loss  - given B, a large ReasmFails is the c6 shortfall explained: FastDDS's
#      own transport default meeting the kernel, the same mechanism as TickLE-ipfrag. Not a harness
#      bug. A tuned-FastDDS arm (maxMessageSize ~1472) would then be tuning FastDDS, and whether to
#      add one is the user's decision, not a harness fix.
#
# DELAYED_READ_S=N (added 2026-09-26 after the first run): re-read the server's counters N seconds
# after each arm as well. The kernel counts a reassembly failure when an incomplete datagram times out
# (ipfrag_time, 30 s) or when fragment memory overflows; at FastDDS's low c6 rate only the timeout
# applies, so the immediate read can show ReasmFails=0 for fragments that were in fact lost. Pre-
# registered for the delayed read of arm C: ReasmFails well above zero means fragments were lost and
# the IP-fragmentation mechanism stands; still ~0 means no fragment was lost at the IP layer and the
# shortfall is in FastDDS's own recovery (the heartbeat-paced suspect), not the kernel.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
OUT=${OUT:-/tmp/fastdds_c6_reassembly_$(date +%Y-%m-%d).txt}
SHA=$(git -C "$REPO" rev-parse origin/main)
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
tc_set() {
    case "$1" in
        off) sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true ;;
        on) sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss 5%" ;;
    esac
    # Read back rather than trust the command: a failed del/replace must not leave the next arm
    # measuring a link it did not ask for.
    local q
    q=$(sh_ "$CLIENT" "tc qdisc show dev eth0 | head -1")
    if [ "$1" = on ]; then
        case "$q" in *"loss 5%"*) ;; *) echo "netem not applied: $q" >&2; exit 1 ;; esac
    else
        case "$q" in *netem*) echo "netem still present: $q" >&2; exit 1 ;; esac
    fi
}
trap 'tc_set off' EXIT
snmp() {
    # shellcheck disable=SC2016
    sh_ "$SERVER" 'awk "/^Ip:/{if(++n==1){for(i=2;i<=NF;i++)h[i]=\$i}else{for(i=2;i<=NF;i++)printf \"%s=%s \",h[i],\$i}}" /proc/net/snmp'
}
get() { tr ' ' '\n' <<<"$1" | grep -E "^$2=" | cut -d= -f2; }
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== FastDDS c6 reassembly check, $(date -Is), repo $SHA ==="
say "--- building fastdds reliable_throughput p1/p4 on both rpis ---"
pids=()
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/fastdds
for s in p1 p4; do ./build.sh reliable_throughput \$s >/tmp/fdc6_build_\$s.log 2>&1 || { echo BUILD-FAIL \$s; tail -6 /tmp/fdc6_build_\$s.log; exit 1; }; done" &
    pids+=($!)
done
bad=0; for p in "${pids[@]}"; do wait "$p" || bad=1; done
[ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "test -x ~/tickle/examples/perf_hil/fastdds/reliable_throughput_p4/client" || { say "no p4 client on $h"; exit 1; }
done
arm() {
    local label="$1" shape="$2" loss="$3" before after res sent recv rq ro rf
    tc_set "$loss"
    before=$(snmp)
    res=$(cd "$REPO/examples/perf_hil/fastdds" && RIG_LOCK_HELD_HIL=1 timeout 180 ./run_scenario.sh "reliable_throughput_$shape" -d 5 2>/dev/null | tr '\n' ' ')
    after=$(snmp)
    local late=""
    if [ "${DELAYED_READ_S:-0}" -gt 0 ]; then
        sleep "$DELAYED_READ_S"
        late=$(snmp)
    fi
    sent=$(grep -oE 'sent=[0-9]+' <<<"$res" | head -1 | cut -d= -f2)
    recv=$(grep -oE 'recv=[0-9]+' <<<"$res" | head -1 | cut -d= -f2)
    rq=$(( $(get "$after" ReasmReqds) - $(get "$before" ReasmReqds) ))
    ro=$(( $(get "$after" ReasmOKs) - $(get "$before" ReasmOKs) ))
    rf=$(( $(get "$after" ReasmFails) - $(get "$before" ReasmFails) ))
    say ""
    say "### $label  shape=$shape loss=$loss"
    say "  sent=${sent:-?} recv=${recv:-?}   server ReasmReqds=$rq ReasmOKs=$ro ReasmFails=$rf"
    [ -n "${recv:-}" ] && [ "$recv" -gt 0 ] && say "  per delivered sample: ReasmReqds $(awk -v a=$rq -v b="$recv" 'BEGIN{printf "%.2f",a/b}')  ReasmFails/ReasmReqds $(awk -v a=$rf -v b=$rq 'BEGIN{if(b>0)printf "%.1f%%",100*a/b; else print "n/a"}')"
    if [ -n "$late" ]; then
        say "  +${DELAYED_READ_S}s: ReasmReqds=$(( $(get "$late" ReasmReqds) - $(get "$before" ReasmReqds) )) ReasmOKs=$(( $(get "$late" ReasmOKs) - $(get "$before" ReasmOKs) )) ReasmFails=$(( $(get "$late" ReasmFails) - $(get "$before" ReasmFails) )) ReasmTimeout=$(( $(get "$late" ReasmTimeout) - $(get "$before" ReasmTimeout) ))"
    fi
    say "  client: $(grep -oE '(drained|write_fail|wire_packets_per_sample|wire_bytes_per_sample)=[^ ]+' <<<"$res" | head -4 | tr '\n' ' ')"
}
arm "A-control  FastDDS p1 + 5% loss (cannot fragment)" p1 on
arm "B          FastDDS p4, no loss"                     p4 off
arm "C          FastDDS p4 + 5% loss (c6)"               p4 on
tc_set off
say ""; say "=== done ==="

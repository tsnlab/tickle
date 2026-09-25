#!/usr/bin/env bash
# Was campaign cell 6 (P4, RELIABLE+KEEP_ALL, 5% loss) measuring TickLE, or measuring its server
# exiting first?
#
# c6 was the only cell in the whole 108-run campaign with sent != recv (14002 vs 13802) and
# peer_acks_end=0. Every other cell, including c5 at the same 5% loss, delivered everything and kept
# its peer. reliable_throughput/server.c caps itself at -d + 15 s = 20 s, run_scenario.sh starts it
# BEFORE the client, and c6's client put 2.10 GB on the wire, which needs >= 16.8 s at line rate.
# So the server plausibly exited while the client was still draining.
#
# HOW TO READ IT, written before running:
#   long server, loss:  sent == recv AND peer_acks_end=1 AND amplification near the no-loss 2.0x
#       -> c6 measured the harness. There is no retransmission storm, and the campaign's c6 row is
#          void rather than an optimisation target.
#   long server, loss:  sent != recv OR peer_acks_end=0, and amplification stays ~50x
#       -> TickLE genuinely fails to drain at P4 under loss. The storm is real.
#   CONTROL, long server, NO loss: must look exactly like the campaign's c4 (2.0x, sent == recv).
#       If the control differs, giving the server a different -d changed something on its own and
#       NEITHER arm says anything about loss.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
DIR=tickle/examples/perf_hil/tickle/reliable_throughput_p4
PIN="taskset -c 1-3"
OUT=${OUT:-/tmp/c6_lifetime_check.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
tc_set() { case "$1" in
    off) sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true ;;
    on)  sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss 5%" ;;
  esac; }
trap 'tc_set off; kill_servers' EXIT
: > "$OUT"
# Leftovers are identified by /proc/PID/exe, never by a command-line pattern. The first version of
# this script used `pkill -INT -f "$DIR/server"`, which matched nothing - the server is started with
# `cd ~/$DIR; ./server`, so its command line is "./server -d 60 -Q" and never contains the path. The
# servers therefore survived every arm and accumulated: arm B ran against two of them and arm C
# against three, which peer_acks_end=2 and =3 recorded faithfully while the throughput numbers they
# produced looked perfectly plausible. Same rule as CLAUDE.md's, in the direction where a pattern
# matches too little instead of too much.
server_pids() {
    # shellcheck disable=SC2016  # deliberate: $p must expand on the rig, not here
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */reliable_throughput_p4/server) echo "$p";; esac; done'
}
kill_servers() {
    local pids; pids=$(server_pids)
    [ -z "$pids" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $pids" >/dev/null 2>&1 || true
    sleep 2
}
require_no_servers() {
    local left; left=$(server_pids)
    [ -z "$left" ] && return 0
    echo "REFUSING: server(s) still running before an arm: $left" | tee -a "$OUT"
    exit 1
}

arm() {
    local label="$1" loss="$2" server_d="$3"
    kill_servers
    require_no_servers
    tc_set "$loss"
    sh_ "$SERVER" "cd ~/$DIR; nohup $PIN ./server -d $server_d -Q > /tmp/c6chk_server.log 2>&1 < /dev/null &"
    sleep 3
    local n; n=$(server_pids | wc -l)
    if [ "$n" != 1 ]; then echo "$label ABORT: expected exactly 1 server, found $n" | tee -a "$OUT"; return 1; fi
    local line; line=$(sh_ "$CLIENT" "cd ~/$DIR && $PIN ./client -d 5 -Q" | grep '^RESULT:')
    kill_servers
    echo "$label server_d=$server_d loss=$loss | $line" | tee -a "$OUT"
}

arm "A-control-noloss-longserver " off 60
arm "B-loss-longserver           " on  60
arm "C-loss-campaign-lifetime    " on  5
tc_set off
echo "qdisc now: $(sh_ "$CLIENT" 'tc qdisc show dev eth0')" | tee -a "$OUT"
echo "=== done ===" | tee -a "$OUT"

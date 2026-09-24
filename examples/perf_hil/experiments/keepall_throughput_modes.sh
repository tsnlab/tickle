#!/usr/bin/env bash
# COMPARISON.MD §3b's TickLE throughput cells carry a provisional mark (◊) because five consecutive
# reps at tc 0% came back 94.4 / 111.0 / 112.7 / 94.9 / 111.5 Mbps - two clean clusters with nothing
# between them, and each published cell was a median of 3, so it reported whichever cluster that
# cell happened to sample. The measured cause is sender-vs-NIC-IRQ core placement (commit 75b65bef),
# which `taskset -c 1-3` in run_scenario.sh does not pin away: the IRQ can still land on one of
# those three.
#
# This runs enough reps per cell to report BOTH modes rather than one of them, and records each
# rep's own cpu_main/cpu_main_share/cpu_migrations alongside its rate so the split can be tied to
# core placement rather than asserted to follow it. The scenario arguments are the ones §3b's own
# figures were taken with - plain `-Q`, 5s - so the output is comparable to the cells it replaces
# and to the DDS columns beside them, which are NOT re-measured here (the ◊ is on TickLE only).
#
# How to read the result: if the rates at a given tc level fall into two separated clusters AND
# cpu_main (or cpu_main_share) differs between them, the bimodality is core placement and both
# modes should be published with their condition. If the rates cluster but cpu_main does not vary,
# the cause is something else and this experiment has not found it. If the rates do not cluster at
# all, the earlier five-rep observation did not reproduce and that is the finding.
set -euo pipefail

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT="10.1.1.214"
PERF_LINK_BROADCAST="192.168.10.255"
REPS="${REPS:-9}"
OUT="${OUT:-/tmp/tickle_3b_modes.txt}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ssh_run() {
    # shellcheck disable=SC2029 # meant to expand remotely
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new "ci@$RPI_CLIENT" "$@"
}

CLIENT_IFACE="$(ssh_run "ip route get $PERF_LINK_BROADCAST" 2>/dev/null | grep -oP 'dev \K\S+' | head -1 || true)"
[ -n "$CLIENT_IFACE" ] || { echo "cannot resolve rpi#1's test-link interface - aborting" >&2; exit 1; }

set_loss() {
    local pct="$1"
    if [ "$pct" = "0" ]; then
        ssh_run "sudo -n tc qdisc del dev $CLIENT_IFACE root" >/dev/null 2>&1 || true
    else
        ssh_run "sudo -n tc qdisc replace dev $CLIENT_IFACE root netem loss ${pct}%"
    fi
}
# Restore the link on ANY exit. Without this a sweep killed mid-run at 50% injected loss leaves the
# rig silently corrupting every later measurement, including CI's - which has happened here before.
trap 'set_loss 0' EXIT

: > "$OUT"
echo "=== §3b TickLE KEEP_ALL throughput modes, $REPS reps per tc level, $(date -Is) ===" | tee -a "$OUT"
echo "rig sha: $(ssh_run 'cd ~/tickle && git rev-parse --short HEAD')" | tee -a "$OUT"

for pct in 0 1 5 20 50; do
    set_loss "$pct"
    echo "" | tee -a "$OUT"
    echo "--- tc loss ${pct}% ---" | tee -a "$OUT"
    for rep in $(seq 1 "$REPS"); do
        line="$("$HERE/../tickle/run_scenario.sh" reliable_throughput -Q -d 5 2>/dev/null | grep '^RESULT:' | grep 'role=client' || true)"
        if [ -z "$line" ]; then
            echo "rep $rep: NO RESULT LINE" | tee -a "$OUT"
            continue
        fi
        mbps=$(echo "$line" | grep -oP 'send_mbps=\K[0-9.]+')
        cpu=$(echo "$line" | grep -oP 'cpu_main=\K-?[0-9]+')
        share=$(echo "$line" | grep -oP 'cpu_main_share=\K[0-9.]+')
        mig=$(echo "$line" | grep -oP 'cpu_migrations=\K[0-9]+')
        mhz=$(echo "$line" | grep -oP 'cpu_mhz_mean=\K[0-9.]+')
        echo "rep $rep: mbps=$mbps cpu_main=$cpu share=$share migrations=$mig mhz=$mhz" | tee -a "$OUT"
    done
done
set_loss 0
echo "" | tee -a "$OUT"
echo "=== done $(date -Is); tc restored ===" | tee -a "$OUT"

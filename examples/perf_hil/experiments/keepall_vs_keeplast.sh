#!/usr/bin/env bash
# What the KEEP_ALL guarantee costs TickLE in throughput, measured against its own KEEP_LAST mode in
# ONE session. §3b's "cost of the guarantee" paragraph previously put a KEEP_ALL figure from one day
# against a KEEP_LAST figure from another - a cross-day comparison in the very paragraph that
# quantifies the headline trade-off, and exactly what §5 exists to warn about. Recomputing it from
# the new KEEP_ALL numbers without re-measuring KEEP_LAST would have put the same two-day comparison
# straight back in.
#
# TickLE only; the two DDS columns are not involved in this trade-off at all. Modes interleaved
# within each loss level so a drift lands on both equally.
set -euo pipefail
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"; RPI_CLIENT=10.1.1.214
REPS="${REPS:-3}"; DUR="${DUR:-5}"; OUT="${OUT:-/tmp/tickle_keepall_cost.txt}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ssh_c() { ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$RPI_CLIENT" "$@"; }
sl() { if [ "$1" = 0 ]; then ssh_c "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
       else ssh_c "sudo -n tc qdisc replace dev eth0 root netem loss $1%"; fi; }
trap 'sl 0' EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== KEEP_ALL vs KEEP_LAST, one session, $(date -Is) ==="
say "rig sha: $(ssh_c 'cd ~/tickle && git rev-parse --short HEAD'), ${REPS} reps, -d ${DUR}"
for pct in 0 1 5 20 50; do
    sl "$pct"; say ""; say "--- tc loss ${pct}% ---"
    for rep in $(seq 1 "$REPS"); do
        for mode in keepall keeplast; do
            args="-d $DUR"; [ "$mode" = keepall ] && args="-Q $args"
            # shellcheck disable=SC2086 # deliberately word-split
            line="$("$HERE/../tickle/run_scenario.sh" reliable_throughput $args 2>/dev/null | grep '^RESULT:' | grep 'role=client' || true)"
            [ -n "$line" ] || { say "$mode rep $rep: NO RESULT LINE"; continue; }
            say "$mode rep $rep: mbps=$(echo "$line" | grep -oP 'send_mbps=\K[0-9.]+') write_fail=$(echo "$line" | grep -oP 'write_fail=\K[0-9]+') drained=$(echo "$line" | grep -oP 'drained=\K[a-z]+')"
        done
    done
done
sl 0; say ""; say "=== done $(date -Is); tc restored ==="

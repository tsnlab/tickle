#!/usr/bin/env bash
# §3b's three columns, measured in ONE session so they are comparable to each other.
#
# Why this exists: the TickLE column was re-measured on 2026-09-24 while the FastDDS and CycloneDDS
# columns still dated from 2026-09-23, and §5 of the same document had just demonstrated that this
# rig can carry a ~13us additive offset between measurement days across every implementation at
# once. A "10-45x either vendor" claim spanning two days is the exact defect that section records,
# left sitting in the table beside it. Re-measuring one column fixed a provisional mark and created
# a comparability problem; this fixes both.
#
# All three frameworks, all five tc levels, interleaved BY FRAMEWORK within each level rather than
# run as three blocks - so a drift during the session lands on all three equally instead of on
# whichever ran last.
#
# How to read it: the three columns are comparable to each other because they were taken minutes
# apart under one condition. They are NOT comparable to the previously published figures, and the
# document must say so rather than quietly replace numbers taken a different way.
set -euo pipefail

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT=10.1.1.214
RPI_SERVER=10.1.1.213
REPS="${REPS:-3}"
DUR="${DUR:-5}"
# Timestamped by default. A fixed filename means a run that never started leaves the PREVIOUS
# run's file in place, and a summary read from it is indistinguishable from a fresh result -
# which happened on 2026-09-24: an interleaved sweep sat blocked on the rig lock while its
# predecessor's numbers were about to be reported as the after-measurement. The tell was that
# they matched to three significant figures.
OUT="${OUT:-/tmp/tickle_3b_threeway_$(date +%Y%m%d-%H%M%S).txt}"
ln -sfn "$OUT" "/tmp/tickle_3b_threeway_latest.txt"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ssh_c() { ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$RPI_CLIENT" "$@"; }
sl() { if [ "$1" = 0 ]; then ssh_c "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
       else ssh_c "sudo -n tc qdisc replace dev eth0 root netem loss $1%"; fi; }
trap 'sl 0' EXIT

: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== §3b three-way, one session, $(date -Is) ==="
say "rig sha: $(ssh_c 'cd ~/tickle && git rev-parse --short HEAD'), ${REPS} reps, -d ${DUR}"

say "--- building all three on both rpis ---"
for host in "$RPI_CLIENT" "$RPI_SERVER"; do
    ssh -i "$SSH_KEY" -o BatchMode=yes "ci@$host" "set -e
cd ~/tickle/examples/perf_hil
for fw in tickle cyclonedds fastdds; do
  (cd \$fw && ./build.sh reliable_throughput >/tmp/3b_build_\$fw.log 2>&1) || { echo \"BUILD FAILED: \$fw\"; tail -8 /tmp/3b_build_\$fw.log; exit 1; }
done
echo built" &
done
wait
say "built"

# Before every cell: is a benchmark process from an earlier cell still alive on either Pi? Three
# false numbers on 2026-09-24 came from a leftover process - a CI server sharing the rig, a cancelled
# CI job, and (suspected, not shown) a FastDDS pair still retransmitting into a 50%-loss link after
# its drain timed out, which preceded the one TickLE rep that read 8.4 instead of ~22.
#
# Identified by /proc/PID/exe under ~/tickle/examples/perf_hil, never by a name pattern (CLAUDE.md
# rule 3: a pattern matches the command doing the matching). Waits for them to exit rather than
# killing them - killing would destroy the evidence of which framework left one behind - and if they
# outlive the wait, the cell is still run but tagged, so a contaminated number can never be read as
# a clean one.
rig_leftovers() {
    local host out=""
    for host in "$RPI_CLIENT" "$RPI_SERVER"; do
        out+=$(ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$host" '
            for d in /proc/[0-9]*; do
                e=$(readlink "$d/exe" 2>/dev/null) || continue
                case "$e" in */tickle/examples/perf_hil/*/client|*/tickle/examples/perf_hil/*/server)
                    fw=${e%/*/*}; fw=${fw##*/}; printf "%s:%s:%s " "$(hostname)" "$fw" "${d#/proc/}";;
                esac
            done' 2>/dev/null)
    done
    printf '%s' "$out"
}
wait_rig_quiet() {
    local left _
    for _ in $(seq 1 20); do
        left=$(rig_leftovers)
        [ -z "$left" ] && { echo none; return 0; }
        sleep 1
    done
    echo "$left"
}

for pct in 0 1 5 20 50; do
    sl "$pct"
    say ""
    say "--- tc loss ${pct}% ---"
    for rep in $(seq 1 "$REPS"); do
        for fw in tickle cyclonedds fastdds; do
            leftover=$(wait_rig_quiet)
            args="-d $DUR"
            [ "$fw" = tickle ] && args="-Q $args"
            # shellcheck disable=SC2086 # $args is deliberately word-split
            line="$("$HERE/../$fw/run_scenario.sh" reliable_throughput $args 2>/dev/null | grep '^RESULT:' | grep 'role=client' || true)"
            if [ -z "$line" ]; then say "$fw rep $rep: NO RESULT LINE leftover_before=$leftover"; continue; fi
            say "$fw rep $rep: mbps=$(echo "$line" | grep -oP 'send_mbps=\K[0-9.]+') drained=$(echo "$line" | grep -oP 'drained=\K[a-z]+') write_fail=$(echo "$line" | grep -oP 'write_fail=\K[0-9]+') leftover_before=$leftover$([ "$leftover" != none ] && echo ' CONTAMINATED')"
        done
    done
done
sl 0
say ""
say "=== done $(date -Is); tc restored ==="

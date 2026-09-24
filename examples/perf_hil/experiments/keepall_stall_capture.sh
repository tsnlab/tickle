#!/usr/bin/env bash
# Why does RELIABLE+KEEP_ALL now deliver NOTHING in some runs? On 2026-09-24, after in-order
# delivery landed, 4 of 15 section 3b reps came back at 0.000 Mbps with drained=timeout and
# write_fail=49 - the same 49 every time, which looks like a fixed window filling and never draining
# rather than a slowdown. Two of three reps stalled at 20% loss.
#
# The counters that would explain it were never read, because run_scenario.sh overwrites the rpis'
# per-scenario logs every rep - a stall's evidence was destroyed by the next rep. This keeps BOTH
# sides' full logs for every rep, so a stalled rep's reorder_* and delivery counters can be read.
#
# How to read it (written before running): for each stalled rep, look at the server's delivery line.
#   reorder_overflow > 0         -> the "sized to the window, overflow impossible" argument has a hole
#   held_peak at the bound, overflow 0 -> back-pressure working as designed; the stall is inherent
#   writer_switches > 0          -> the subscriber saw the writer's identity change mid-run, which
#                                    resets ordering state; a strong lead on its own
# A stalled rep with NO delivery line at all is void for these questions, not evidence of anything.
set -euo pipefail
K="$HOME/.ssh/tickle_ci_ed25519"; C=ci@10.1.1.214; S=ci@10.1.1.213
LOSS="${LOSS:-20}"; REPS="${REPS:-6}"
DIR="${DIR:-/tmp/tickle_stall_$(date +%Y%m%d-%H%M%S)}"; mkdir -p "$DIR"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sl(){ if [ "$1" = 0 ]; then ssh -i "$K" -o BatchMode=yes "$C" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
      else ssh -i "$K" -o BatchMode=yes "$C" "sudo -n tc qdisc replace dev eth0 root netem loss ${1}%"; fi; }
trap 'sl 0' EXIT
echo "=== KEEP_ALL stall capture, tc ${LOSS}%, ${REPS} reps, interleave=${INTERLEAVE:-0}, rig at $(ssh -i "$K" -o BatchMode=yes "$C" 'cd ~/tickle && git rev-parse --short HEAD'), logs in $DIR ===" | tee "$DIR/summary.txt"
sl "$LOSS"
# INTERLEAVE=1 runs a CycloneDDS and a FastDDS reliable_throughput cell between TickLE reps, exactly
# as the section 3b three-way sweep did. That sweep stalled TickLE in 2 of 3 reps at 20% loss; a
# TickLE-only run of the same cell stalled in 0 of 6. The only procedural difference is the DDS cells
# in between, so this arm asks whether the stall needs them.
for rep in $(seq 1 "$REPS"); do
  if [ "${INTERLEAVE:-0}" = "1" ]; then
    "$HERE/../cyclonedds/run_scenario.sh" reliable_throughput -d 5 >/dev/null 2>&1 || true
    "$HERE/../fastdds/run_scenario.sh" reliable_throughput -d 5 >/dev/null 2>&1 || true
  fi
  line="$("$HERE/../tickle/run_scenario.sh" reliable_throughput -Q -d 5 2>/dev/null | grep '^RESULT:' | grep 'role=client' || true)"
  scp -q -i "$K" "$S:/tmp/tickle_reliable_throughput_server.log" "$DIR/rep${rep}.server.log" 2>/dev/null || echo "(no server log)" > "$DIR/rep${rep}.server.log"
  scp -q -i "$K" "$C:/tmp/tickle_reliable_throughput_client.log" "$DIR/rep${rep}.client.log" 2>/dev/null || true
  printf '%s\n' "$line" > "$DIR/rep${rep}.result"
  mbps=$(grep -oP 'send_mbps=\K[0-9.]+' <<<"$line" || echo "?")
  wf=$(grep -oP 'write_fail=\K[0-9]+' <<<"$line" || echo "?")
  dr=$(grep -oP 'drained=\K[a-z]+' <<<"$line" || echo "?")
  del=$(grep -a 'delivery: delivered=' "$DIR/rep${rep}.server.log" | tail -1 | grep -oE '(delivered|writer_switches|timestamp_not_newer|out_of_order_discarded|reorder_held_peak|reorder_overflow|reorder_abandoned)=[0-9]+' | paste -sd' ' || true)
  echo "rep $rep: mbps=$mbps write_fail=$wf drained=$dr | ${del:-NO DELIVERY LINE (void)}" | tee -a "$DIR/summary.txt"
done
sl 0
echo "=== done $(date -Is); tc restored ===" | tee -a "$DIR/summary.txt"

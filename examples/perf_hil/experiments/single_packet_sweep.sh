#!/usr/bin/env bash
# The single-packet comparison. User's decision (2026-09-25): the cases where TickLE's datagram
# splits are set aside until DATA_FRAG exists, so the comparison and the optimisation are limited to
# payloads where ALL THREE send exactly one packet - and CPU is an explicit target alongside the rest.
#
# In scope, from the measured framing model (base framing tickle 70.6, cyclonedds 104.1, fastdds
# 209.6 B; one frame is 1514 B, so the fair ceiling is 1514-209.6 = 1304 B, FastDDS binding):
#     p1   76 B    all three 1 packet
#     p2 1292 B    all three 1 packet   <- resized for exactly this by 1957a4b1, never yet measured
# Out of scope now: p3 (1424, splits both vendors) and p4 (2800, splits all three).
#
# Every framework is given its QoS EXPLICITLY, per the user's binding instruction ("모든 애들이
# 동일한 QoS를 설정한 후 테스트"): RELIABLE + KEEP_ALL on all three. TickLE takes -Q; both DDS
# harnesses have KEEP_ALL as their default and now also report keep_all= so the line records the
# history it RAN rather than the one it was asked for.
#
# HOW TO READ IT, written before running:
#   THE HEADLINE QUESTION is CPU in the latency cells. The campaign's only CPU losses were there -
#   TickLE 6,516 cpu_s_per_Msample against CycloneDDS's 200, a 32x loss caused by tt_Node_poll()
#   waking every 100 us. The poll change (69e63a34, 6f01de6f) cut idle wakes 1013x. So:
#     TickLE's latency cpu_s_per_Msample drops by roughly that order and lands near the vendors'
#       -> the poll change fixed the campaign's CPU loss and there is nothing left to optimise there.
#     It drops but stays far above them
#       -> the remaining cost is per-sample work, not the poll cadence, and that is the next target.
#     It does not drop
#       -> the poll change does not reach this path and the earlier 1013x was measured on something
#          the latency client does not do.
#   THROUGHPUT CELLS are expected to stay as the campaign found them (TickLE ahead on CPU, memory,
#   bandwidth and throughput). A reversal there would mean one of today's four core changes cost
#   something the campaign's cells cannot see, and is more important than any win here.
#   BOUNDARY GATE: at p2 every framework's client wire_role_packets_per_sample must read below 1.5.
#   If FastDDS splits at 1292 B the fair ceiling is wrong and p2's comparison is VOID - it has 12.4 B
#   of margin, the least of the three.
#   Any RELIABLE cell with sent != recv is VOID, not a data point.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
REPS=${REPS:-3}
OUT=${OUT:-/tmp/single_packet.txt}
SHA=$(git -C "$(dirname "${BASH_SOURCE[0]}")/../../.." rev-parse --short origin/main)
PH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== single-packet sweep, $(date -Is), repo $SHA ==="
say "in scope: p1 (76 B), p2 (1292 B) - all three frameworks send one packet at both"

VARIANTS="reliable_throughput_p1 reliable_throughput_p2 reliable_latency_p1 reliable_latency_p2"
pids=(); bad=0
say ""; say "--- building $VARIANTS x 3 frameworks on both rpis ---"
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx
cd examples/perf_hil
for v in $VARIANTS; do
  scen=\${v%_p[0-9]}; size=\${v##*_}
  for fw in tickle cyclonedds fastdds; do
    (cd \$fw && ./build.sh \$scen \$size > /tmp/sp_\${fw}_\$v.log 2>&1) \
      || { echo \"BUILD FAILED \$fw \$v\"; tail -6 /tmp/sp_\${fw}_\$v.log; exit 1; }
  done
done
echo \"\$(hostname) built at \$(git rev-parse --short HEAD)\"" &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || bad=1; done
[ "$bad" = 0 ] || { say "BUILD FAILED - stopping"; exit 1; }
missing=""
for h in "$CLIENT" "$SERVER"; do
    absent=$(sh_ "$h" "for v in $VARIANTS; do for fw in tickle cyclonedds fastdds; do
  for b in client server; do [ -x ~/tickle/examples/perf_hil/\$fw/\$v/\$b ] || echo \"\$fw/\$v/\$b\"; done; done; done" 2>&1)
    [ -z "$absent" ] || missing+="$h: $(tr '\n' ' ' <<<"$absent")"
done
[ -z "$missing" ] || { say "MISSING BINARIES: $missing"; exit 1; }
say "  all $(( $(wc -w <<<"$VARIANTS") * 3 * 2 )) binaries present on both rpis"

cell() {   # $1 variant  $2 fw  $3 args
    local res
    # shellcheck disable=SC2086 # $3 is deliberately word-split into the harness's argv
    res=$(cd "$PH/$2" && timeout 180 ./run_scenario.sh "$1" $3 2>/dev/null \
        | grep '^RESULT:' | tr '\n' ' ' | sed -E 's/RESULT: framework=[a-z]+ scenario=[a-z_0-9]+ //g' || true)
    echo "${res:-NO-RESULT}"
}
for v in $VARIANTS; do
    args="-d 5"; case "$v" in reliable_latency*) args="-i 0.1 -d 10";; esac
    say ""; say "### $v   args: $args"
    for r in $(seq 1 "$REPS"); do
        for fw in tickle cyclonedds fastdds; do
            a="$args"; [ "$fw" = tickle ] && a="-Q $args"
            res=$(cell "$v" "$fw" "$a")
            # Identity, asserted rather than optionally matched: TickLE's line must say which core
            # build it ran. Every figure published before 2026-09-26 was core_build=debug (-O0)
            # against vendor release packages, and nothing in the output said so. A reader that only
            # greps the fields it wants cannot notice a field that is missing.
            if [ "$fw" = tickle ]; then
                cb=$(grep -oE 'core_build=[a-z]+' <<<"$res" | head -1 | cut -d= -f2)
                if [ "${cb:-none}" != release ]; then
                    say "$v $fw rep$r VOID(core_build=${cb:-absent}, wanted release)"; continue
                fi
            fi
            say "$v $fw rep$r | $res"
        done
    done
done
say ""; say "=== done ==="

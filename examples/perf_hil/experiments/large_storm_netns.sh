#!/usr/bin/env bash
# large_storm_netns.sh - why the 1 MiB RELIABLE control of wifi_burst_loss_netns.sh (offload off, no loss configured)
# went into a repair storm (RESULTS.md WB, 2026-10-23). Runs that harness's ctl arm, rel_large cell only, with
# TT_UDP_OFFLOAD=0 and RSTATS, once per VARIANT, each under OUT/<variant>/. A diagnostic: the variants are the controls
# the hypotheses predict differently.
#   base      the WB control as it ran: two prio bands (broadcast in its own netem), limit 10000, slot 1-4 ms x 42.
#   bands1    broadcast shares the unicast netem (one FIFO): nothing can overtake the queued data.
#   limit1e5  netem limit 100000: the queue cannot overflow.
#   mtu       the writer sends MTU units (-F mtu): no 45-unit IP run lost whole for one frame.
#   noslot    no slot (delay 2ms only): no serialization, the 1 MiB drains at veth speed.
# Usage: [VARIANTS="base bands1 ..."] [REPS=3] [DUR=30] [OUT=dir] large_storm_netns.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VARIANTS=${VARIANTS:-"base bands1 limit1e5 mtu noslot"}
REPS=${REPS:-3}
DUR=${DUR:-30}
OUT=${OUT:-$HOME/rig_results_safe/large_storm/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT" || exit 1
echo "=== large_storm_netns $(date -Is) VARIANTS='$VARIANTS' REPS=$REPS DUR=$DUR OUT=$OUT"
for v in $VARIANTS; do
    base="limit 10000 delay 2ms slot 1ms 4ms packets 42"
    bands=2 pub_args=""
    case "$v" in
    base) ;;
    bands1) bands=1 ;;
    limit1e5) base="limit 100000 delay 2ms slot 1ms 4ms packets 42" ;;
    mtu) pub_args="-F mtu" ;;
    noslot) base="limit 10000 delay 2ms" ;;
    *)
        echo "unknown variant $v" >&2
        exit 2
        ;;
    esac
    echo "--- $v $(date -Is) load=$(cut -d' ' -f1 /proc/loadavg)"
    OUT="$OUT/$v" ARMS=ctl CELLS=rel_large REPS=$REPS DUR=$DUR NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1 BANDS=$bands \
        NETEM_BASE="$base" LARGE_PUB_ARGS="$pub_args" "$HERE/wifi_burst_loss_netns.sh" >"$OUT/$v.log" 2>&1
    for d in "$OUT/$v"/runs/*; do
        echo "$v $(basename "$d") $(cut -d' ' -f1 "$d/meta.txt")" \
            "$(grep -o 'retransmitted=[0-9]*' "$d/ping.log" | head -1)" \
            "$(grep -o 'latency_median_us=[0-9.]* latency_p99_us=[0-9.]*' "$d/echo.log")" \
            "$(grep -o 'hybrid_n_hist=[0-9/]*' "$d/ping.log")" \
            "drops=$(grep -o 'dropped [0-9]*' "$d/counters.txt" | awk '{s += $2} END {print s + 0}')" \
            "$(grep 'role=sub acknack_tx' "$d/echo.log" | grep -o 'immediate=[0-9]* timer=[0-9]* new_gap=[0-9]* sent=[0-9]* bits_sent=[0-9]*')" \
            "$(grep 'role=sub sub' "$d/echo.log" | grep -o 'gaps_opened=[0-9]* missing_opened=[0-9]*')"
    done
done
echo "=== large_storm_netns ended $(date -Is)"

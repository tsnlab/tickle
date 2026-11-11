#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# Stage 0 of RANGE_NACK_DESIGN_2026-11-11: ACKNACK share on T1 broadcast cells. Simulation only. Measurement harness, no library change.
# CELLS="be rel" PROFS="..." ; mode small|large. Binaries built from the worktree $W (main ae3a5f85).
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
W=${W:-$(cd "$S/../../../.." && pwd)} # tree the nodes are built from
E=$W/examples/perf_hil/experiments
export PHY_BASE=6 RETRY=1 SEED=7 PIN_WM=2 TMPDIR=${T1_TMPDIR:-$HOME/tmp_build}
MODE=${MODE:-small}
REPS=${REPS:-2}
DUR=${DUR:-60}
CELLS=${CELLS:-"be rel"}
PROFILES=${PROFILES:-"ug:0.36:733,2.2,0.01,2.1,1.3,30.5 psu_cs:0.44:504,28,0.01,9.7,1.0,46.5 o60:0.9:1140,60,0.01"}
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-range_nack_s0}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "S0_START $(date -Is) out=$OUT mode=$MODE cells=$CELLS dur=$DUR reps=$REPS rev=$(git -C "$W" log -1 --format=%h)"
if [ "$MODE" = small ]; then
    sed "s/^    return qos;/    qos.history.depth = 100;\n    return qos;/" "$E/wifi_link_node.c" >"$OUT/wifi_link_node_d100.c"
    cmp -s "$E/wifi_link_node.c" "$OUT/wifi_link_node_d100.c" && { echo "VOID: sed changed nothing"; exit 1; }
    cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS -Dtt_UNICAST_PEER_THRESHOLD=0 -I"$W/include" -I"$W/src" -I"$W/examples/perf_hil/tickle/common" \
        -o "$T/node_s0" "$OUT/wifi_link_node_d100.c" "$W/src/tickle.c" "$W/src/encoding.c" "$W/src/log.c" "$W/src/hal_linux.c" -lpthread -lm >"$OUT/build.log" 2>&1 \
        || { echo "VOID build"; tail -3 "$OUT/build.log"; exit 1; }
else
    cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS -Dtt_UNICAST_PEER_THRESHOLD=0 \
        -I"$W/include" -I"$W/src" -o "$T/large_s0" "$E/large_sample_bench.c" "$W/src/tickle.c" "$W/src/encoding.c" "$W/src/log.c" "$W/src/hal_linux.c" -lpthread -lm >"$OUT/build.log" 2>&1 \
        || { echo "VOID build"; tail -3 "$OUT/build.log"; exit 1; }
fi
txb() { sudo -n ip -j -s -n "$1" link show dev "$2" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["stats64"]["tx"]["bytes"])'; }
for cell in $CELLS; do
for prof in $PROFILES; do
    name=${prof%%:*}; rest=${prof#*:}; badp=${rest%%:*}; ge=${rest#*:}
    for rep in $(seq 1 "$REPS"); do
        run=${cell}_${name}_r$rep
        "$S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        BAD_P=$badp "$S/t1_topo.sh" up 1 "$ge" >"$OUT/$run.topo" 2>&1
        grep -q "^T1_UP lanes" "$OUT/$run.topo" || { echo "VOID $run (topo)"; continue; }
        b1=$(txb t1_0_1 wlan7); b2=$(txb t1_0_2 wlan8)
        if [ "$MODE" = small ]; then
            taskset -c 6 sudo -n ip netns exec t1_0_2 "$T/node_s0" echo -c 2 -b 10.80.0.255 -d $((DUR + 20)) -m "$cell" -s 100 -r 100 -p wifi >"$OUT/$run.echo" 2>&1 &
            ep=$!
            sleep 1
            taskset -c 4 sudo -n ip netns exec t1_0_1 "$T/node_s0" ping -c 1 -b 10.80.0.255 -d "$DUR" -m "$cell" -s 100 -r 100 -p wifi >"$OUT/$run.ping" 2>&1
        else
            sudo -n ip netns exec t1_0_2 env TT_UDP_OFFLOAD=0 "$T/large_s0" sub -s 131072 -d $((DUR + 8)) -R -b 10.80.0.255 -P wifi >"$OUT/$run.echo" 2>&1 &
            ep=$!
            sleep 1
            sudo -n ip netns exec t1_0_1 env TT_UDP_OFFLOAD=0 "$T/large_s0" pub -s 131072 -r 0.25 -d "$DUR" -R -b 10.80.0.255 -P wifi >"$OUT/$run.ping" 2>&1
        fi
        wait $ep
        a1=$(txb t1_0_1 wlan7); a2=$(txb t1_0_2 wlan8)
        echo "txbytes ping=$((a1-b1)) echo=$((a2-b2))" >"$OUT/$run.txb"
        grep -m1 '^RESULT' "$OUT/$run.ping" >"$OUT/$run.result"
        echo "$run $(cat "$OUT/$run.txb") $(grep -o 'scored=[0-9]*\|retransmitted=[0-9]*\|p90_us=[0-9.]*\|published=[0-9]*' "$OUT/$run.result" | tr '\n' ' ') rstats_ping=$(grep -c 'RSTATS.*acknack_tx' "$OUT/$run.ping") rstats_echo=$(grep -c 'RSTATS.*acknack_tx' "$OUT/$run.echo")"
    done
done
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "S0_END $(date -Is)"

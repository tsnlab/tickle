#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# T1 diagnostic: rel_small p=0.024 with writer history KEEP_LAST 10 (node_base) vs 100 (node_d100, built from the same tree).
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6} TMPDIR=${T1_TMPDIR:-$HOME/tmp_build}
W=${W:-$(cd "$S/../../../.." && pwd)} # tree the nodes are built from
E=$W/examples/perf_hil/experiments
OUT=${RESULTS:-$HOME/rig_results_safe}/t1_depth/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_DEPTH_START $(date -Is) out=$OUT"
sed "s/^    return qos;/    qos.history.depth = 100;\n    return qos;/" "$E/wifi_link_node.c" >"$OUT/wifi_link_node_d100.c"
if cmp -s "$E/wifi_link_node.c" "$OUT/wifi_link_node_d100.c"; then echo "VOID: sed changed nothing"; sed -n 270,280p "$E/wifi_link_node.c"; exit 1; fi
diff "$E/wifi_link_node.c" "$OUT/wifi_link_node_d100.c" | head -6
cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS -I"$W/include" -I"$W/src" -I"$W/examples/perf_hil/tickle/common" \
    -o "$T/node_d100" "$OUT/wifi_link_node_d100.c" "$W/src/tickle.c" "$W/src/encoding.c" "$W/src/log.c" "$W/src/hal_linux.c" -lpthread -lm >"$OUT/build.log" 2>&1 \
    || { echo "VOID build"; tail -3 "$OUT/build.log"; exit 1; }
for r in 1 2 3 4 5; do
    for arm in D10 D100; do
        case $arm in D100) N=$T/node_d100 ;; *) N=$T/node_base ;; esac
        run=${arm}_r$r
        "$S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        BAD_P=0.012 LOSS=0.012 "$S/t1_topo.sh" up 1 "" >"$OUT/$run.topo" 2>&1
        grep -q "^T1_UP lanes" "$OUT/$run.topo" || { echo "VOID $run"; continue; }
        sudo -n ip netns exec t1_0_2 "$N" echo -c 2 -b 10.80.0.255 -d 50 -m rel -s 100 -r 100 -p wifi >"$OUT/$run.echo" 2>&1 &
        sleep 1
        sudo -n ip netns exec t1_0_1 "$N" ping -c 1 -b 10.80.0.255 -d 20 -m rel -s 100 -r 100 -p wifi >"$OUT/$run.ping" 2>&1
        sleep 4
        grep -m1 '^RESULT' "$OUT/$run.ping" >"$OUT/$run.result"
        echo "$run $(grep -o 'scored=[0-9]*\|retransmitted=[0-9]*\|gap_evicted=[0-9]*\|p50_us=[0-9.]*\|p99_us=[0-9.]*' "$OUT/$run.result" | tr '\n' ' ')"
    done
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_DEPTH_END $(date -Is)"

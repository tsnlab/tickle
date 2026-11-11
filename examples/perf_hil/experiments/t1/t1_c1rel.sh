#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# T1 test 1 (validation, rules V1/V2 in ~/claude_reports/T1_PLAN.md): be_small over one lane at link error p = 0, 0.1, 0.3,
# 3 reps each. Simulation only (t1_topo.sh refuses a phy that is not mac80211_hwsim).
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6}
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-t1_v1}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_V1_START $(date -Is) out=$OUT"
for p in ${P_LIST:-0.0 0.1 0.3}; do
    for rep in 1 2 3; do
        run=p${p}_r$rep
        "$S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        LOSS=$p "$S/t1_topo.sh" up 1 "" >"$OUT/$run.topo" 2>&1
        if ! grep -q "^T1_UP lanes" "$OUT/$run.topo"; then echo "VOID $run: no topology"; continue; fi
        echo "load=$(cut -d' ' -f1 /proc/loadavg) start=$(date -Is)" >"$OUT/$run.meta"
        sudo -n ip netns exec t1_0_2 "$T/node" echo -c 2 -b 10.80.0.255 -d 50 -m rel -s 100 -r 100 -p wifi \
            -l "$OUT/$run.owd" >"$OUT/$run.echo" 2>&1 &
        sleep 1
        sudo -n ip netns exec t1_0_1 "$T/node" ping -c 1 -b 10.80.0.255 -d 20 -m rel -s 100 -r 100 -p wifi \
            -l "$OUT/$run.rtt" >"$OUT/$run.ping" 2>&1
        sleep 4
        grep -m1 '^RESULT' "$OUT/$run.ping" >"$OUT/$run.result"
        echo "$run $(grep -o 'published=[0-9]*\|scored=[0-9]*\|p50_us=[0-9.]*\|p99_us=[0-9.]*' "$OUT/$run.result" | tr '\n' ' ')"
    done
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_V1_END $(date -Is)"

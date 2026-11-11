#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# T1 calibration of the Gilbert-Elliott overlay: be_small over one lane (station -> AP -> station), arms "name:good_ms,bad_ms".
# The bad state uses link error BAD_P (default 0.3, about 90% realized loss), the good state 0. Simulation only.
# ARMS and REPS may be set in the environment.
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6}
export BAD_P=${BAD_P:-0.3}
REPS=${REPS:-2}
ARMS=${ARMS:-"l1b20:3580,20 l1b40:7160,40 l1b80:14320,80 l5b20:700,20 l5b40:1400,40 l5b80:2800,80 l10b20:340,20 l10b40:680,40 l10b80:1360,80"}
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-t1_ge}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_GE_START $(date -Is) out=$OUT bad_p=$BAD_P reps=$REPS"
for arm in $ARMS; do
    name=${arm%%:*}
    ge=${arm#*:},0.0
    for rep in $(seq 1 "$REPS"); do
        run=${name}_r$rep
        "$S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        "$S/t1_topo.sh" up 1 "$ge" >"$OUT/$run.topo" 2>&1
        if ! grep -q "^T1_UP lanes" "$OUT/$run.topo"; then echo "VOID $run: no topology"; continue; fi
        echo "load=$(cut -d' ' -f1 /proc/loadavg) start=$(date -Is) ge=$ge" >"$OUT/$run.meta"
        sudo -n ip netns exec t1_0_2 "$T/node" echo -c 2 -b 10.80.0.255 -d 50 -m be -s 100 -r 100 -p wifi \
            -l "$OUT/$run.owd" >"$OUT/$run.echo" 2>&1 &
        sleep 1
        sudo -n ip netns exec t1_0_1 "$T/node" ping -c 1 -b 10.80.0.255 -d 20 -m be -s 100 -r 100 -p wifi \
            -l "$OUT/$run.rtt" >"$OUT/$run.ping" 2>&1
        sleep 4
        grep -m1 '^RESULT' "$OUT/$run.ping" >"$OUT/$run.result"
        echo "$run $(grep -o 'published=[0-9]*\|scored=[0-9]*\|p50_us=[0-9.]*\|p99_us=[0-9.]*' "$OUT/$run.result" | tr '\n' ' ')"
    done
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_GE_END $(date -Is)"

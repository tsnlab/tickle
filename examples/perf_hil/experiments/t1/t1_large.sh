#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# T1 rel_large (1 MiB reliable samples at 5 Hz, large_sample_bench from the dseed_ab_h tree = main ce4b411e) over one lane with the
# per-attempt retry model (RETRY=1). Waits for WAIT_FOR (a log with a marker) first. Profiles: name:bad_p:ge as in t1_ge2.sh.
# Simulation only.
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6} TMPDIR=${T1_TMPDIR:-$HOME/tmp_build}
if [ -n "${WAIT_FOR:-}" ]; then
    until grep -q "${WAIT_MARK:-T1_GE2_END}" "$WAIT_FOR" 2>/dev/null; do sleep 5; done
fi
REPO=${REPO:-$(cd "$S/../../../.." && pwd)} # tree the benchmark is built from
E=$REPO/examples/perf_hil/experiments
REPS=${REPS:-2}
DUR=${DUR:-30}
HZ=${HZ:-5}
PROFILES=${PROFILES:-"ctl:0.0:1000000,1,0.0 psu_cs:0.44:504,28,0.01,9.7,1.0,46.5 o60:0.9:1140,60,0.01 o200:0.9:3800,200,0.01"}
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-t1_large}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_LARGE_START $(date -Is) out=$OUT dur=$DUR"
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS \
    -I"$REPO/include" -I"$REPO/src" -o "$T/large" "$E/large_sample_bench.c" \
    "$REPO/src/tickle.c" "$REPO/src/encoding.c" "$REPO/src/log.c" "$REPO/src/hal_linux.c" -lpthread -lm >"$OUT/build.log" 2>&1; then
    echo "VOID large did not build"; tail -3 "$OUT/build.log"; exit 1
fi
for prof in $PROFILES; do
    name=${prof%%:*}
    rest=${prof#*:}
    badp=${rest%%:*}
    ge=${rest#*:}
    for rep in $(seq 1 "$REPS"); do
        run=${name}_r$rep
        "$S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        RETRY=1 BAD_P=$badp "$S/t1_topo.sh" up 1 "$ge" >"$OUT/$run.topo" 2>&1
        grep -q "^T1_UP lanes" "$OUT/$run.topo" || { echo "VOID $run"; continue; }
        sudo -n ip netns exec t1_0_2 env TT_UDP_OFFLOAD=0 "$T/large" sub -s "${SZ:-131072}" -d $((DUR + 8)) -R -b 10.80.0.255 -P wifi \
            >"$OUT/$run.sub" 2>&1 &
        sleep 1
        sudo -n ip netns exec t1_0_1 env TT_UDP_OFFLOAD=0 "$T/large" pub -s "${SZ:-131072}" -r "${HZ:-5}" -d "$DUR" -R -b 10.80.0.255 -P wifi \
            >"$OUT/$run.pub" 2>&1
        sleep 10
        echo "$run pub: $(grep -m1 '^RESULT' "$OUT/$run.pub" | grep -o 'published=[0-9]*\|retransmitted=[0-9]*\|large_abandoned=[0-9]*\|large_dropped=[0-9]*' | tr '\n' ' ') sub: $(grep -m1 '^RESULT' "$OUT/$run.sub" | cut -c1-260)"
    done
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_LARGE_END $(date -Is)"

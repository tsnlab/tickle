#!/usr/bin/env bash
# X0a (TIME_VARYING experiment plan): is the clean path's reordering of large samples (T1 2026-11-02: s_l0 rel_large,
# lossless 1 ms, resends in 5 of 5 reps) made by CPU scheduling on this PC? Two arms, the same s_l0 rel_large cell,
# REPS each: nodes unpinned (as T1), then each node on a CPU of its own (NODE_PIN). Reads each arm's units resent.
# PRE-REGISTERED: pinned resends 0 in every rep and unpinned > 0 in at least 3 -> SCHEDULING (a netns/veth artefact:
# the control gets pinned); pinned > 0 in at least 3 reps -> NOT_SCHEDULING (reorder source still open: next RPS and
# delay 6 ms); anything else -> UNCLEAR. A missing RESULT voids the run (time_varying_read.py's rules).
# Usage: setsid nohup x0a_reorder_pin.sh > log 2>&1 < /dev/null &   Env: REPS (5) PIN ("2 4 6") OUT
# Last line always "X0A_END <verdict>".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPS=${REPS:-5}
PIN=${PIN:-"2 4 6"}
OUT=${OUT:-$HOME/rig_results_safe/time_varying/x0a_$(date +%Y%m%d-%H%M%S)}
verdict=VOID
trap 'echo "X0A_END $verdict out=$OUT"' EXIT
mkdir -p "$OUT" || exit 1
for arm in unpinned pinned; do
    pin=""
    [ "$arm" = pinned ] && pin=$PIN
    NODE_PIN="$pin" ARMS=s_l0 CELLS=rel_large REPS=$REPS OUT="$OUT/$arm" "$HERE/time_varying_netns.sh" \
        >"$OUT/$arm.log" 2>&1
    echo "$arm rc=$? $(tail -1 "$OUT/$arm/summary.txt" 2>/dev/null)"
done
# units resent per run: the large publisher's (ping.log) first retransmitted= count
count() { # dir -> one number per rep, "-" when the run has no RESULT
    local r f n
    for r in $(seq 1 "$REPS"); do
        f="$1/runs/s_l0-rel_large-$r/ping.log"
        n=$(grep -m1 -o "retransmitted=[0-9]*" "$f" 2>/dev/null | cut -d= -f2)
        echo -n "${n:--} "
    done
}
u=($(count "$OUT/unpinned")); p=($(count "$OUT/pinned"))
echo "unpinned resent: ${u[*]}"
echo "pinned   resent: ${p[*]}"
if [ "${#u[@]}" -ne "$REPS" ] || [ "${#p[@]}" -ne "$REPS" ] || printf '%s\n' "${u[@]}" "${p[@]}" | grep -qx -- -; then
    verdict=VOID
else
    pz=0; pn=0; un=0
    for x in "${p[@]}"; do if [ "$x" -eq 0 ]; then pz=$((pz + 1)); else pn=$((pn + 1)); fi; done
    for x in "${u[@]}"; do [ "$x" -gt 0 ] && un=$((un + 1)); done
    if [ "$pz" -eq "$REPS" ] && [ "$un" -ge 3 ]; then verdict=SCHEDULING
    elif [ "$pn" -ge 3 ]; then verdict=NOT_SCHEDULING
    else verdict=UNCLEAR; fi
fi

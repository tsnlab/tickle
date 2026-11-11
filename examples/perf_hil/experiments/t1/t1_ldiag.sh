#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# rel_large diagnosis in T1: one no-loss run, interface counters and AP/bridge state sampled while the run is live.
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6}
OUT=${RESULTS:-$HOME/rig_results_safe}/t1_ldiag/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_LDIAG_START $(date -Is) out=$OUT"
"$S/t1_topo.sh" down >/dev/null 2>&1
sleep 1
RETRY=1 BAD_P=0.0 TRACE="$OUT/wm.trace" "$S/t1_topo.sh" up 1 "1000000,1,0.0" >"$OUT/topo" 2>&1
for ns in t1_0_0 t1_0_1 t1_0_2; do
    sudo -n ip netns exec $ns ip -s link >"$OUT/$ns.link.before" 2>&1
done
sudo -n ip netns exec t1_0_2 env TT_UDP_OFFLOAD=0 "$T/large" sub -s "${SZ:-1048576}" -d 25 -R -b 10.80.0.255 -P wifi >"$OUT/sub" 2>&1 &
sleep 1
sudo -n ip netns exec t1_0_1 env TT_UDP_OFFLOAD=0 "$T/large" pub -s "${SZ:-1048576}" -r "${HZ:-1}" -d 15 -R -b 10.80.0.255 -P wifi >"$OUT/pub" 2>&1 &
sleep 8
for ns in t1_0_0 t1_0_1 t1_0_2; do
    sudo -n ip netns exec $ns ip -s link >"$OUT/$ns.link.mid" 2>&1
    sudo -n ip netns exec $ns ip -4 addr >"$OUT/$ns.addr" 2>&1
    sudo -n ip netns exec $ns nstat -az >"$OUT/$ns.nstat" 2>&1
done
sleep 20
for ns in t1_0_0 t1_0_1 t1_0_2; do
    sudo -n ip netns exec $ns ip -s link >"$OUT/$ns.link.after" 2>&1
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_LDIAG_END $(date -Is)"

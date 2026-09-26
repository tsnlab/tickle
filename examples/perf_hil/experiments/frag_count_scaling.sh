#!/usr/bin/env bash
# How TickLE's recovery cost scales with fragment count - the case the per-datagram seq_no decision
# (rmw_tickle/DATAFRAG_PLAN.md section 13) exists for. TickLE only: it measures TickLE against its own
# theory, not against the vendors.
#
# The same 2800 B sample (p4) is sent two ways: the default 1472 B datagram (2 fragments) and
# TICKLE_DATAGRAM_BYTES=800 (4 fragments), each unshaped and under 5% netem loss. SHA is pinned by
# the caller, so a run meant as the "before" arm cannot drift onto the change it is the baseline for.
#
# HOW TO READ IT, written before running. Client transmitted datagrams per delivered sample
# (wire_role_packets_per_sample), median of REPS:
#   IDENTITY: unshaped, 2 fragments must read ~2.0 and 4 fragments ~4.0, and every TickLE row must say
#     datagram_bytes=1472 or =800 as its arm claims. Otherwise the arm is not what it is named.
#   BEFORE the change (whole-sample retransmission): 5% loss should read near 2.216 for 2 fragments
#     and near 4.911 for 4. If the 4-fragment arm already reads near 4.211, the current code is
#     already per-datagram in effect and section 13's premise is wrong - that is reported, not absorbed.
#   AFTER the change (per-datagram): 2.105 and 4.211. The 4-fragment gap between the arms is the
#     claim; at 2 fragments the difference (5%) is close to run-to-run spread and is not relied on.
#   drained=acked is required in every row; a timeout makes the row a delivery failure, not a figure.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
SHA=${SHA:?set SHA to the commit this arm measures}
REPS=${REPS:-3}
OUT=${OUT:-/tmp/frag_count_scaling_$(date +%Y%m%d-%H%M%S).txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
tc_set() {
    case "$1" in
        off) sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true ;;
        on) sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss 5%" ;;
    esac
    local q
    q=$(sh_ "$CLIENT" "tc qdisc show dev eth0 | head -1")
    if [ "$1" = on ]; then
        case "$q" in *"loss 5%"*) ;; *) echo "netem not applied: $q" >&2; exit 1 ;; esac
    else
        case "$q" in *netem*) echo "netem still present: $q" >&2; exit 1 ;; esac
    fi
}
trap 'tc_set off' EXIT
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== fragment-count scaling, $(date -Is), SHA $SHA, $REPS reps ==="
build() { # $1 = datagram bytes, or "" for the default
    local dg="$1" pids=() bad=0 h
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle
TICKLE_P4_PATH=frag TICKLE_DATAGRAM_BYTES=$dg ./build.sh reliable_throughput p4 >/tmp/fcs_build.log 2>&1 || { echo BUILD-FAIL; tail -6 /tmp/fcs_build.log; exit 1; }" &
        pids+=($!)
    done
    for h in "${pids[@]}"; do wait "$h" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED (datagram=${dg:-default}) - stopping"; exit 1; }
    [ "$(sh_ "$CLIENT" 'git -C ~/tickle rev-parse HEAD')" = "$SHA" ] || { say "IDENTITY FAIL: rig not at $SHA"; exit 1; }
}
arm() { # $1 label, $2 expected datagram_bytes, $3 loss on|off
    local label="$1" want="$2" loss="$3" r res
    tc_set "$loss"
    for r in $(seq 1 "$REPS"); do
        res=$(cd "$REPO/examples/perf_hil/tickle" && RIG_LOCK_HELD_HIL=1 timeout 180 ./run_scenario.sh reliable_throughput_p4 -d 5 -Q 2>/dev/null | tr '\n' ' ')
        # Every raw RESULT line is kept (2026-09-26): the first runs printed selected fields only, so a
        # later question about recovery_srtt_ns could not be answered from them.
        printf '%s | %s\n' "$label rep$r" "$res" >>"$OUT.raw"
        case "$res" in *"datagram_bytes=$want"*) ;; *) say "$label rep$r | IDENTITY FAIL: not datagram_bytes=$want"; continue ;; esac
        case "$res" in *core_build=release*) ;; *) say "$label rep$r | IDENTITY FAIL: not release"; continue ;; esac
        say "$label rep$r | $(grep -oE '(sent|recv|drained|send_mbps|wire_role_packets_per_sample|frag_duplicate|frag_abandoned)=[^ ]+' <<<"$res" | tr '\n' ' ')"
    done
}
build ""
arm "frag2 loss0" 1472 off
arm "frag2 loss5" 1472 on
build 800
arm "frag4 loss0" 800 off
arm "frag4 loss5" 800 on
tc_set off
say ""; say "=== done ==="

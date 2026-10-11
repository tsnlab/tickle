#!/usr/bin/env bash
# large_saturation_pc.sh - RESULTS L4 cell I4s on the PC: a 4 MB best-effort stream at 30 Hz through a 1 Gbit/s link
# (tbf on the publisher's egress, two private network namespaces), receive buffers granted 425,984 bytes as on the
# rig's Pis, and a subscriber that spends time on each sample - inside the callback (-w, as rmw_tickle decodes a large
# message inside the poll) and/or after the poll (-W, as an application handles it). Two arms, interleaved ABBA per
# rep: PARENT, the tree before core asks for the receive drain, and HEAD, with it (tt_rx_drain_start(), hal.h).
#
# NOT a rig figure: it answers whether the drain removes the subscriber-side loss the mechanism predicts. The rig task
# (docs/ROADMAP.md, I4s) is what decides the cell.
#
# HOW TO READ IT, written before the first run (2026-10-11) and enforced by the summary at the end:
#   TREATMENT  every HEAD run reports drain_starts > 0 and every PARENT run drain_starts = 0, else the arms are not what
#              they say and the run is VOID.
#   S0 CONTROL (w 0, W 0): both arms deliver; torn = 0. Their abandoned/socket_drops are this PC's background.
#   S1 REPRO   PARENT, in a stalled cell: socket_drops > 0 and abandoned > 0 in every rep. A stalled cell where it
#              does not is NOT REPRODUCED and S2 says nothing about it.
#   S2 FIX     HEAD, in each reproduced cell: socket_drops = 0 in at least 2 of 3 reps and its total abandoned at most
#              10% of PARENT's there: PASS; otherwise FAIL.
#   S3 SAFETY  torn = 0 and out_of_order = 0 in every run; in the control cell HEAD's delivered mean is not below
#              PARENT's by more than 2 standard errors of the difference: else REGRESSION.
#   S4 COST    subscriber cpu_ms_per_mb, HEAD against PARENT, per cell: printed; FLAG when HEAD is more than 10% dearer
#              in the control cell (the drain runs there too, between polls).
#
# Usage: large_saturation_pc.sh PARENT_SHA HEAD_SHA [REPS] [DURATION_S]    results to $OUT
#   EXTRA="label:sha ..." adds arms, each read against PARENT as HEAD is (same S1-S4; the treatment check expects
#   drain_starts > 0 of every arm but PARENT), all interleaved in an order that rotates per rep, so arms measured on a
#   loaded PC share its load.
# Needs passwordless `sudo ip` and `tc`. Run it detached (CLAUDE.md, "Long-running work").
set -u
PARENT=${1:?usage: large_saturation_pc.sh PARENT_SHA HEAD_SHA [REPS] [DURATION_S]}
HEAD_SHA=${2:?usage: large_saturation_pc.sh PARENT_SHA HEAD_SHA [REPS] [DURATION_S]}
REPS=${3:-3}
DURATION=${4:-20}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$HERE/../../.." && git rev-parse --show-toplevel)}
WORK=${WORK:-$HOME/largemsg_pc/saturation}
OUT=${OUT:-$WORK/results.txt}
NS1=lgsat-pub-$$
NS2=lgsat-sub-$$
SIZE=4194304
RATE=30

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    for wt in "$WORK"/wt_*; do
        [ -d "$wt" ] && git -C "$REPO" worktree remove --force "$wt" 2>/dev/null
    done
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK"
# Each arm built from its own worktree at its sha, the bench from HEAD's (it reads only counters both trees have).
build() { # <arm> <sha>
    local arm=$1 sha=$2 wt=$WORK/wt_$1
    git -C "$REPO" worktree remove --force "$wt" 2>/dev/null
    git -C "$REPO" worktree add --detach "$wt" "$sha" >/dev/null 2>&1 || return 1
    cc -O2 -DNDEBUG -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$wt/include" -I"$wt/src" -o "$WORK/bench_$arm" \
        "$WORK/wt_head/examples/perf_hil/experiments/large_sample_bench.c" "$wt/src/tickle.c" "$wt/src/encoding.c" \
        "$wt/src/log.c" "$wt/src/hal_linux.c" -lpthread -lm
}
build head "$HEAD_SHA" || { echo "build head failed"; exit 1; }
build parent "$PARENT" || { echo "build parent failed"; exit 1; }
ARMS="parent head"
for extra in ${EXTRA:-}; do
    build "${extra%%:*}" "${extra#*:}" || { echo "build ${extra%%:*} failed"; exit 1; }
    ARMS="$ARMS ${extra%%:*}"
done

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add lgsat1 netns "$NS1" type veth peer name lgsat2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.78.1/24 dev lgsat1
sudo -n ip -n "$NS2" addr add 192.168.78.2/24 dev lgsat2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set lgsat1 up
sudo -n ip -n "$NS2" link set lgsat2 up
sudo -n ip netns exec "$NS1" tc qdisc add dev lgsat1 root tbf rate 1gbit burst 256kb latency 100ms || exit 1

echo "=== large_saturation_pc $(date -Is) parent=$PARENT head=$HEAD_SHA extra=${EXTRA:-} reps=$REPS duration=${DURATION}s size=$SIZE rate=$RATE ===" | tee -a "$OUT"
start_line=$(wc -l <"$OUT")

run() { # <arm> <w_us> <W_us> <rep>
    local arm=$1 w=$2 big_w=$3 rep=$4 bin=$WORK/bench_$1
    local sublog=$WORK/sub.log publog=$WORK/pub.log
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" "$bin" sub -s "$SIZE" -d $((DURATION + 8)) -b 192.168.78.255 -w "$w" -W "$big_w" \
        >"$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" "$bin" pub -s "$SIZE" -r "$RATE" -d "$DURATION" -b 192.168.78.255 >"$publog" 2>&1
    wait "$sub_pid"
    local p s
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN arm=$arm w=$w W=$big_w rep=$rep VOID: no RESULT (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    echo "RUN arm=$arm w=$w W=$big_w rep=$rep | $p | $s" | tee -a "$OUT"
}

for cellspec in "0 0" "3000 0" "0 5000" "3000 5000"; do
    read -r w big_w <<<"$cellspec"
    for rep in $(seq 1 "$REPS"); do
        read -r -a order <<<"$ARMS"
        n=${#order[@]}
        for i in $(seq 0 $((n - 1))); do run "${order[$(((i + rep) % n))]}" "$w" "$big_w" "$rep"; done
    done
done
echo "=== runs done $(date -Is) ===" | tee -a "$OUT"

tail -n +"$((start_line + 1))" "$OUT" | grep '^RUN arm=' | grep -v VOID | python3 -c '
import re, sys, statistics as st
runs = []
for line in sys.stdin:
    head, pub, sub = line.split("|")
    r = dict(re.findall(r"(\w+)=(\S+)", head))
    p = dict(re.findall(r"(\w+)=(\S+)", pub))
    s = dict(re.findall(r"(\w+)=(\S+)", sub))
    num = lambda d, k: float(d.get(k, "nan"))
    runs.append({"arm": r["arm"], "cell": (int(r["w"]), int(r["W"])), "published": num(p, "published"),
        "tail_abandoned": num(p, "large_tail_abandoned"), "delivered": num(s, "delivered"), "torn": num(s, "torn"),
        "ooo": num(s, "out_of_order"), "abandoned": num(s, "large_abandoned"), "drops": num(s, "socket_drops"),
        "starts": num(s, "drain_starts"), "cpu": num(s, "cpu_ms_per_mb"), "lat": num(s, "latency_median_us")})
verdict = {}
bad = [x for x in runs if (x["arm"] != "parent") != (x["starts"] > 0)]
arms = ["parent"] + sorted({x["arm"] for x in runs} - {"parent"})
verdict["TREATMENT"] = "PASS" if runs and not bad else "VOID (%d runs whose drain_starts contradict their arm)" % len(bad)
def sel(arm, cell): return [x for x in runs if x["arm"] == arm and x["cell"] == cell]
cells = sorted({x["cell"] for x in runs})
for c in cells:
    for arm in arms:
        xs = sel(arm, c)
        print("SUMMARY w=%-4d W=%-4d %-6s delivered %s abandoned %s socket_drops %s tail_abandoned %s torn %s cpu_ms_per_mb %s lat_med_ms %s" % (
            c[0], c[1], arm, [int(x["delivered"]) for x in xs], [int(x["abandoned"]) for x in xs], [int(x["drops"]) for x in xs],
            [int(x["tail_abandoned"]) for x in xs], [int(x["torn"]) for x in xs], [round(x["cpu"], 3) for x in xs],
            [round(x["lat"] / 1000, 1) for x in xs]))
ctl = (0, 0)
verdict["S0"] = "PASS" if all(sel(a, ctl) for a in arms) and all(x["torn"] == 0 for a in arms for x in sel(a, ctl)) else "VOID"
repro = [c for c in cells if c != ctl and sel("parent", c) and all(x["drops"] > 0 and x["abandoned"] > 0 for x in sel("parent", c))]
verdict["S1"] = "REPRODUCED in %s" % repro if repro else "NOT REPRODUCED"
for arm in arms[1:]:
    s2 = []
    for c in repro:
        h, p = sel(arm, c), sel("parent", c)
        ok = len([x for x in h if x["drops"] == 0]) >= 2 and sum(x["abandoned"] for x in h) <= 0.10 * sum(x["abandoned"] for x in p)
        s2.append("%s %s (abandoned %d vs %d)" % (c, "PASS" if ok else "FAIL", sum(x["abandoned"] for x in h), sum(x["abandoned"] for x in p)))
    verdict["S2 " + arm] = "; ".join(s2) if s2 else "NO READING"
    unsafe = [x for x in sel(arm, ctl) + [y for c in cells for y in sel(arm, c)] if x["torn"] != 0 or x["ooo"] != 0]
    h, p = [x["delivered"] for x in sel(arm, ctl)], [x["delivered"] for x in sel("parent", ctl)]
    reg = ""
    if len(h) > 1 and len(p) > 1:
        se = (st.variance(h) / len(h) + st.variance(p) / len(p)) ** 0.5
        if st.mean(h) < st.mean(p) - 2 * se:
            reg = "REGRESSION: control delivered %.1f vs %.1f (2 SE %.1f)" % (st.mean(h), st.mean(p), 2 * se)
    verdict["S3 " + arm] = ("FAIL (%d runs torn or out of order)" % len(unsafe) if unsafe else "PASS") + ("; " + reg if reg else "")
    cost = []
    for c in cells:
        hc, pc = [x["cpu"] for x in sel(arm, c)], [x["cpu"] for x in sel("parent", c)]
        if hc and pc:
            ratio = st.median(hc) / st.median(pc)
            cost.append("%s %+.1f%%%s" % (c, (ratio - 1) * 100, " FLAG" if c == ctl and ratio > 1.10 else ""))
    verdict["S4 " + arm] = "; ".join(cost)
for k in sorted(verdict):
    print("VERDICT %s %s" % (k, verdict[k]))
' | tee -a "$OUT"
echo "=== done $(date -Is) ===" | tee -a "$OUT"

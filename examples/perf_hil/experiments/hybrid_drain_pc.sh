#!/usr/bin/env bash
# hybrid_drain_pc.sh - the hybrid transport with the receive drain, on the PC: cell I4s's shape (4 MB at 30 Hz through
# a 1 Gbit/s link: tbf on the publisher's egress, two private network namespaces, receive buffers granted 425,984 bytes
# as on the rig's Pis) with AUTO fragmentation at 0% loss, best effort and RELIABLE, and a subscriber that takes its
# time - 3 ms in each sample's callback (-w, as rmw_tickle decodes inside the poll) and 5 ms after each delivering poll
# (-W, as an application handles it) - beside a control cell that takes none. Arms, interleaved in an order that rotates
# per rep: PARENT, the hybrid transport without the drain (ab/hybrid-land, d8980f9b), and HEAD, the two combined.
#
# Why (2026-10-12): on the rig, ab/hybrid-land at 4 MB and 0% loss overflowed the subscriber's receive socket
# (RcvbufErrors 58-148 a best-effort run) and its RELIABLE runs were VOID - AUTO's n stayed at 4-31 instead of 45,
# because the reader counted the socket's drops as path loss. HEAD drains the sockets while core is away (buffers sized
# for a run, the ceiling the reader advertises) and keeps any drop that still happens out of the loss the control law
# reads (tickle.c, "Receiver drops are not path loss").
#
# NOT a rig figure: it says whether the combined branch is worth the rig's time (~/rig_queue_hybrid_drain.sh decides).
#
# HOW TO READ IT, written before the first run (2026-10-12) and enforced by the summary at the end:
#   TREATMENT  every HEAD run in a stalled cell reports drain_starts > 0; no PARENT run reports a drain. Else VOID.
#   C0 CONTROL (w 0, W 0): both arms deliver and torn = 0; their socket drops are this PC's background.
#   R1 REPRO   PARENT, stalled, per QoS: socket_drops > 0 in every rep - the failure the rig showed. Otherwise NOT
#              REPRODUCED, and D1 for that QoS has no contrast (it is still read).
#   D1 DROPS   HEAD, every run of every cell: socket_drops = 0 and drain_truncated = 0. A run cut by the drain or a
#              socket that still overflowed: FAIL.
#   N45        HEAD, every run at 0% loss: n reached n_max = 45 - the publisher's n histogram has its n_max bucket as
#              its largest, and the subscriber's cap ends at 45. Any run below: FAIL. PARENT's are printed beside it.
#   S3 SAFETY  torn = 0 and out_of_order = 0 in every run of every arm (the bench checks every byte of every sample).
#   S4 SAMPLES HEAD's delivered against PARENT's per cell: printed; FLAG where HEAD's mean is below PARENT's by more
#              than 2 standard errors of the difference.
#   S5 COST    subscriber cpu_ms_per_mb, HEAD against PARENT, per cell: printed; FLAG above +10% in the control cell.
# Falsified: D1 failing with the treatment shown (the drain does not keep a busy receiver's socket from overflowing when
# runs arrive), or N45 failing with D1 passing (something other than receiver drops holds n down).
#
# Usage: hybrid_drain_pc.sh PARENT_SHA HEAD_SHA [REPS] [DURATION_S]    results appended to $OUT
#   MTU_ARM=1 adds "headmtu": HEAD's binary with the publisher forced to MTU fragments (-F mtu) - the drain's buffers
#   are sized for a run whatever the writer sends, so an MTU-mode stream has 64 of them, not 2,048 (receive offload
#   packs a merged read into one); its socket drops are printed, not judged.
# Needs passwordless `sudo ip` and `tc`. Run it detached (CLAUDE.md, "Long-running work"). Every bench process runs
# under `ulimit -v` and a timeout.
set -u
PARENT=${1:?usage: hybrid_drain_pc.sh PARENT_SHA HEAD_SHA [REPS] [DURATION_S]}
HEAD_SHA=${2:?usage: hybrid_drain_pc.sh PARENT_SHA HEAD_SHA [REPS] [DURATION_S]}
REPS=${3:-3}
DURATION=${4:-20}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$HERE/../../.." && git rev-parse --show-toplevel)}
WORK=${WORK:-$HOME/largemsg_pc/hybrid_drain}
OUT=${OUT:-$WORK/results.txt}
NS1=hdrn-pub-$$
NS2=hdrn-sub-$$
SIZE=4194304
RATE=30
VMEM_KB=${VMEM_KB:-2097152}

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
# Each arm from its own worktree at its sha, the bench from HEAD's (it reads the counters a tree has, and only those).
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
if [ "${MTU_ARM:-0}" = 1 ]; then
    cp "$WORK/bench_head" "$WORK/bench_headmtu"
    ARMS="$ARMS headmtu"
fi

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add hdrn1 netns "$NS1" type veth peer name hdrn2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.79.1/24 dev hdrn1
sudo -n ip -n "$NS2" addr add 192.168.79.2/24 dev hdrn2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set hdrn1 up
sudo -n ip -n "$NS2" link set hdrn2 up
sudo -n ip netns exec "$NS1" tc qdisc add dev hdrn1 root tbf rate 1gbit burst 256kb latency 100ms || exit 1

echo "=== hybrid_drain_pc $(date -Is) parent=$PARENT head=$HEAD_SHA arms=$ARMS reps=$REPS duration=${DURATION}s size=$SIZE rate=$RATE load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"
start_line=$(wc -l <"$OUT")

run() { # <arm> <qos: be|rel> <w_us> <W_us> <rep>
    local arm=$1 qos=$2 w=$3 big_w=$4 rep=$5 bin=$WORK/bench_$1
    local sublog=$WORK/sub.log publog=$WORK/pub.log qflag="" fflag=""
    [ "$qos" = rel ] && qflag="-R"
    [ "$arm" = headmtu ] && fflag="-F mtu"
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin sub -s $SIZE \
        -d $((DURATION + 8)) $qflag -b 192.168.79.255 -w $w -W $big_w" >"$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $bin pub -s $SIZE \
        -r $RATE -d $DURATION $qflag $fflag -b 192.168.79.255" >"$publog" 2>&1
    wait "$sub_pid"
    local p s
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN arm=$arm qos=$qos w=$w W=$big_w rep=$rep VOID: no RESULT (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    echo "RUN arm=$arm qos=$qos w=$w W=$big_w rep=$rep | $p | $s" | tee -a "$OUT"
}

for qos in be rel; do
    for cellspec in "0 0" "3000 5000"; do
        read -r w big_w <<<"$cellspec"
        for rep in $(seq 1 "$REPS"); do
            read -r -a order <<<"$ARMS"
            n=${#order[@]}
            for i in $(seq 0 $((n - 1))); do run "${order[$(((i + rep) % n))]}" "$qos" "$w" "$big_w" "$rep"; done
        done
    done
done
echo "=== runs done $(date -Is) load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

tail -n +"$((start_line + 1))" "$OUT" | grep '^RUN arm=' | python3 -c '
import re, sys, statistics as st
runs, void = [], 0
for line in sys.stdin:
    if " VOID:" in line:
        void += 1
        continue
    head, pub, sub = line.split("|")
    r = dict(re.findall(r"(\w+)=(\S+)", head))
    p = dict(re.findall(r"(\w+)=(\S+)", pub))
    s = dict(re.findall(r"(\w+)=(\S+)", sub))
    num = lambda d, k: float(d.get(k, "nan"))
    hist = [int(x) for x in p.get("hybrid_n_hist", "").split("/") if x]
    runs.append({"arm": r["arm"], "qos": r["qos"], "cell": (int(r["w"]), int(r["W"])),
        "published": num(p, "published"), "delivered": num(s, "delivered"), "torn": num(s, "torn"),
        "ooo": num(s, "out_of_order"), "abandoned": num(s, "large_abandoned"), "drops": num(s, "socket_drops"),
        "starts": float(s.get("drain_starts", "0")), "cut": float(s.get("drain_truncated", "0")),
        "merged": float(s.get("drain_merged", "0")), "cap": num(s, "hybrid_cap"), "hist": hist,
        "rlost": float(s.get("hybrid_receiver_lost", "nan")), "cpu": num(s, "cpu_ms_per_mb"),
        "lat": num(s, "latency_median_us")})
ctl, stalled = (0, 0), (3000, 5000)
arms = ["parent", "head"] + sorted({x["arm"] for x in runs} - {"parent", "head"})
def sel(arm, qos, cell): return [x for x in runs if x["arm"] == arm and x["qos"] == qos and x["cell"] == cell]
def nmax_top(x): return len(x["hist"]) == 7 and x["hist"][6] > 0 and x["hist"][6] == max(x["hist"])
verdict = {}
if void:
    verdict["VOID_RUNS"] = "%d runs printed no RESULT" % void
bad = [x for x in runs if (x["arm"] == "parent" and x["starts"] > 0) or
       (x["arm"] != "parent" and x["cell"] == stalled and not x["starts"] > 0)]
verdict["TREATMENT"] = "PASS" if runs and not bad else "VOID (%d runs whose drain_starts contradict their arm)" % len(bad)
for qos in ("be", "rel"):
    for c in (ctl, stalled):
        for arm in arms:
            xs = sel(arm, qos, c)
            print("SUMMARY %-3s w=%-4d W=%-4d %-7s delivered %s socket_drops %s drain_truncated %s drain_starts %s merged %s cap %s n_hist %s receiver_lost %s torn %s cpu_ms_per_mb %s lat_med_ms %s" % (
                qos, c[0], c[1], arm, [int(x["delivered"]) for x in xs], [int(x["drops"]) for x in xs],
                [int(x["cut"]) for x in xs], [int(x["starts"]) for x in xs], [int(x["merged"]) for x in xs],
                [x["cap"] for x in xs], ["/".join(map(str, x["hist"])) for x in xs], [x["rlost"] for x in xs],
                [int(x["torn"]) for x in xs], [round(x["cpu"], 3) for x in xs], [round(x["lat"] / 1000, 1) for x in xs]))
    verdict["C0 " + qos] = "PASS" if all(sel(a, qos, ctl) for a in ("parent", "head")) and all(
        x["torn"] == 0 for a in ("parent", "head") for x in sel(a, qos, ctl)) else "VOID"
    p = sel("parent", qos, stalled)
    verdict["R1 " + qos] = "REPRODUCED" if p and all(x["drops"] > 0 for x in p) else "NOT REPRODUCED"
    h = sel("head", qos, ctl) + sel("head", qos, stalled)
    fails = [x for x in h if x["drops"] != 0 or x["cut"] != 0]
    verdict["D1 " + qos] = ("PASS" if h and not fails else "FAIL (%d of %d runs with socket drops or a cut run)" % (len(fails), len(h)))
    low = [x for x in h if not (nmax_top(x) and x["cap"] == 45)]
    verdict["N45 " + qos] = ("PASS" if h and not low else "FAIL (%d of %d runs below n_max)" % (len(low), len(h)))
    pn = sel("parent", qos, ctl) + sel("parent", qos, stalled)
    verdict["N45 " + qos] += "; parent at n_max in %d of %d" % (len([x for x in pn if nmax_top(x) and x["cap"] == 45]), len(pn))
    for c in (ctl, stalled):
        hd, pd = [x["delivered"] for x in sel("head", qos, c)], [x["delivered"] for x in sel("parent", qos, c)]
        s4 = "head %s parent %s" % (round(st.mean(hd), 1) if hd else "-", round(st.mean(pd), 1) if pd else "-")
        if len(hd) > 1 and len(pd) > 1:
            se = (st.variance(hd) / len(hd) + st.variance(pd) / len(pd)) ** 0.5
            if st.mean(hd) < st.mean(pd) - 2 * se:
                s4 += " FLAG (below by more than 2 SE %.1f)" % (2 * se)
        verdict["S4 %s %s" % (qos, c)] = s4
        hc, pc = [x["cpu"] for x in sel("head", qos, c)], [x["cpu"] for x in sel("parent", qos, c)]
        if hc and pc:
            ratio = st.median(hc) / st.median(pc)
            verdict["S5 %s %s" % (qos, c)] = "%+.1f%%%s" % ((ratio - 1) * 100, " FLAG" if c == ctl and ratio > 1.10 else "")
unsafe = [x for x in runs if x["torn"] != 0 or x["ooo"] != 0]
verdict["S3"] = "PASS" if runs and not unsafe else "FAIL (%d runs torn or out of order)" % len(unsafe)
for k in sorted(verdict):
    print("VERDICT %s %s" % (k, verdict[k]))
' | tee -a "$OUT"
echo "=== done $(date -Is) ===" | tee -a "$OUT"

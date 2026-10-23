#!/usr/bin/env bash
# large_repair_followups_netns.sh - the follow-ups the reliable HEARTBEAT fix left open (CHANGELOG, "A reliable HEARTBEAT
# went out with batched broadcast content"; RESULTS WB/WBS/WR), measured before anything is changed. 2026-10-23.
# The WB harness (wifi_burst_loss_netns.sh) does the running: private namespaces, PROFILE=ethernet, offload off, RSTATS
# on every node. Several trees run rep by rep in alternation (never at once: the PC is shared).
#
#   (a) the amplifier for large samples: a 1 ms first retry (retry_initial, nothing timed yet) and no per-unit resend
#       hold-down at the writer, so a reader that asks again while a repair is still queued gets it again.
#   (b) small DATA with a batched discovery summary goes broadcast.
#   (c) tt_Publisher_request_ack() flushes batched broadcast-only content (a summary, an announce) to its peers alone.
#
# PHASES (PHASE=routes|amp|both, default both):
#   routes  (b) and (c). The WB shape (two prio bands, broadcast in its own netem, slot 1-4 ms x 42), arms ctl and
#           ge5b4, cells rel_small (100 B RELIABLE KEEP_LAST 10 at 100 Hz, echoed) and rel_large (1 MiB RELIABLE at
#           5 Hz), and rel_large again with a KEEP_ALL writer (-A, OUT/keepall), which solicits acknowledgements
#           (tt_Publisher_request_ack()) and is the one cell here whose writer calls it. Counted per node by RSTATS in
#           flush_tx(): broadcast_with_data (a datagram broadcast with a sample DATA in it), broadcast_with_reliable_control
#           (one broadcast with a non-discovery HEARTBEAT or ACKNACK), addressed_with_discovery (one sent to peers while
#           it held a batched announce or summary - which then reached those peers alone).
#   amp     (a), on a rig-like ETHERNET path: one band (broadcast shares the unicast queue, as on the rig's NIC), netem
#           "limit 100000 delay 100us rate 1gbit", Bernoulli loss (gemodel with mean burst 1) at 0 / 0.1 / 1% - the
#           rig's Array1m cells (rig_queue: rmw_keepall_rig.sh, Array1m 30 Hz) - 1 MiB RELIABLE KEEP_LAST 10 at 30 Hz.
#
# PRE-REGISTERED (written before the first run; the python at the end applies it - it, not this comment, decides):
#   VOID (a rep says nothing): no RESULT line from ping or echo; a tc drop on any band in a ctl arm; the PC's 1-minute
#     load above nproc/2 at the rep's start (counted and listed, left out of every reading).
#   CONTROL that no route change can touch: the idle node sends only discovery and has no peer to address, so its
#     broadcast_with_data, broadcast_with_reliable_control and addressed_with_discovery must all be 0 - else the instrument
#     counts something else and the routes phase is VOID.
#   (b) REAL on a tree when ping's broadcast_with_data, summed over its usable reps of a cell, is > 0: ping's writer has
#     exactly one matched reader (echo), so a broadcast sample is one that went to every node past its one peer, on the
#     broadcast path. NOT SHOWN when it is 0 in every cell: the default publish already sends batched content first
#     (flush_pending_before_unicast()), and (b) is then no cost on the default path - nothing is changed for it.
#   (c) REAL on a tree when addressed_with_discovery on ping or echo, summed over a cell's usable reps, is > 0: a summary
#     or announce reached the peers and not the idle node. Each such datagram is a summary the idle node did not get.
#     The source is named by the cell: a count in the KEEP_ALL cell and none in the KEEP_LAST rel_large cell points at
#     the solicitation; counts in rel_small / rel_large point at another addressed send (retransmission, initial or
#     eviction HEARTBEAT, ACKNACK) - each a defect of the same class, listed, fixed only where a test shows it.
#   A FIX for (b) or (c) is SHOWN when the parent is REAL on a cell, the fix's tree counts 0 there, every other counter of
#     the cell is within the parent's range (rep min..max, widened by 10%) and delivery is 100% on both.
#   (a) REAL on ETHERNET when, in ge0.1b1 or ge1b1 pooled over usable reps, the writer's resends per lost unit
#     (retransmitted / the reader's missing_opened) exceed 1.25 / (1 - L) - more than a quarter above an ideal ARQ that
#     resends each lost unit until one copy arrives - OR the reader's duplicate units (large_duplicate) exceed 20% of the
#     resends. FALSIFIED (nothing is changed for (a) on ETHERNET) when both stay under in both lossy arms. The ctl arm
#     must resend 0 (a resend with nothing lost is a false gap, a different defect: the phase is VOID and named).
#
# Usage: TREES="P=/tree [F=/tree ...]" [PHASE=both] [REPS=3] [DUR=30] [OUT=dir] large_repair_followups_netns.sh
#   Each tree holds this harness, wifi_burst_loss_netns.sh, wifi_link_node.c, large_sample_bench.c, src/ include/ and
#   examples/perf_hil/tickle/common/ (a git archive or a worktree). OUT defaults to
#   ~/rig_results_safe/large_repair/<stamp>; the summary in OUT/summary.txt ends with "=== large_repair_followups ended".
#   READ_ONLY=1 re-reads an existing OUT.
set -uo pipefail
TREES=${TREES:?TREES=\"label=dir ...\"}
PHASE=${PHASE:-both}
REPS=${REPS:-3}
DUR=${DUR:-30}
OUT=${OUT:-$HOME/rig_results_safe/large_repair/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT" || exit 1
SUM="$OUT/summary.txt"
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== large_repair_followups ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ===" >>"$SUM"
}
trap on_exit EXIT

if [ "${READ_ONLY:-0}" != 1 ]; then
    echo "=== large_repair_followups $(date -Is) TREES='$TREES' PHASE=$PHASE REPS=$REPS DUR=$DUR" >"$SUM"
    # job LABEL DIR SUBDIR VARS...: one rep of one WB invocation of tree DIR under OUT/LABEL/SUBDIR.
    job() {
        local label=$1 dir=$2 sub=$3 rep=$4
        shift 4
        env OUT="$OUT/$label/$sub" REP_FIRST="$rep" REPS=1 DUR="$DUR" PROFILE=ethernet NODE_ENV=TT_UDP_OFFLOAD=0 \
            LARGE_RSTATS=1 "$@" "$dir/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" \
            >>"$OUT/$label/$sub.log" 2>&1
    }
    for rep in $(seq 1 "$REPS"); do
        for t in $TREES; do
            label=${t%%=*}
            dir=${t#*=}
            mkdir -p "$OUT/$label"
            echo "rep $rep tree $label $(date -Is) load=$(cut -d' ' -f1 /proc/loadavg)" >>"$SUM"
            if [ "$PHASE" != amp ]; then
                job "$label" "$dir" routes "$rep" ARMS="ctl ge5b4" CELLS="rel_small rel_large"
                job "$label" "$dir" keepall "$rep" ARMS="ctl ge5b4" CELLS="rel_large" LARGE_PUB_ARGS="-A"
            fi
            if [ "$PHASE" != routes ]; then
                job "$label" "$dir" amp "$rep" ARMS="ctl ge0.1b1 ge1b1" CELLS="rel_large" LARGE_HZ=30 BANDS=1 \
                    NETEM_BASE="limit 100000 delay 100us rate 1gbit"
            fi
        done
    done
fi

python3 - "$OUT" "$PHASE" <<'EOF' | tee -a "$SUM"
import os, pathlib, re, sys
out, phase = pathlib.Path(sys.argv[1]), sys.argv[2]
NPROC = os.cpu_count() or 1
KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")

def fields(text, prefix):
    d = {}
    for line in text.splitlines():
        if line.startswith(prefix):
            d.update(KV.findall(line))
    return d

def num(d, k):
    try:
        return float(d.get(k, "nan"))
    except ValueError:
        return float("nan")

def read_run(run):
    r = {"name": run.name}
    meta = (run / "meta.txt").read_text() if (run / "meta.txt").exists() else ""
    m = re.search(r"load=([\d.]+)", meta)
    r["load"] = float(m.group(1)) if m else float("nan")
    for node, f in (("p", "ping.log"), ("e", "echo.log"), ("i", "idle.log")):
        t = (run / f).read_text(errors="replace") if (run / f).exists() else ""
        r[node + "_result"] = fields(t, "RESULT")
        r[node + "_rstats"] = fields(t, "RSTATS")
    c = (run / "counters.txt").read_text() if (run / "counters.txt").exists() else ""
    r["drops"] = sum(int(x) for x in re.findall(r"dropped (\d+)", c))
    return r

def usable(r, arm):
    if not r["p_result"] or not r["e_result"]:
        return "no RESULT line"
    if arm == "ctl" and r["drops"]:
        return f"{r['drops']} tc drops in the control"
    if r["load"] > NPROC / 2:
        return f"load {r['load']} > nproc/2"
    return ""

def runs(base):
    return sorted((base / "runs").glob("*")) if (base / "runs").exists() else []

trees = sorted(p.name for p in out.iterdir() if p.is_dir())
ROUTE_KEYS = ("broadcast_with_data", "broadcast_with_reliable_control", "addressed_with_discovery")
verdicts = []
if phase != "amp":
    print("--- routes: per tree, sub-run, arm-cell; per node p/e/i: broadcast_with_data / broadcast_with_reliable_control / "
          "addressed_with_discovery, summed over usable reps (min..max per rep); delivery")
    ranges = {}
    for tree in trees:
        for sub in ("routes", "keepall"):
            groups = {}
            for run in runs(out / tree / sub):
                arm_cell = run.name.rsplit("-", 1)[0]
                groups.setdefault(arm_cell, []).append(read_run(run))
            for arm_cell, rs in sorted(groups.items()):
                arm = arm_cell.split("-")[0]
                ok = []
                for r in rs:
                    why = usable(r, arm)
                    if why:
                        print(f"VOID  {tree} {sub} {r['name']}: {why}")
                    else:
                        ok.append(r)
                if not ok:
                    continue
                cells = []
                for node in "pei":
                    vals = [[int(num(r[node + "_rstats"], k)) for r in ok] for k in ROUTE_KEYS]
                    cells.append(f"{node} " + "/".join(f"{sum(v)}({min(v)}..{max(v)})" for v in vals))
                    for k, v in zip(ROUTE_KEYS, vals):
                        ranges[(tree, sub, arm_cell, node, k)] = (sum(v), min(v), max(v))
                pub = [int(num(r["p_result"], "published")) for r in ok]
                if "rel_large" in arm_cell:
                    dlv = [int(num(r["e_result"], "delivered")) for r in ok]
                else:
                    dlv = [int(num(r["e_result"], "scored") + num(r["e_result"], "below_warmup")) for r in ok]
                print(f"{tree:4} {sub:8} {arm_cell:16} reps {len(ok)}  " + "  ".join(cells) +
                      f"  published {sum(pub)} delivered {sum(dlv)}")
                idle = [ranges[(tree, sub, arm_cell, "i", k)][0] for k in ROUTE_KEYS]
                if any(idle):
                    verdicts.append(f"VOID  routes {tree} {sub} {arm_cell}: the idle node counted {idle} (control must be 0)")
                b = ranges[(tree, sub, arm_cell, "p", "broadcast_with_data")][0]
                c = sum(ranges[(tree, sub, arm_cell, n, "addressed_with_discovery")][0] for n in "pe")
                verdicts.append(f"{'REAL' if b else 'NONE'}  (b) {tree} {sub} {arm_cell}: ping broadcast_with_data {b}")
                verdicts.append(f"{'REAL' if c else 'NONE'}  (c) {tree} {sub} {arm_cell}: addressed_with_discovery p+e {c}")
if phase != "routes":
    print("--- amp: per tree and arm, pooled over usable reps: resent, reader missing units, resends per lost unit "
          "(limit 1.25/(1-L)), reader duplicate units / resent (limit 20%), timer/immediate ACKNACKs, p50/p99 ms")
    for tree in trees:
        groups = {}
        for run in runs(out / tree / "amp"):
            groups.setdefault(run.name.split("-")[0], []).append(read_run(run))
        for arm in ("ctl", "ge0.1b1", "ge1b1"):
            ok = []
            for r in groups.get(arm, []):
                why = usable(r, arm)
                if why:
                    print(f"VOID  {tree} amp {r['name']}: {why}")
                else:
                    ok.append(r)
            if not ok:
                continue
            resent = sum(num(r["p_result"], "retransmitted") for r in ok)
            missing = sum(num(r["e_rstats"], "missing_opened") for r in ok)
            dups = sum(num(r["e_result"], "large_duplicate") for r in ok)
            timer = sum(num(r["e_rstats"], "timer") for r in ok)
            imm = sum(num(r["e_rstats"], "immediate") for r in ok)
            p50 = sorted(num(r["e_result"], "latency_median_us") / 1000 for r in ok)
            p99 = sorted(num(r["e_result"], "latency_p99_us") / 1000 for r in ok)
            dlv = sum(num(r["e_result"], "delivered") for r in ok)
            pub = sum(num(r["p_result"], "published") for r in ok)
            loss = {"ctl": 0.0, "ge0.1b1": 0.001, "ge1b1": 0.01}[arm]
            per = resent / missing if missing else float("nan")
            share = dups / resent if resent else 0.0
            print(f"{tree:4} {arm:8} reps {len(ok)} resent {resent:.0f} missing {missing:.0f} per_lost {per:.3f} "
                  f"(limit {1.25 / (1 - loss):.3f}) dups {dups:.0f} ({100 * share:.1f}% of resent) acknack timer {timer:.0f} "
                  f"immediate {imm:.0f} p50 {p50} p99 {p99} delivered {dlv:.0f}/{pub:.0f}")
            if arm == "ctl":
                if resent:
                    verdicts.append(f"VOID  (a) {tree}: the control resent {resent:.0f} with nothing lost (a false gap)")
            elif missing:
                real = per > 1.25 / (1 - loss) or share > 0.20
                verdicts.append(f"{'REAL' if real else 'NONE'}  (a) {tree} {arm}: per_lost {per:.3f} dups {100 * share:.1f}%")
print("--- verdicts")
for v in verdicts:
    print(v)
EOF

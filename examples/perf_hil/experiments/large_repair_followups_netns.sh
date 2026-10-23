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
# PHASES (PHASE=routes|amp|both|slow|all|order, default both; all adds slow; order is amp and slow, read as an A/B):
#   routes  (b) and (c). The WB shape (two prio bands, broadcast in its own netem, slot 1-4 ms x 42), arms ctl and
#           ge5b4, cells rel_small (100 B RELIABLE KEEP_LAST 10 at 100 Hz, echoed) and rel_large (1 MiB RELIABLE at
#           5 Hz), and rel_large again with a KEEP_ALL writer (-A, OUT/keepall), which solicits acknowledgements
#           (tt_Publisher_request_ack()) and is the one cell here whose writer calls it. Counted per node by RSTATS in
#           flush_tx(): broadcast_with_data (a datagram broadcast with a sample DATA in it), broadcast_with_reliable_control
#           (one broadcast with a non-discovery HEARTBEAT or ACKNACK), addressed_with_discovery (one sent to peers while
#           it held a batched announce or summary - which then reached those peers alone).
#   (slow: see AMENDED below)
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
# AMENDED 2026-10-23 19:25, AFTER THE FIRST PARENT RUN (parent_947e) AND BEFORE ANY RUN OF THE PHASE IT ADDS. That run
#   was loaded by other sessions (load 9-28 at most reps' start): every amp rep and every keepall rep VOID by the rule
#   above, and none is read for (a). What they showed, as a lead and not a result: the reps whose READER dropped in
#   its socket (socket_drops 222-6,658, a CPU-starved reader) resent 8,179-532,481 units with duplicates 47-97% of the
#   resends - the ctl rep resent 532,481 with nothing lost on the path - while the reps without socket drops resent
#   1.03-1.05 per lost unit with 2-4% duplicates. So the amplifier may need loss AT THE READER, which the amp phase's
#   path loss does not make. Hence:
#   slow    (a) under reader-side loss, the treatment made on purpose: the amp phase's path, ctl only (no path loss),
#           the reader spinning W us in each sample's callback (large_sample_bench sub -w W), W = 0 (the control) and
#           W = 25000 (75% of the 33.3 ms sample period, so the next sample's 8.4 ms burst arrives while it spins).
#   VOID for slow: the W = 0 control with any socket drop or any resend; a W = 25000 rep with no socket drop (the
#     treatment did not apply); tc drops on any rep (nothing is configured to drop); the load rule above.
#   (a) REAL under reader-side loss when, pooled over usable W = 25000 reps, resends per missing unit exceed 1.25 or
#     duplicates exceed 20% of resends (an ideal ARQ resends each unit lost in the socket once: the path loses
#     nothing). FALSIFIED when both stay under. Either way the reading is about ETHERNET's default repair.
#
# AMENDED 2026-10-23 23:00, BEFORE ITS FIRST RUN: the resend order's A/B (DESIGN.md section 4, "Large-sample resend
#   order"; branch ab/large-resend-order). PHASE=order runs amp and slow on two trees, labelled P (the parent) and F
#   (the change), rep by rep in alternation; the python's "order" section applies this reading:
#   VOID as above, per rep. STORM: a usable lossy amp rep whose writer resent more than 2 units per unit its reader
#     found missing (retransmitted / missing_opened > 2; the calm parent reps of LRF read 1.01-1.02, the storm 3.83
#     pooled with 251,028 resent in its one rep).
#   CONTROL the change cannot touch: the ctl arm (nothing lost: no unit is ever named, the wire is the parent's -
#     tests/wire_identity_resend_order.sh). Both trees must resend 0 there; |p99(F) - p99(P)| over its usable reps (the
#     medians of the rep p99s) is the noise band N.
#   ge1b1 SHOWN when F has no STORM rep, P has at least one, and F delivers no fewer samples than P. UNDECIDED when P has
#     no STORM rep either (the parent did not storm this run: nothing to stop), and then read only for cost.
#     FALSIFIED when F has a STORM rep.
#   COST, ge0.1b1 and ge1b1, read even when UNDECIDED: F's median rep p99 above P's by more than N, or F delivering
#     fewer samples than P, is a COST (named, with numbers); none is NO COST.
#   slow (reader-side loss, W = 25000): F's pooled resends / socket drops, against P's - a measurement, the hold's
#     effect where the reader's own socket drops repairs (the sim: 564,402 -> 36,999); not a verdict.
#
# AMENDED 2026-10-24 05:30, BEFORE ITS FIRST RUN: the resend order's tail (DESIGN.md section 4, "The tail: RACK's time
#   rule"; branch ab/large-resend-order-tail). PHASE=tail runs amp only, on trees labelled O (optional: the resend
#   order's parent, 896d4a29), P (the resend order, the base) and F (the change), rep by rep in alternation; the
#   python's "tail" section applies this reading:
#   VOID and STORM as for order, per rep. CONTROL the change cannot touch: the ctl arm (nothing lost, no unit named):
#     every tree resends 0 there, else VOID; N = |median rep p99 F - P| on ctl.
#   FALSIFIED when F has a STORM rep at ge0.1b1 or ge1b1, or delivers fewer samples than P in either.
#   ge1b1 SHOWN when F's median rep p99 is below P's by more than N; COST when above by more than N; NOT SHOWN
#     otherwise. Printed beside it, not a verdict: the share of P's cost over O that F takes back, (P - F) / (P - O).
#   ge0.1b1 COST when F's median rep p99 is above P's by more than N; else NO COST.
#   The price in copies, a measurement: resent per missing unit and the reader's duplicate units per tree and arm.
#   RSTATS lro (F only: the base does not print it): held units released by the order, RACK's time rule, the timeout.
#   LOAD: the PC is shared; before each rep the harness waits (at most 120 s) for the 1-minute load to fall to 4, and a
#     rep that starts above 4 is VOID for this phase (stricter than nproc/2 above).
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
            if [ "$PHASE" = tail ]; then
                waited=0
                while awk '{ exit !($1 > 4) }' /proc/loadavg && [ "$waited" -lt 120 ]; do
                    sleep 10
                    waited=$((waited + 10))
                done
            fi
            echo "rep $rep tree $label $(date -Is) load=$(cut -d' ' -f1 /proc/loadavg)" >>"$SUM"
            if [ "$PHASE" != amp ] && [ "$PHASE" != slow ] && [ "$PHASE" != order ] && [ "$PHASE" != tail ]; then
                job "$label" "$dir" routes "$rep" ARMS="ctl ge5b4" CELLS="rel_small rel_large"
                job "$label" "$dir" keepall "$rep" ARMS="ctl ge5b4" CELLS="rel_large" LARGE_PUB_ARGS="-A"
            fi
            if [ "$PHASE" = slow ] || [ "$PHASE" = all ] || [ "$PHASE" = order ]; then
                for w in 0 25000; do
                    job "$label" "$dir" "slow_w$w" "$rep" ARMS="ctl" CELLS="rel_large" LARGE_HZ=30 BANDS=1 \
                        NETEM_BASE="limit 100000 delay 100us rate 1gbit" LARGE_SUB_ARGS="-w $w"
                done
            fi
            if [ "$PHASE" = slow ]; then
                continue
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
if phase in ("slow", "all", "order"):
    print("--- slow: per tree and W, pooled over usable reps: resent, reader missing units, socket drops, resends per "
          "missing unit (limit 1.25), duplicates / resent (limit 20%), delivered")
    for tree in trees:
        for w in ("0", "25000"):
            ok = []
            for run in runs(out / tree / f"slow_w{w}"):
                r = read_run(run)
                why = usable(r, "ctl")
                sd = num(r["e_result"], "socket_drops") if r["e_result"] else 0
                rs = num(r["p_result"], "retransmitted") if r["p_result"] else 0
                if not why and w == "0" and (sd or rs):
                    why = f"the control dropped {sd:.0f} in the socket / resent {rs:.0f}"
                if not why and w != "0" and not sd:
                    why = "no socket drop: the treatment did not apply"
                if why:
                    print(f"VOID  {tree} slow_w{w} {r['name']}: {why}")
                else:
                    ok.append(r)
            if not ok:
                continue
            resent = sum(num(r["p_result"], "retransmitted") for r in ok)
            missing = sum(num(r["e_rstats"], "missing_opened") for r in ok)
            dups = sum(num(r["e_result"], "large_duplicate") for r in ok)
            sd = sum(num(r["e_result"], "socket_drops") for r in ok)
            dlv = sum(num(r["e_result"], "delivered") for r in ok)
            pub = sum(num(r["p_result"], "published") for r in ok)
            per = resent / missing if missing else float("nan")
            share = dups / resent if resent else 0.0
            print(f"{tree:4} W={w:5} reps {len(ok)} resent {resent:.0f} missing {missing:.0f} socket_drops {sd:.0f} "
                  f"per_missing {per:.3f} dups {dups:.0f} ({100 * share:.1f}%) delivered {dlv:.0f}/{pub:.0f}")
            if w != "0":
                real = per > 1.25 or share > 0.20
                verdicts.append(f"{'REAL' if real else 'NONE'}  (a, reader-side) {tree}: per_missing {per:.3f} "
                                f"dups {100 * share:.1f}%")
if phase not in ("amp", "slow", "order"):
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
if phase not in ("routes", "slow"):
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
if phase == "order" and {"P", "F"} <= set(trees):
    import statistics
    print("--- order: per tree and arm, usable amp reps: STORM reps (resent / missing > 2), resent, delivered, "
          "median rep p99 ms")
    view = {}
    for tree in ("P", "F"):
        groups = {}
        for run in runs(out / tree / "amp"):
            groups.setdefault(run.name.split("-")[0], []).append(read_run(run))
        for arm in ("ctl", "ge0.1b1", "ge1b1"):
            ok = [r for r in groups.get(arm, []) if not usable(r, arm)]
            storms, rows = 0, []
            for r in ok:
                rs = num(r["p_result"], "retransmitted")
                ms = num(r["e_rstats"], "missing_opened")
                storm = arm != "ctl" and ms > 0 and rs / ms > 2
                storms += storm
                rows.append(f"{rs:.0f}/{ms:.0f}{'*' if storm else ''}")
            p99 = [num(r["e_result"], "latency_p99_us") / 1000 for r in ok]
            dlv = sum(num(r["e_result"], "delivered") for r in ok)
            pub = sum(num(r["p_result"], "published") for r in ok)
            resent = sum(num(r["p_result"], "retransmitted") for r in ok)
            med = statistics.median(p99) if p99 else float("nan")
            view[(tree, arm)] = {"n": len(ok), "storms": storms, "resent": resent, "dlv": dlv, "pub": pub, "p99": med}
            print(f"{tree} {arm:8} reps {len(ok)} storms {storms} resent/missing per rep {' '.join(rows)} "
                  f"delivered {dlv:.0f}/{pub:.0f} median p99 {med:.1f}")
    ctl_p, ctl_f = view.get(("P", "ctl")), view.get(("F", "ctl"))
    if not ctl_p or not ctl_f or not ctl_p["n"] or not ctl_f["n"]:
        verdicts.append("VOID  order: no usable control rep on one tree")
    elif ctl_p["resent"] or ctl_f["resent"]:
        verdicts.append(f"VOID  order: the control resent (P {ctl_p['resent']:.0f}, F {ctl_f['resent']:.0f})")
    else:
        noise = abs(ctl_f["p99"] - ctl_p["p99"])
        print(f"control noise band N = |p99 F - p99 P| on ctl = {noise:.2f} ms")
        p1, f1 = view.get(("P", "ge1b1")), view.get(("F", "ge1b1"))
        if p1 and f1 and p1["n"] and f1["n"]:
            if f1["storms"]:
                verdicts.append(f"FALSIFIED  order ge1b1: F stormed in {f1['storms']} of {f1['n']} reps")
            elif not p1["storms"]:
                verdicts.append(f"UNDECIDED  order ge1b1: the parent did not storm in its {p1['n']} reps")
            elif f1["dlv"] < p1["dlv"]:
                verdicts.append(f"FALSIFIED  order ge1b1: F delivered {f1['dlv']:.0f} < P {p1['dlv']:.0f}")
            else:
                verdicts.append(f"SHOWN  order ge1b1: P stormed {p1['storms']}/{p1['n']}, F 0/{f1['n']}; resent "
                                f"{p1['resent']:.0f} -> {f1['resent']:.0f}")
        for arm in ("ge0.1b1", "ge1b1"):
            pa, fa = view.get(("P", arm)), view.get(("F", arm))
            if not pa or not fa or not pa["n"] or not fa["n"]:
                continue
            worse = fa["p99"] - pa["p99"] > noise
            fewer = fa["dlv"] < pa["dlv"]
            verdicts.append(f"{'COST' if worse or fewer else 'NO COST'}  order {arm}: median p99 P {pa['p99']:.1f} F "
                            f"{fa['p99']:.1f} ms (N {noise:.2f}); delivered P {pa['dlv']:.0f} F {fa['dlv']:.0f}")
if phase == "tail" and {"P", "F"} <= set(trees):
    import statistics
    print("--- tail: per tree and arm, usable amp reps: STORM reps, resent / missing, reader duplicates, delivered, "
          "median rep p99 ms (rep p99s)")
    view = {}
    for tree in [t for t in ("O", "P", "F") if t in trees]:
        groups = {}
        for run in runs(out / tree / "amp"):
            groups.setdefault(run.name.split("-")[0], []).append(read_run(run))
        for arm in ("ctl", "ge0.1b1", "ge1b1"):
            all_reps = groups.get(arm, [])

            def why_void(r):
                return usable(r, arm) or (f"load {r['load']} > 4" if r["load"] > 4 else "")

            ok = [r for r in all_reps if not why_void(r)]
            for r in all_reps:
                if why_void(r):
                    print(f"  void {tree} {arm} {r['name']}: {why_void(r)}")
            storms = 0
            for r in ok:
                rs = num(r["p_result"], "retransmitted")
                ms = num(r["e_rstats"], "missing_opened")
                storms += arm != "ctl" and ms > 0 and rs / ms > 2
            p99 = [num(r["e_result"], "latency_p99_us") / 1000 for r in ok]
            dlv = sum(num(r["e_result"], "delivered") for r in ok)
            pub = sum(num(r["p_result"], "published") for r in ok)
            resent = sum(num(r["p_result"], "retransmitted") for r in ok)
            missing = sum(num(r["e_rstats"], "missing_opened") for r in ok)
            dups = sum(num(r["e_result"], "large_duplicate") for r in ok)
            lro = {k: sum(num(r["p_rstats"], k) for r in ok)
                   for k in ("released_shown", "rack", "released_timeout", "timeout_tail")}
            med = statistics.median(p99) if p99 else float("nan")
            view[(tree, arm)] = {"n": len(ok), "storms": storms, "resent": resent, "dlv": dlv, "pub": pub, "p99": med}
            per = resent / missing if missing else float("nan")
            print(f"{tree} {arm:8} reps {len(ok)} storms {storms} resent {resent:.0f} / missing {missing:.0f} = "
                  f"{per:.3f} dups {dups:.0f} delivered {dlv:.0f}/{pub:.0f} median p99 {med:.1f} "
                  f"({' '.join(f'{x:.1f}' for x in sorted(p99))}) lro shown/rack/timeout(tail) "
                  f"{lro['released_shown']:.0f}/{lro['rack']:.0f}/{lro['released_timeout']:.0f}({lro['timeout_tail']:.0f})")
    ctl = [view.get((t, "ctl")) for t in ("O", "P", "F") if (t, "ctl") in view]
    ctl_p, ctl_f = view.get(("P", "ctl")), view.get(("F", "ctl"))
    if not ctl_p or not ctl_f or not ctl_p["n"] or not ctl_f["n"]:
        verdicts.append("VOID  tail: no usable control rep on P or F")
    elif any(c["resent"] for c in ctl if c["n"]):
        verdicts.append("VOID  tail: the control resent on a tree: " +
                        " ".join(f"{t} {view[(t, 'ctl')]['resent']:.0f}" for t in ("O", "P", "F") if (t, "ctl") in view))
    else:
        noise = abs(ctl_f["p99"] - ctl_p["p99"])
        print(f"control noise band N = |p99 F - p99 P| on ctl = {noise:.2f} ms")
        bad = []
        for arm in ("ge0.1b1", "ge1b1"):
            pa, fa = view.get(("P", arm)), view.get(("F", arm))
            if not pa or not fa or not pa["n"] or not fa["n"]:
                verdicts.append(f"VOID  tail {arm}: no usable rep on P or F")
                continue
            if fa["storms"]:
                bad.append(f"F stormed in {fa['storms']} of {fa['n']} {arm} reps")
            if fa["dlv"] < pa["dlv"]:
                bad.append(f"F delivered {fa['dlv']:.0f} < P {pa['dlv']:.0f} at {arm}")
            d = fa["p99"] - pa["p99"]
            if arm == "ge1b1":
                word = "SHOWN" if d < -noise else ("COST" if d > noise else "NOT SHOWN")
                oa = view.get(("O", arm))
                share = ""
                if oa and oa["n"] and pa["p99"] != oa["p99"]:
                    share = f"; O {oa['p99']:.1f} ms, share of P's cost taken back {(pa['p99'] - fa['p99']) / (pa['p99'] - oa['p99']):.2f}"
                verdicts.append(f"{word}  tail ge1b1: median p99 P {pa['p99']:.1f} F {fa['p99']:.1f} ms (N {noise:.2f}){share}")
            else:
                verdicts.append(f"{'COST' if d > noise else 'NO COST'}  tail {arm}: median p99 P {pa['p99']:.1f} F "
                                f"{fa['p99']:.1f} ms (N {noise:.2f})")
        if bad:
            verdicts.append("FALSIFIED  tail: " + "; ".join(bad))
print("--- verdicts")
for v in verdicts:
    print(v)
EOF

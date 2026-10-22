#!/usr/bin/env bash
# wifi_repair_ab.sh - network profiles phase 3, Wi-Fi repair values (2026-10-23): the WB harness
# (wifi_burst_loss_netns.sh, rules as amended there) for several trees AT ONCE, one instance per tree, same profile,
# arms and cells, so the trees share the PC's load rep by rep. For a core change against its parent: the arms differ
# only in the tree each was built from.
#
# PRE-REGISTERED (written before the first run of it; the python at the end applies it):
#   per tree and arm: offered, delivered %, one-way p99 (unloaded reps), the median over reps of each rep's own p99
#     (rel_small; the pooled p99 rests on ~1% of samples and moves 40 -> 60 ms between runs of one build), resends per
#     lost datagram, copies after delivery, and each rep's writer resends (rel_large: a rep resending more than 10 x
#     the median rep is a STORM, counted per tree).
#   the harness's own R1-R7 verdicts per tree, side by side.
#   A tree (not the first) is NOT WORSE on an arm when, at ge5b2/ge5b4 rel_small, its median per-rep p99 is within 10% of the first
#     tree's or below it, and, on every arm, its delivered % is no more than 0.1 points below; else WORSE. STORM reps are listed; a tree
#     with more STORM reps than the first is WORSE on that cell whatever its medians.
#
# Usage: TREES="P=/path/tree F=/path/tree ..." TAG=name [PROFILE=wifi] [REPS=4] [ARMS=...] [CELLS=...] wifi_repair_ab.sh
#   Each tree is a checkout (a detached git worktree) holding this harness. Output under
#   ~/rig_results_safe/wifi_repair/<TAG>_<label>/ and <TAG>.ab.txt, which ends with "=== wifi_repair_ab ended".
set -uo pipefail
TREES=${TREES:?TREES=\"label=dir ...\"}
TAG=${TAG:?TAG names the output}
BASE_OUT=${BASE_OUT:-$HOME/rig_results_safe/wifi_repair}
mkdir -p "$BASE_OUT" || exit 1
AB="$BASE_OUT/$TAG.ab.txt"
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== wifi_repair_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ===" >>"$AB"
}
trap on_exit EXIT
echo "=== wifi_repair_ab $(date -Is) tag=$TAG TREES='$TREES' PROFILE=${PROFILE:-wifi} REPS=${REPS:-4}" \
    "ARMS='${ARMS:-}' CELLS='${CELLS:-}' ===" >"$AB"
pids=()
labels=()
for t in $TREES; do
    label=${t%%=*}
    dir=${t#*=}
    labels+=("$label")
    echo "tree $label: $dir at $(git -C "$dir" rev-parse --short HEAD) dirty $(git -C "$dir" status --porcelain -- src | wc -l)" >>"$AB"
    PROFILE=${PROFILE:-wifi} REPS=${REPS:-4} OUT="$BASE_OUT/${TAG}_$label" NODE_ENV=${NODE_ENV:-TT_UDP_OFFLOAD=0} \
        LARGE_RSTATS=${LARGE_RSTATS:-1} "$dir/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" \
        >"$BASE_OUT/${TAG}_$label.log" 2>&1 &
    pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid"; done
python3 - "$BASE_OUT" "$TAG" "${labels[@]}" <<'EOF' | tee -a "$AB"
import os, re, statistics, sys
base, tag, labels = sys.argv[1], sys.argv[2], sys.argv[3:]
KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
def kv(line): return dict(KV.findall(line))
def read(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []
def q(v, f):
    return v[min(len(v) - 1, int(len(v) * f))] if v else float("nan")
rows = {}
for lab in labels:
    out = os.path.join(base, f"{tag}_{lab}")
    nproc = os.cpu_count() or 1
    for name in sorted(os.listdir(os.path.join(out, "runs"))) if os.path.isdir(os.path.join(out, "runs")) else []:
        arm, cell, rep = name.rsplit("-", 2)
        d = os.path.join(out, "runs", name)
        meta = kv(" ".join(read(os.path.join(d, "meta.txt"))))
        loaded = float(meta.get("load", 0)) > nproc / 2
        r = rows.setdefault((lab, cell, arm), {"p99": [], "retx": [], "loaded": 0, "reps": 0})
        r["reps"] += 1
        r["loaded"] += loaded
        ping = read(os.path.join(d, "ping.log"))
        res = [kv(x) for x in ping if x.startswith("RESULT")]
        r["retx"].append(int(res[0].get("retransmitted", 0)) if res else -1)
        if cell != "rel_large" and not loaded:
            owd = sorted(int(x) for x in read(os.path.join(d, "owd_us.txt")) if x.strip().isdigit())
            if owd:
                r["p99"].append(q(owd, 0.99) / 1000)
print("per-rep reading (pre-registered in wifi_repair_ab.sh):")
first = labels[0]
for (lab, cell, arm), r in sorted(rows.items(), key=lambda k: (k[0][1], k[0][2], labels.index(k[0][0]))):
    med = statistics.median(r["p99"]) if r["p99"] else float("nan")
    rmed = statistics.median([x for x in r["retx"] if x >= 0]) if r["retx"] else 0
    storms = sum(1 for x in r["retx"] if cell == "rel_large" and rmed > 0 and x > 10 * rmed)
    r["med"], r["storms"] = med, storms
    print(f"  {cell:9} {arm:7} {lab:4} reps {r['reps']} (loaded {r['loaded']}) per-rep p99 ms "
          f"{' '.join(f'{x:.1f}' for x in r['p99']) or '-'} median {med:.1f}; writer resends {r['retx']} storms {storms}")
deliv = {}
for lab in labels:
    s = os.path.join(base, f"{tag}_{lab}", "summary.txt")
    print(f"--- {lab}: the harness's table and rules ({s})")
    for line in read(s):
        if re.match(r"^(rel_small|rel_large|be_small) ", line) or re.match(r"^(PASS|FAIL|VOID) ", line):
            print("  " + line[:200])
        f = line.split()
        if len(f) > 3 and f[0] in ("rel_small", "rel_large", "be_small"):
            try:
                deliv[(lab, f[0], f[1])] = float(f[3])
            except ValueError:
                pass
for (lab, cell, arm), r in sorted(rows.items()):
    if lab == first:
        continue
    f = rows.get((first, cell, arm))
    if f is None:
        continue
    worse = []
    if cell == "rel_small" and arm in ("ge5b2", "ge5b4") and r["med"] == r["med"] and f["med"] == f["med"]:
        if r["med"] > 1.10 * f["med"]:
            worse.append(f"median per-rep p99 {r['med']:.1f} > 1.10 x {f['med']:.1f}")
    dl, df = deliv.get((lab, cell, arm)), deliv.get((first, cell, arm))
    if dl is not None and df is not None and dl < df - 0.1:
        worse.append(f"delivered {dl:.2f}% < {df:.2f}% - 0.1")
    if r["storms"] > f["storms"]:
        worse.append(f"{r['storms']} storm reps against {f['storms']}")
    print(f"VERDICT {lab} vs {first} {cell}/{arm}: {'WORSE (' + '; '.join(worse) + ')' if worse else 'NOT WORSE'}")
EOF

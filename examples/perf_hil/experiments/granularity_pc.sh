#!/usr/bin/env bash
# The measured retry granularity (G = the context's own timer lateness, 2026-10-08, ROADMAP Now 5a) on THIS PC, in
# private network namespaces: what G a context measures here, idle and under load, and whether the lossy reliable cells
# regress against the fixed 100 us it replaces. A PC figure for the decision to A/B it on the rig, never a published one.
#
# PART 1, G. timer_lateness_probe.c (one context, a retry timer due every 1 ms, polled to it) at arm B, REPS runs idle
# and REPS with one busy `yes` per CPU. Each line prints the G the context ended with beside the lateness its retry
# timer saw.
#   READING (the probe's own header): G reads as intended when late_p90 <= G <= 4 x late_p99 + resolution.
#
# PART 2, the lossy cells, as campaign_sweep.sh runs them but on a veth pair with netem loss 5% on the client's egress:
#   c5: reliable_throughput p1 -Q -N 2048 -B 100, c6: reliable_throughput p4 -Q -N 187 -B 100; -d 5, 2 s warm-up and
#   cool-down. Arms, interleaved per rep (the order rotates by one each rep):
#     A    = the parent (fixed 100 us G)
#     B    = the change (measured G)
#     Bfix = the change built with -Dtt_RELIABLE_RETRY_GRANULARITY=100000: the same code path with G fixed at the old
#            value - the CONTROL, which differs from A only in code the measured G does not run. Its delta to A is
#            this harness's noise at this design.
#   PRIMARY metrics, per cell: client send_mbps (higher is better) and client wire_bytes_per_sample (lower is better).
#   VOID: a rep whose client printed no RESULT, did not drain (drained=acked), or whose server lost a sample (lost=0
#     is required at KEEP_ALL); a cell with fewer than 3 valid reps per arm.
#   READING, per cell and metric, on the medians: d(B) = B - A, d(ctl) = Bfix - A, and Welch t of B against A.
#     REGRESSED  if d(B) is in the worse direction, |t| > 2, and |d(B)| > |d(ctl)|.
#     IMPROVED   if d(B) is in the better direction, |t| > 2, and |d(B)| > |d(ctl)|.
#     HELD       otherwise.
#   TREATMENT: Bfix's client binary must differ from B's, or the control did not build differently and is VOID. The B
#     server's timer_lateness_ns= (the G its retry timer used at the end) is printed for every rep.
#
# Usage: A=<rev> B=<rev> [REPS=5] granularity_pc.sh      (both revisions committed). Launch detached:
#   setsid nohup examples/perf_hil/experiments/granularity_pc.sh > ~/rig_results_safe/granularity_pc.launch.log 2>&1 &
# Results in ~/rig_results_safe/granularity_pc_<A>_<B>_<time>.txt. Never touches the rig.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=box
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
A=${A:?A=<rev> the parent}
B=${B:?B=<rev> the change}
REPS=${REPS:-5}
a_sha=$(git -C "$REPO" rev-parse --short "$A") || exit 1
b_sha=$(git -C "$REPO" rev-parse --short "$B") || exit 1
OUT=${OUT:-$HOME/rig_results_safe/granularity_pc_${a_sha}_${b_sha}_$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
WORK=$(mktemp -d /tmp/granpc.XXXXXX)
NS1=granpc1-$$
NS2=granpc2-$$
LOAD_PIDS=()
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
stop_load() {
    local pid
    for pid in "${LOAD_PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    LOAD_PIDS=()
}
cleanup() {
    stop_load
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

say "=== granularity PC check A=$a_sha B=$b_sha reps=$REPS $(date -Is) ==="
build_arm() { # build_arm <arm> <rev> <extra cflags>
    local arm=$1 rev=$2 extra=$3 sz
    mkdir -p "$WORK/src_$arm" "$WORK/home_$arm"
    git -C "$REPO" archive "$rev" | tar -x -C "$WORK/src_$arm" || { say "FATAL cannot export $rev"; exit 1; }
    for sz in p1 p4; do
        # Each arm installs libtickle.a under its own HOME, so no arm can be handed another's core.
        (cd "$WORK/src_$arm/examples/perf_hil/tickle" && HOME="$WORK/home_$arm" TICKLE_EXTRA_CFLAGS="$extra" \
            ./build.sh reliable_throughput "$sz") >"$WORK/build_${arm}_$sz.log" 2>&1 ||
            { say "FATAL build $arm $sz"; tail -5 "$WORK/build_${arm}_$sz.log" | tee -a "$OUT"; exit 1; }
        mkdir -p "$WORK/bin_${arm}_$sz"
        cp "$WORK/src_$arm/examples/perf_hil/tickle/reliable_throughput_$sz/client" \
            "$WORK/src_$arm/examples/perf_hil/tickle/reliable_throughput_$sz/server" "$WORK/bin_${arm}_$sz/"
        say "IDENTITY arm=$arm rev=$rev extra='$extra' size=$sz client=$(sha256sum "$WORK/bin_${arm}_$sz/client" | cut -c1-16)"
    done
}
build_arm A "$a_sha" ""
build_arm B "$b_sha" ""
build_arm Bfix "$b_sha" "-Dtt_RELIABLE_RETRY_GRANULARITY=100000"

# The probe, from arm B's tree: it #includes that tree's src/tickle.c (whitebox, as the unit tests), in the default
# configuration.
P="$WORK/src_B"
gcc -O2 -I"$P/include" -I"$P/src" -o "$WORK/probe" "$P/examples/perf_hil/experiments/timer_lateness_probe.c" \
    "$P/src/hal_linux.c" "$P/src/encoding.c" "$P/src/log.c" -lpthread -lm >"$WORK/probe_build.log" 2>&1 ||
    { say "FATAL probe build"; tail -5 "$WORK/probe_build.log" | tee -a "$OUT"; exit 1; }

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || { say "FATAL netns"; exit 1; }
sudo -n ip link add granpc1 netns "$NS1" type veth peer name granpc2 netns "$NS2" || { say "FATAL veth"; exit 1; }
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 broadcast 192.168.10.255 dev granpc1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 broadcast 192.168.10.255 dev granpc2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set granpc1 up
sudo -n ip -n "$NS2" link set granpc2 up

say "--- part 1: G measured by the context (probe, a 1 ms retry timer, 5 s) ---"
for load in idle loaded; do
    if [ "$load" = loaded ]; then
        for _ in $(seq 1 "$(nproc)"); do
            yes >/dev/null &
            LOAD_PIDS+=("$!")
        done
        sleep 1
    fi
    for rep in $(seq 1 "$REPS"); do
        # shellcheck disable=SC2024 # the log is ours
        line=$(sudo -n ip netns exec "$NS1" "$WORK/probe" 1000 5 2>&1 | grep '^PROBE')
        say "load=$load rep=$rep ${line:-PROBE MISSING}"
    done
    stop_load
done

sudo -n ip netns exec "$NS1" tc qdisc add dev granpc1 root netem loss 5% || { say "FATAL netem"; exit 1; }
say "$(sudo -n ip netns exec "$NS1" tc qdisc show dev granpc1)"
say "--- part 2: lossy cells c5 (p1) and c6 (p4), 5% loss ---"
run_cell() { # run_cell <rep> <cell> <arm>
    local rep=$1 cell=$2 arm=$3 sz args
    if [ "$cell" = c5 ]; then sz=p1; args="-Q -N 2048 -B 100"; else sz=p4; args="-Q -N 187 -B 100"; fi
    args="$args -d 5 --warmup-s 2 --cooldown-s 2"
    local bin="$WORK/bin_${arm}_$sz"
    # shellcheck disable=SC2086,SC2024 # args split on purpose; logs are ours
    sudo -n ip netns exec "$NS2" env BENCH_IFACE=granpc2 timeout 120 "$bin/server" $args >"$WORK/srv.log" 2>&1 &
    local srv=$! # sudo's PID, captured at launch: sudo relays the SIGINT below to the server it runs
    sleep 3
    # shellcheck disable=SC2086,SC2024
    sudo -n ip netns exec "$NS1" env BENCH_IFACE=granpc1 timeout 120 "$bin/client" $args >"$WORK/cli.log" 2>&1
    sleep 1
    kill -INT "$srv" 2>/dev/null
    wait "$srv"
    local tag="rep=$rep cell=$cell arm=$arm"
    { grep -h '^RESULT' "$WORK/cli.log" | sed "s/^/$tag client /"
        grep -h '^RESULT' "$WORK/srv.log" | sed "s/^/$tag server /"; } >>"$OUT"
    grep -q '^RESULT' "$WORK/cli.log" || { say "$tag: client printed no RESULT; its last lines:"; tail -4 "$WORK/cli.log" | tee -a "$OUT"; }
}
arms=(A B Bfix)
for rep in $(seq 1 "$REPS"); do
    k=$(((rep - 1) % 3))
    order="${arms[*]:$k} ${arms[*]:0:$k}"
    say "--- rep $rep/$REPS $(date -Is) order: $order ---"
    for cell in c5 c6; do
        for arm in $order; do run_cell "$rep" "$cell" "$arm"; done
    done
done
say "=== runs done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
ident, res, probe = {}, {}, []
for line in open(sys.argv[1]):
    m = re.match(r"IDENTITY arm=(\S+) .* size=(\S+) client=(\S+)", line)
    if m:
        ident[(m.group(1), m.group(2))] = m.group(3)
        continue
    m = re.match(r"load=(\S+) rep=\d+ PROBE (.*)", line)
    if m:
        probe.append((m.group(1), dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)))
        continue
    m = re.match(r"rep=(\d+) cell=(\S+) arm=(\S+) (client|server) RESULT:? (.*)", line)
    if m:
        f = dict(kv.split("=", 1) for kv in m.group(5).split() if "=" in kv)
        res.setdefault((m.group(2), m.group(3), m.group(1)), {})[m.group(4)] = f

print("=== ANALYSIS (pre-registered in the header) ===")
for load in ("idle", "loaded"):
    rows = [p for l, p in probe if l == load]
    for p in rows:
        g, p90, p99, resn = (int(p[k]) for k in ("g_ns", "late_p90_ns", "late_p99_ns", "resolution_ns"))
        ok = p90 <= g <= 4 * p99 + resn
        print(f"G {load:6s} g={g/1000:8.1f} us mean={int(p['mean_ns'])/1000:7.1f} p50={int(p['late_p50_ns'])/1000:7.1f} "
              f"p90={p90/1000:7.1f} p99={p99/1000:7.1f} p99.9={int(p['late_p999_ns'])/1000:7.1f} "
              f"entries={p['entries']} -> {'AS INTENDED' if ok else 'OUTSIDE p90..4xp99'}")
    if rows:
        print(f"G {load} median {st.median(int(p['g_ns']) for p in rows)/1000:.1f} us over {len(rows)} runs")
for sz in ("p1", "p4"):
    same = ident.get(("B", sz)) == ident.get(("Bfix", sz))
    print(f"TREATMENT {sz}: B and Bfix clients {'IDENTICAL - the control did not build differently: VOID' if same else 'differ'}")

def valid(r):
    c, s = r.get("client"), r.get("server")
    return c is not None and s is not None and c.get("drained") == "acked" and s.get("lost") == "0"

for cell in ("c5", "c6"):
    vals = {}
    for arm in ("A", "B", "Bfix"):
        reps = [v for (c, a, _), v in res.items() if c == cell and a == arm]
        good = [r for r in reps if valid(r)]
        print(f"{cell} {arm:4s}: {len(good)}/{len(reps)} valid reps"
              + (f"; server timer_lateness_ns= {[r['server'].get('timer_lateness_ns') for r in good]}" if arm != "A" else ""))
        vals[arm] = good
    if any(len(vals[a]) < 3 for a in vals):
        print(f"{cell}: VOID (fewer than 3 valid reps in an arm)")
        continue
    for metric, better in (("send_mbps", +1), ("wire_bytes_per_sample", -1)):
        xs = {a: [float(r["client"][metric]) for r in vals[a]] for a in vals}
        ma, mb, mc = (st.median(xs[a]) for a in ("A", "B", "Bfix"))
        db, dc = mb - ma, mc - ma
        se = (st.variance(xs["A"]) / len(xs["A"]) + st.variance(xs["B"]) / len(xs["B"])) ** 0.5
        t = (st.mean(xs["B"]) - st.mean(xs["A"])) / se if se > 0 else float("inf") if db else 0.0
        if abs(t) > 2 and abs(db) > abs(dc) and db * better < 0:
            verdict = "REGRESSED"
        elif abs(t) > 2 and abs(db) > abs(dc) and db * better > 0:
            verdict = "IMPROVED"
        else:
            verdict = "HELD"
        print(f"{cell} {metric:22s} A {ma:10.3f}  B {mb:10.3f} ({100*db/ma:+.2f}%)  Bfix {mc:10.3f} ({100*dc/ma:+.2f}%)"
              f"  t {t:+.2f}  -> {verdict}")
PYEOF
say "=== done $(date -Is) ==="

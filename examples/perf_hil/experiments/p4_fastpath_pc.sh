#!/usr/bin/env bash
# Does the RELIABLE in-order fragment fast path (frag_fast_take(), src/tickle.c, 2026-10-06) close the same-host
# p4 - p3 latency gap? Before/after on THIS PC, in a private network namespace. A PC figure for the decision to
# A/B it on the rig, never a published one.
#
# WHAT CHANGED. A RELIABLE Subscriber used to store every DATA_FRAG fragment in its reorder slot, in order or not,
# and copy the sample out of the ring once whole (p4_reorder_firsttouch.sh, H-RING): with the bench's 4096 x 2840 B
# ring that is 5.7 KB of never-touched memory per round trip per process for the first lap of 2048 samples - page
# faults - and cold ring lines after it. In order, a sample is now put together in the node's frag_scratch and never
# touches the ring. A p3 sample is a DATA and never touched it before or after.
#
# ARMS: {before, after} x {p3, p4} x {first, warm}, interleaved per rep (the arm order alternates by rep).
#   first: -W 0 -C 0 -i 0.005 -d 8, about 1,600 round trips, all inside the ring's first lap.
#   warm:  -W 4096 -C 4096 -I 0.001 -i 0.005 -d 8, the bench's own warm-up (two laps) before the measured window.
#   Both processes run under rusage_run.py, so each reports its minor page faults.
#
# HOW TO READ IT, written before running and enforced in the analysis below.
#   IDENTITY: p4's client binary must differ between before and after (else the change never reached it: VOID).
#     p3 builds without fragmentation (tt_MAX_SAMPLE_LENGTH 1472), where every line of the change is compiled out,
#     so its before and after binaries are expected to be byte-identical: that is what makes p3 the control the
#     change provably cannot touch. If they differ, it is printed, and p3 is still the control.
#   SAMPLES: a client RESULT counts only with measured >= 1000 round trips and loss_pct = 0; the first window
#     must also have measured <= 2048 (inside lap one). A cell with fewer than 3 such reps is VOID.
#   TREATMENT: in the first window, after's p4 client must take fewer minor faults per round trip than before's,
#     by at least 0.5 (two ring pages per sample is ~1.4 per round trip). If not, the fast path did not run and
#     no latency reading is made for that window.
#   CONTROL: delta(p3) = median p50 after - before. The change cannot touch p3, so |delta(p3)| is this PC's
#     run-to-run noise at this design.
#   READING, per window: gap = median p4 p50 - median p3 p50, per arm; closed = gap(before) - gap(after).
#     CLOSES if closed >= 2 us and closed > 2 x |delta(p3)|.
#     NO EFFECT if |closed| <= 1 us, or |closed| <= 2 x |delta(p3)|.
#     WORSE if closed <= -2 us and -closed > 2 x |delta(p3)|.
#     PARTIAL otherwise, with the numbers.
#
# Usage: [BEFORE=<rev>] [AFTER=<rev>] [REPS=5] p4_fastpath_pc.sh     (both revisions must be committed)
# Launch detached; results in ~/rig_results_safe/. Never touches the rig.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# This machine's benchmarks contend for this machine: the box lock, taken inside the detached process.
export RIG_LOCK_SCOPE=box
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
BEFORE=${BEFORE:-HEAD~1}
AFTER=${AFTER:-HEAD}
REPS=${REPS:-5}
DUR=${DUR:-8}
PIN_SERVER=${PIN_SERVER:-}
PIN_CLIENT=${PIN_CLIENT:-}
before_sha=$(git -C "$REPO" rev-parse --short "$BEFORE") || exit 1
after_sha=$(git -C "$REPO" rev-parse --short "$AFTER") || exit 1
NS=tickle_p4fp_$$
OUT=${OUT:-$HOME/rig_results_safe/p4_fastpath_pc_${before_sha}_${after_sha}_$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
WORK=$(mktemp -d /tmp/p4fp.XXXXXX)
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

say "=== p4 fast path, PC, before=$before_sha after=$after_sha reps=$REPS dur=$DUR $(date -Is) ==="
for arm in before after; do
    rev=$before_sha
    [ "$arm" = after ] && rev=$after_sha
    mkdir -p "$WORK/src_$arm" "$WORK/home_$arm"
    git -C "$REPO" archive "$rev" | tar -x -C "$WORK/src_$arm" || { say "FATAL cannot export $rev"; exit 1; }
    for sz in p3 p4; do
        # Each arm installs libtickle.a under its own HOME: build.sh reinstalls only when src/ is newer than the
        # installed library, so a shared prefix would hand one arm the other's core.
        (cd "$WORK/src_$arm/examples/perf_hil/tickle" && HOME="$WORK/home_$arm" ./build.sh reliable_latency "$sz") \
            >"$WORK/build_${arm}_$sz.log" 2>&1 || { say "FATAL build $arm $sz"; tail -5 "$WORK/build_${arm}_$sz.log"; exit 1; }
        mkdir -p "$WORK/bin_${arm}_$sz"
        cp "$WORK/src_$arm/examples/perf_hil/tickle/reliable_latency_$sz/client" \
            "$WORK/src_$arm/examples/perf_hil/tickle/reliable_latency_$sz/server" "$WORK/bin_${arm}_$sz/"
        say "IDENTITY arm=$arm size=$sz client=$(sha256sum "$WORK/bin_${arm}_$sz/client" | cut -c1-16)"
    done
done

sudo -n ip netns add "$NS" || { say "FATAL cannot create netns"; exit 1; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
sudo -n ip netns exec "$NS" ip link add bench0 type dummy
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip netns exec "$NS" ip link set bench0 up

RU="$REPO/examples/perf_hil/experiments/rusage_run.py"
run_cell() { # run_cell <rep> <window> <size> <arm>
    local rep=$1 win=$2 sz=$3 arm=$4 wargs srv_d
    if [ "$win" = first ]; then
        wargs="-W 0 -C 0"
        srv_d=$((DUR + 6))
    else
        wargs="-W 4096 -C 4096 -I 0.001"
        srv_d=$((DUR + 20))
    fi
    local bin="$WORK/bin_${arm}_$sz"
    # shellcheck disable=SC2024,SC2086 # the logs stay ours; PIN_* and wargs are meant to split
    sudo -n ip netns exec "$NS" env BENCH_IFACE=lo timeout $((srv_d + 30)) $PIN_SERVER python3 "$RU" \
        "$bin/server" -d "$srv_d" $wargs >"$WORK/srv.log" 2>&1 &
    local srv=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS" env BENCH_IFACE=lo timeout $((srv_d + 30)) $PIN_CLIENT python3 "$RU" \
        "$bin/client" -i 0.005 -d "$DUR" $wargs >"$WORK/cli.log" 2>&1
    wait "$srv"
    local tag="rep=$rep window=$win size=$sz arm=$arm"
    { grep -h '^RESULT' "$WORK/cli.log" | sed "s/^/$tag client /"
        grep -h '^RUSAGE' "$WORK/cli.log" | sed "s/^/$tag client /"
        grep -h '^RESULT' "$WORK/srv.log" | sed "s/^/$tag server /"
        grep -h '^RUSAGE' "$WORK/srv.log" | sed "s/^/$tag server /"; } >>"$OUT"
    grep -q '^RESULT' "$WORK/cli.log" || { say "$tag: client printed no RESULT; its last lines:"; tail -4 "$WORK/cli.log" | tee -a "$OUT"; }
}

for rep in $(seq 1 "$REPS"); do
    order="before after"
    [ $((rep % 2)) = 0 ] && order="after before"
    say "--- rep $rep/$REPS $(date -Is) order: $order ---"
    for win in first warm; do
        for sz in p3 p4; do
            for arm in $order; do run_cell "$rep" "$win" "$sz" "$arm"; done
        done
    done
done
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
ident, res, ru = {}, {}, {}
for line in open(sys.argv[1]):
    m = re.match(r"IDENTITY arm=(\S+) size=(\S+) client=(\S+)", line)
    if m:
        ident[(m.group(1), m.group(2))] = m.group(3)
        continue
    m = re.match(r"rep=(\d+) window=(\S+) size=(\S+) arm=(\S+) (client|server) (RESULT|RUSAGE):? (.*)", line)
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(7).split() if "=" in kv)
    key = (m.group(2), m.group(3), m.group(4), m.group(1), m.group(5))
    (res if m.group(6) == "RESULT" else ru)[key] = f

if ident.get(("before", "p4")) == ident.get(("after", "p4")):
    print("VOID: p4's client is byte-identical before and after - the change never reached it"); sys.exit(0)
print("p3 binaries " + ("identical before/after (control untouched by construction)"
      if ident.get(("before", "p3")) == ident.get(("after", "p3")) else "DIFFER before/after - p3 still the control"))

def cell(win, sz, arm):
    p50, flt, void = [], [], []
    for (w, s, a, rep, role), f in sorted(res.items()):
        if (w, s, a, role) != (win, sz, arm, "client"):
            continue
        n = int(f.get("measured", "0"))
        if n < 1000 or f.get("loss_pct") != "0" or (win == "first" and n > 2048):
            void.append(f"rep {rep}: measured={n} loss_pct={f.get('loss_pct')}"); continue
        p50.append(float(f["rtt_p50_ms"]) * 1000.0)
        r = ru.get((w, s, a, rep, "client"))
        if r is not None:
            flt.append(int(r["minflt"]) / n)
    return p50, flt, void

verdicts = {}
for win in ("first", "warm"):
    print(f"\n=== window {win} ===")
    cells, ok = {}, True
    for sz in ("p3", "p4"):
        for arm in ("before", "after"):
            p50, flt, void = cell(win, sz, arm)
            for v in void:
                print(f"  dropped {sz} {arm} {v}")
            if len(p50) < 3:
                print(f"  VOID {sz} {arm}: {len(p50)} usable reps"); ok = False; continue
            cells[(sz, arm)] = (st.median(p50), st.median(flt) if flt else float("nan"), p50)
            print(f"  {sz} {arm:6s} p50 median {st.median(p50):7.1f} us  reps {['%.1f' % x for x in p50]}  "
                  f"client minflt/rt {cells[(sz, arm)][1]:.3f}")
    if not ok:
        print(f"  window {win}: NO READING"); continue
    if win == "first":
        dflt = cells[("p4", "before")][1] - cells[("p4", "after")][1]
        if not dflt >= 0.5:
            print(f"  TREATMENT NOT SEEN: p4 client faults per round trip fell by {dflt:.3f} (< 0.5) - no reading")
            continue
        print(f"  treatment: p4 client faults per round trip fell by {dflt:.3f}")
    d3 = cells[("p3", "after")][0] - cells[("p3", "before")][0]
    gb = cells[("p4", "before")][0] - cells[("p3", "before")][0]
    ga = cells[("p4", "after")][0] - cells[("p3", "after")][0]
    closed = gb - ga
    noise = 2 * abs(d3)
    if closed >= 2 and closed > noise:
        v = "CLOSES"
    elif abs(closed) <= 1 or abs(closed) <= noise:
        v = "NO EFFECT"
    elif closed <= -2 and -closed > noise:
        v = "WORSE"
    else:
        v = "PARTIAL"
    print(f"  gap p4-p3: before {gb:+.1f} us, after {ga:+.1f} us; closed {closed:+.1f} us; "
          f"control delta(p3) {d3:+.1f} us -> {v}")
PYEOF
echo "raw: $OUT"

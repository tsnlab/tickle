#!/usr/bin/env bash
# layout_control.sh - is a small cross-host difference between two commits function layout, or work?
#
# p1_layout_check.sh generalised (WIRE_PLAN 10.4): any scenario, several sizes, and the metric chosen by the caller.
# Written for the 2026-10-05 safety A-B-B-A (8d1c3712 -> 63f00280), where the strict 2xSE rule flagged server CPU per
# sample +0.1..0.5% on all three BEST_EFFORT cells (c8 p1, c13 p2, c15 p4) - the subscriber side, which neither change
# was read to touch. Reading is not evidence; this is the control that can decide it.
#
# Four arms per size: A and B as built, Aa and Ba with -falign-functions=32 (changes nothing the program computes).
#
# HOW TO READ IT, written before the run (METRIC is "higher is worse", e.g. server cpu_s_per_Msample):
#   - A->B does not show B worse at t > 2              -> VOID for that size: tonight's rig does not reproduce it.
#   - A->B shows it, Aa->Ba within |t| <= 2            -> LAYOUT: aligning both closes the gap.
#   - A->B shows it, Aa->Ba REVERSES (Ba better, t<-2) -> LAYOUT_DOMINATED: a neutral flag flips its sign.
#   - A->B shows it, Aa->Ba keeps it (Ba worse, t > 2) -> NOT_LAYOUT: a real cost of B; find it.
#   - the flag alone (A->Aa or B->Ba) moving the metric by more than the A->B gap is reported as the FLOOR.
#   - a run without the metric on its RESULT line, or a client sending < 10000 samples, is VOID; an arm with < 3
#     usable runs at a size gives that size no verdict.
#
# Usage: A=<sha> B=<sha> SCEN=best_effort_throughput SIZES="p1 p2 p4" METRIC=cpu_s_per_Msample ROLE=server \
#        BLOCKS=3 layout_control.sh        Output: $OUT
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
A=${A:?A=<sha>}
B=${B:?B=<sha>}
SCEN=${SCEN:-best_effort_throughput}
SIZES=${SIZES:-p1 p2 p4}
METRIC=${METRIC:-cpu_s_per_Msample}
ROLE=${ROLE:-server}
BLOCKS=${BLOCKS:-3}
ALIGN=${ALIGN:--falign-functions=32}
DUR=${DUR:-10}
OUT=${OUT:-$HOME/rig_results_safe/layout_control.txt}
K=$HOME/.ssh/tickle_ci_ed25519
CLIENT=10.1.1.214
SERVER=10.1.1.213
SAVE=/tmp/laycon
srv_pid=""
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
note() { echo "$*" >>"$OUT"; echo "$*" >&2; }

cleanup() {
    [ -z "${srv_pid:-}" ] && return 0
    sh_ "$SERVER" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/server*) kill -INT $srv_pid;; esac" \
        </dev/null >/dev/null 2>&1
    srv_pid=""
}
trap cleanup EXIT

say "=== layout control $(date -Is) A=$A B=$B scen=$SCEN sizes='$SIZES' metric=$ROLE.$METRIC align='$ALIGN' blocks=$BLOCKS dur=$DUR ==="

build_arm() { # build_arm <sha> <arm> <size> <extra-cflags>; prints the client sha256
    local sha=$1 name=$2 size=$3 extra=$4 h out
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log
cat > examples/perf_hil/tickle/common/BenchStats.h" <"$REPO/examples/perf_hil/tickle/common/BenchStats.h" >/dev/null \
            || { note "FATAL checkout failed for $sha on $h"; return 1; }
        sh_ "$h" "cat > ~/tickle/examples/perf_hil/tickle/build.sh && chmod +x ~/tickle/examples/perf_hil/tickle/build.sh" \
            <"$REPO/examples/perf_hil/tickle/build.sh" >/dev/null || { note "FATAL build.sh copy failed on $h"; return 1; }
        out=$(sh_ "$h" "set -e; cd ~/tickle/examples/perf_hil/tickle && TICKLE_EXTRA_CFLAGS='$extra' ./build.sh $SCEN $size > /tmp/laycon_build.log 2>&1 || { echo BUILD_FAILED; tail -3 /tmp/laycon_build.log; exit 0; }
mkdir -p $SAVE/$size/$name && cp ${SCEN}_${size}/client ${SCEN}_${size}/server $SAVE/$size/$name/ && sha256sum $SAVE/$size/$name/client | cut -c1-16" </dev/null 2>&1 | tail -1)
        case "$out" in *BUILD_FAILED*) note "FATAL build failed for $name/$size on $h: $out"; return 1;; esac
    done
    note "  arm $name/$size ($sha, extra='$extra') built, client sha256=$out"
    echo "$out"
}

run_one() { # run_one <size> <arm>
    local size=$1 name=$2 cline sline
    cleanup
    srv_pid=$(sh_ "$SERVER" "cd $SAVE/$size/$name && rm -f /tmp/laycon_server.pid /tmp/laycon_server.log && (setsid sh -c 'echo \$\$ > /tmp/laycon_server.pid; exec taskset -c 1-3 ./server -Q -d $((DUR + 6))' > /tmp/laycon_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/laycon_server.pid" </dev/null)
    cline=$(sh_ "$CLIENT" "cd $SAVE/$size/$name && taskset -c 1-3 ./client -Q -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    sline=$(sh_ "$SERVER" "for i in \$(seq 1 20); do grep -q '^RESULT' /tmp/laycon_server.log && break; sleep 1; done; grep '^RESULT' /tmp/laycon_server.log" </dev/null)
    srv_pid=""
    [ -n "$cline" ] && say "size=$size arm=$name $cline"
    [ -n "$sline" ] && say "size=$size arm=$name $sline"
    [ -n "$cline" ] && [ -n "$sline" ] || say "    NORESULT size=$size arm=$name"
}

for size in $SIZES; do
    sa=$(build_arm "$A" A "$size" "") || exit 1
    sb=$(build_arm "$B" B "$size" "") || exit 1
    saa=$(build_arm "$A" Aa "$size" "$ALIGN") || exit 1
    sba=$(build_arm "$B" Ba "$size" "$ALIGN") || exit 1
    say "  $size built: A=$sa Aa=$saa B=$sb Ba=$sba"
    if [ "$sa" = "$saa" ] || [ "$sb" = "$sba" ]; then
        say "FATAL $size: a forced arm equals its unforced twin - '$ALIGN' never reached the compiler. No verdict."
        exit 1
    fi
done

for b in $(seq 1 "$BLOCKS"); do
    say "--- block $b/$BLOCKS $(date -Is) ---"
    for size in $SIZES; do
        for arm in A B Ba Aa Aa Ba B A; do
            run_one "$size" "$arm"
        done
    done
done
cleanup
say "=== done $(date -Is) ==="

python3 - "$OUT" "$METRIC" "$ROLE" "$SIZES" <<'PYEOF' | tee -a "$OUT"
import math, re, sys
path, metric, role, sizes = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4].split()
rows, sent = {}, {}
for line in open(path):
    m = re.match(r"size=(\S+) arm=(\w+) (RESULT.*)", line.strip())
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(3).split() if "=" in kv)
    key = (m.group(1), m.group(2))
    if f.get("role") == "client":
        sent.setdefault(key, []).append(int(f.get("sent", "0")))
    if f.get("role") == role and metric in f:
        rows.setdefault(key, []).append(float(f[metric]))

def stat(v):
    n = len(v); mu = sum(v) / n
    return mu, (0.0 if n < 2 else math.sqrt(sum((x - mu) ** 2 for x in v) / (n - 1) / n)), n

for size in sizes:
    print(f"=== {size}: {role}.{metric} (higher is worse) ===")
    if any(s < 10000 for arm in "A B Aa Ba".split() for s in sent.get((size, arm), [0])):
        print("  VOID: a client sent fewer than 10000 samples"); continue
    if any(len(rows.get((size, arm), [])) < 3 for arm in ("A", "B", "Aa", "Ba")):
        print("  VOID: an arm has fewer than 3 usable runs"); continue
    def gap(x, y):
        mx, sx, nx = stat(rows[(size, x)]); my, sy, ny = stat(rows[(size, y)])
        den = math.hypot(sx, sy); t = 0.0 if den == 0 else (my - mx) / den
        pct = 100 * (my - mx) / mx
        print(f"  {x} {mx:.4f}+-{sx:.4f} (n={nx}) -> {y} {my:.4f}+-{sy:.4f} (n={ny}):  {pct:+.3f}%  t={t:.2f}")
        return pct, t
    p_u, t_u = gap("A", "B")
    p_f, t_f = gap("Aa", "Ba")
    p_a, t_a = gap("A", "Aa")
    p_b, t_b = gap("B", "Ba")
    if not (t_u > 2):
        print("  VERDICT=VOID - A->B does not show B worse here tonight; nothing to attribute")
    elif abs(t_f) <= 2:
        print(f"  VERDICT=LAYOUT - aligning both closes the gap ({p_f:+.3f}%, t={t_f:.2f}) while unforced it is {p_u:+.3f}%")
    elif t_f < -2:
        print(f"  VERDICT=LAYOUT_DOMINATED - the gap reverses under a forced alignment ({p_f:+.3f}%, t={t_f:.2f})")
    else:
        print(f"  VERDICT=NOT_LAYOUT - the gap survives a forced alignment ({p_f:+.3f}%, t={t_f:.2f}); a real cost of B")
    for name, p, t in (("A->Aa", p_a, t_a), ("B->Ba", p_b, t_b)):
        if abs(t) > 2 and abs(p) >= abs(p_u):
            print(f"  FLOOR: the flag alone moved {name} by {p:+.3f}% (t={t:.2f}), at least the A->B gap")
PYEOF

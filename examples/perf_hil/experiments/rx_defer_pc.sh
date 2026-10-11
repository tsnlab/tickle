#!/usr/bin/env bash
# rx_defer_pc.sh - receive-path A/B for large samples on this PC: the subscriber's instructions, wakes and latency per MB
# delivered, base (A) against a candidate (B), with an A/A control (A2: A's binary again) and the candidate's own
# switch-off (Boff: B's binary with TT_RX_DEFER=0 and TT_RX_DIRECT=0 at the subscriber - what the code costs while
# it does nothing). Receiving in place is off by default, so TREATMENT=direct runs B with TT_RX_DIRECT=1. Two private namespaces joined by a veth pair; large_sample_bench.c at both ends, built as
# largemsg_cpu_profile.sh builds it (rmw's datagram, large samples, no segment: the namespaces share /dev/shm).
#
# The rig's arrival pattern, reproduced: on the Pis a 1 MB sample's datagrams arrive at line rate and the NIC raises
# its interrupt every ~49 us (macb rx-usecs 49), so the receiver is woken every ~5 datagrams in every receive method
# (rx_gro_probe_rig.sh, 2026-10-10). A bare veth delivers a whole send call at once instead. So the publisher's veth
# end carries netem "rate 1gbit slot 49us 49us" - datagrams paced at 1 Gbit/s and released in 49 us slots - and the
# publisher sends one datagram per skb (TT_UDP_OFFLOAD=0 at the publisher only: netem paces a UDP_SEGMENT run as one
# packet). The subscriber keeps its receive offload as rmw_tickle has it.
#
# NOT a CPU-time result: perf stat instruction counts on a shared PC. The rig A/B says what it costs (rx_defer task).
#
# Cells: rel1m - 1 MB RELIABLE KEEP_LAST 10, 30 Hz (the rig's I1 shape); rel4m - 4 MB, 15 Hz (I1's Array4m);
# small - 1000 B RELIABLE at 1000 Hz, not a large sample (nothing to defer or place: the cost of the code, unused);
# and the two stall cells, where the subscriber spins STALL_US in its callback after every sample, as an rmw
# application decoding a large message does, so its socket fills with nobody reading: stall1m - rel1m with the stall;
# stall4m - 4 MB BEST_EFFORT offered at 40 Hz, more than the link carries (I4s's saturation). A deferral leaves
# datagrams in the socket on purpose, which is the same headroom a stall spends.
# Both sockets are granted what the rig's are (tt_SOCKET_BUFFER_SIZE 212,992, granted 425,984: budget 36).
# Socket drops are the kernel's own count, Udp RcvbufErrors in the subscriber's namespace, before and after each run.
#
# HOW TO READ IT (written before the first run, enforced in the analysis below):
#   - VOID run: no RESULT line from either side, delivered < 95% of published, or torn > 0.
#   - treatment, per run: A and A2 report no rx_defers field or 0; B in rel1m/rel4m rx_defers > 0 (with
#     TREATMENT=defer) or direct_placed > 0 (TREATMENT=direct); Boff reports 0 of both; small: 0 of both in every arm.
#     A run failing its treatment is VOID_treatment and not counted.
#   - per cell and arm, the median over reps of: instructions (user + kernel) per MB delivered, voluntary context
#     switches per MB (the wakes), latency median and p99.
#   - noise = |A2 - A|; tolerance = max(noise, 1% of A) for instructions and wakes, max(noise, 2% of A) for latency.
#   - B BETTER / WORSE / SAME on each metric by that tolerance. Boff must be SAME as A on instructions (the code's cost
#     when it does nothing) - otherwise the line says COST WHEN OFF.
#   - socket drops, per stall cell: the sum over reps of B (and of Boff) must not exceed the larger of A's and A2's
#     sums, and must be 0 where both are 0; otherwise the line says DROPS. Every other cell must drop nothing in any
#     arm (a drop there is printed). Stall cells are read for drops only, not for the hypothesis's instructions.
#     If A or A2 drops in a cell without a stall, the setup drops on its own (a loaded PC preempts the reader as a stall
#     would - 2026-10-11, load ~7-18): the drop reading is VOID, said so, and left out of the hypothesis, which then
#     stands on the other metrics only and says that its drop half is unread.
#   - the hypothesis (TREATMENT=defer): rel1m and rel4m wakes BETTER and instructions BETTER, latency median not WORSE,
#     small SAME on every metric, and no DROPS. Falsified when rel1m wakes or instructions are not BETTER, or a stall
#     cell DROPS.
#   - the hypothesis (TREATMENT=direct): rel1m and rel4m instructions BETTER, wakes and latency not WORSE, small SAME.
# Usage: A=<rev> B=<rev> REPS=5 TREATMENT=defer|direct rx_defer_pc.sh    Launch detached; never the rig.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=box
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
A=${A:?A=<base rev>}
B=${B:-HEAD}
REPS=${REPS:-5}
DUR=${DUR:-10}
TREATMENT=${TREATMENT:-defer}
CELLS=${CELLS:-rel1m rel4m small stall1m stall4m}
STALL_US=${STALL_US:-5000}
a_sha=$(git -C "$REPO" rev-parse --short "$A") || exit 1
b_sha=$(git -C "$REPO" rev-parse --short "$B") || exit 1
OUT=${OUT:-$HOME/rig_results_safe/rx_defer_pc_${TREATMENT}_${b_sha}_$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
WORK=$(mktemp -d /tmp/rxdefer.XXXXXX) || exit 1
NSP=rxd-pub-$$
NSS=rxd-sub-$$
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
cleanup() {
    local ns pids
    for ns in "$NSP" "$NSS"; do
        pids=$(sudo -n ip netns pids "$ns" 2>/dev/null | tr '\n' ' ')
        # shellcheck disable=SC2086
        [ -n "${pids// /}" ] && sudo -n ip netns exec "$ns" kill -KILL $pids 2>/dev/null
        sudo -n ip netns del "$ns" 2>/dev/null
    done
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT

say "=== rx_defer_pc treatment=$TREATMENT A=$a_sha B=$b_sha reps=$REPS dur=${DUR}s cells='$CELLS' $(date -Is) ==="
for arm in B A; do # B first: both arms build B's bench (its -S stall), each against its own core
    rev=$a_sha
    [ "$arm" = B ] && rev=$b_sha
    mkdir -p "$WORK/src_$arm"
    git -C "$REPO" archive "$rev" | tar -x -C "$WORK/src_$arm" || { say "FATAL cannot export $rev"; exit 1; }
    t="$WORK/src_$arm"
    cc -O2 -DNDEBUG -Dtt_SOCKET_BUFFER_SIZE=212992 -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 -I"$t/include" -I"$t/src" \
        -o "$WORK/bench_$arm" "$WORK/src_B/examples/perf_hil/experiments/large_sample_bench.c" "$t/src/tickle.c" \
        "$t/src/encoding.c" "$t/src/log.c" "$t/src/hal_linux.c" -lpthread -lm >"$WORK/build_$arm.log" 2>&1 ||
        { say "FATAL build $arm"; tail -5 "$WORK/build_$arm.log" | tee -a "$OUT"; exit 1; }
    say "IDENTITY arm=$arm rev=$rev bench=$(sha256sum "$WORK/bench_$arm" | cut -c1-16)"
done

if ! sudo -n ip netns add "$NSP" || ! sudo -n ip netns add "$NSS"; then say "FATAL netns"; exit 1; fi
sudo -n ip link add rxdv1 netns "$NSP" type veth peer name rxdv2 netns "$NSS" || { say "FATAL veth"; exit 1; }
sudo -n ip -n "$NSP" addr add 192.168.10.1/24 brd + dev rxdv1
sudo -n ip -n "$NSS" addr add 192.168.10.2/24 brd + dev rxdv2
for ns in "$NSP" "$NSS"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NSP" link set rxdv1 up
sudo -n ip -n "$NSS" link set rxdv2 up
sudo -n ip netns exec "$NSP" tc qdisc add dev rxdv1 root netem limit 100000 rate 1gbit slot 49us 49us ||
    { say "FATAL netem"; exit 1; }
say "pub egress: $(sudo -n ip netns exec "$NSP" tc qdisc show dev rxdv1)"

args_of() { # cell -> "SIZE RATE RELIABLE STALL_US"
    case $1 in
    rel1m) echo "1048576 30 -R 0" ;;
    rel4m) echo "4194304 15 -R 0" ;;
    small) echo "1000 1000 -R 0" ;;
    stall1m) echo "1048576 30 -R $STALL_US" ;;
    stall4m) echo "4194304 40 - $STALL_US" ;;
    esac
}
rcvbuf_errors() { # the subscriber namespace's Udp RcvbufErrors
    sudo -n ip netns exec "$NSS" awk '/^Udp:/ { if (!h) { for (i = 1; i <= NF; i++) c[$i] = i; h = 1 } else print $c["RcvbufErrors"] }' /proc/net/snmp
}

run_cell() { # rep cell arm
    local rep=$1 cell=$2 arm=$3 bin_arm=$3 size rate rel stall env_sub=""
    [ "$arm" = A2 ] && bin_arm=A
    # Boff switches off only the treatment, so it is A's behaviour in B's binary: TREATMENT=direct runs on a base that
    # has the deferral, which Boff keeps (2026-10-11: the first direct run switched both off, and its Boff was void).
    [ "$arm" = Boff ] && bin_arm=B && env_sub="TT_RX_DEFER=0"
    [ "$arm" = Boff ] && [ "$TREATMENT" = direct ] && env_sub="TT_RX_DIRECT=0"
    [ "$arm" = B ] && [ "$TREATMENT" = direct ] && env_sub="TT_RX_DIRECT=1" # off by default
    read -r size rate rel stall <<<"$(args_of "$cell")"
    [ "$rel" = - ] && rel=""
    local bin="$WORK/bench_$bin_arm" drops_before
    drops_before=$(rcvbuf_errors)
    # shellcheck disable=SC2086,SC2024
    sudo -n ip netns exec "$NSS" env $env_sub perf stat -x, -e instructions:u,instructions:k,context-switches \
        -o "$WORK/sub.csv" -- "$bin" sub -s "$size" -d $((DUR + 8)) $rel -S "$stall" >"$WORK/sub.log" 2>&1 </dev/null &
    local sub=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NSP" env TT_UDP_OFFLOAD=0 timeout $((DUR + 30)) "$bin" pub -s "$size" -r "$rate" -d "$DUR" $rel \
        >"$WORK/pub.log" 2>&1 </dev/null
    wait "$sub"
    local drops=$(($(rcvbuf_errors) - drops_before))
    local res pres
    res=$(grep -m1 '^RESULT' "$WORK/sub.log")
    pres=$(grep -m1 '^RESULT' "$WORK/pub.log")
    if [ -z "$res" ] || [ -z "$pres" ]; then
        say "rep=$rep cell=$cell arm=$arm VOID no RESULT: $(tail -2 "$WORK/sub.log" "$WORK/pub.log" | tr '\n' ' ')"
        return
    fi
    local iu ik cs published
    iu=$(awk -F, '$3 ~ /^instructions:u/ { print $1 }' "$WORK/sub.csv")
    ik=$(awk -F, '$3 ~ /^instructions:k/ { print $1 }' "$WORK/sub.csv")
    cs=$(awk -F, '$3 ~ /^context-switches/ { print $1 }' "$WORK/sub.csv")
    published=$(grep -o 'published=[0-9]*' <<<"$pres" | head -1 | cut -d= -f2)
    local f
    f=$(tr ' ' '\n' <<<"$res" |
        grep -E '^(size|delivered|torn|latency_median_us|latency_p99_us|rx_defer_budget|rx_defers|direct_placed|gro_reads|gro_merged)=' |
        tr '\n' ' ')
    say "rep=$rep cell=$cell arm=$arm insns_u=$iu insns_k=$ik csw=$cs published=$published drops=$drops $f"
}

ARMS=(A A2 B Boff)
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep/$REPS $(date -Is) ---"
    for cell in $CELLS; do
        for k in 0 1 2 3; do run_cell "$rep" "$cell" "${ARMS[$(((k + rep) % 4))]}"; done
    done
done
say "=== runs done $(date -Is) ==="

python3 - "$OUT" "$TREATMENT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
path, treatment = sys.argv[1], sys.argv[2]
rows = {}
voids = []
drops = {}
for line in open(path):
    m = re.match(r"rep=(\d+) cell=(\S+) arm=(\S+) insns_u=(\d+) insns_k=(\d+) csw=(\d+) published=(\d+) drops=(-?\d+) (.*)", line)
    if not m:
        continue
    rep, cell, arm = m.group(1), m.group(2), m.group(3)
    f = dict(kv.split("=", 1) for kv in m.group(9).split() if "=" in kv)
    delivered, published, torn = int(f.get("delivered", 0)), int(m.group(7)), int(f.get("torn", 1))
    drops.setdefault((cell, arm), []).append(int(m.group(8)))
    if torn > 0:
        voids.append(f"VOID rep={rep} cell={cell} arm={arm} torn={torn}")
        continue
    if cell.startswith("stall"):
        continue  # read for drops only: a saturated best-effort cell delivers what it can
    if published == 0 or delivered < 0.95 * published:
        voids.append(f"VOID rep={rep} cell={cell} arm={arm} delivered={delivered} published={published} torn={torn}")
        continue
    defers, direct = int(f.get("rx_defers", 0)), int(f.get("direct_placed", 0))
    large = cell != "small"
    # Only the treatment's own counter is the check: a direct A/B's base has the deferral in every arm.
    treated = defers if treatment == "defer" else direct
    if arm in ("A", "A2", "Boff") or not large:
        ok = treated == 0
    else:
        ok = treated > 0
    if not ok:
        voids.append(f"VOID_treatment rep={rep} cell={cell} arm={arm} rx_defers={defers} direct_placed={direct}")
        continue
    mb = delivered * int(f["size"]) / 1e6
    rows.setdefault((cell, arm), []).append({
        "insns": (int(m.group(4)) + int(m.group(5))) / mb,
        "wakes": int(m.group(6)) / mb,
        "lat": float(f.get("latency_median_us", "nan")),
        "p99": float(f.get("latency_p99_us", "nan")),
    })
for v in voids:
    print(v)
verdicts = {}
for cell in ("rel1m", "rel4m", "small"):
    have = {a: rows.get((cell, a), []) for a in ("A", "A2", "B", "Boff")}
    if any(len(v) < 3 for v in have.values()):
        print(f"{cell}: NO READING (usable reps {({a: len(v) for a, v in have.items()})})")
        continue
    for metric, floor in (("insns", 0.01), ("wakes", 0.01), ("lat", 0.02), ("p99", 0.02)):
        med = {a: st.median(r[metric] for r in have[a]) for a in have}
        a = med["A"]
        tol = max(abs(med["A2"] - a), floor * a)
        def word(x):
            return "BETTER" if x < a - tol else "WORSE" if x > a + tol else "SAME"
        verdicts[(cell, metric)] = word(med["B"])
        off = word(med["Boff"])
        note = "" if metric != "insns" or off == "SAME" else f"  <- COST WHEN OFF ({off})"
        print(f"{cell:6} {metric:5} A {a:14.1f} A2 {med['A2']:14.1f} ({100*(med['A2']-a)/a:+.2f}%) "
              f"B {med['B']:14.1f} ({100*(med['B']-a)/a:+.2f}%) Boff {med['Boff']:14.1f} ({100*(med['Boff']-a)/a:+.2f}%) "
              f"tol {100*tol/a:.2f}% -> B {verdicts[(cell, metric)]}{note}")
drop_ok = True
drop_void = False
for (cell, arm), d in sorted(drops.items()):
    if not cell.startswith("stall") and sum(d) != 0:
        print(f"DROPS {cell} {arm}: {d} (a cell without a stall must drop nothing)")
        drop_ok = False
        drop_void = drop_void or arm in ("A", "A2")
if drop_void:
    print("DROPS VOID: the control (A/A2) dropped without a stall - this setup drops on its own; drops not read")
for cell in ("stall1m", "stall4m"):
    s = {a: sum(drops.get((cell, a), [])) for a in ("A", "A2", "B", "Boff")}
    if not all(drops.get((cell, a)) for a in s):
        print(f"{cell}: NO READING (drops {s})")
        continue
    bound = max(s["A"], s["A2"])
    bad = [a for a in ("B", "Boff") if s[a] > bound]
    print(f"{cell:7} drops (sum over reps) A {s['A']} A2 {s['A2']} B {s['B']} Boff {s['Boff']} -> "
          f"{'DROPS (' + ','.join(bad) + ')' if bad else 'no more than A'}")
    drop_ok = drop_ok and not bad
if drop_void:
    drop_ok = True  # unread, not passed: the HYPOTHESIS line says so
def v(cell, metric):
    return verdicts.get((cell, metric), "NO READING")
if treatment == "defer":
    held = all(v(c, "wakes") == "BETTER" and v(c, "insns") == "BETTER" and v(c, "lat") != "WORSE" for c in ("rel1m", "rel4m"))
else:
    held = all(v(c, "insns") == "BETTER" and v(c, "wakes") != "WORSE" and v(c, "lat") != "WORSE" for c in ("rel1m", "rel4m"))
held = held and drop_ok and all(v("small", m) == "SAME" for m in ("insns", "wakes", "lat"))
falsified = v("rel1m", "insns") != "BETTER" or (treatment == "defer" and v("rel1m", "wakes") != "BETTER") or not drop_ok
note = " (drops VOID: unread)" if drop_void else ""
print(f"HYPOTHESIS ({treatment}): {'HELD' if held else 'NOT HELD'}{' - FALSIFIED' if falsified else ''}{note}")
PYEOF
say "=== rx_defer_pc done $(date -Is) out=$OUT ==="

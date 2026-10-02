#!/usr/bin/env bash
# slot_depth_cost.sh - what does a slot big enough for p4 cost, when the segment stays the same size?
#
# WHY, BEFORE THE ENCODER WORK AND NOT AFTER. SHM_PLAN 6e's remaining step is to encode into the slot and
# stop fragmenting a sample bound only for shared memory, which requires a slot that holds a whole sample.
# Its pre-registration names two costs that pull against the gain and specifies a control arm for each:
# a shallower ring, and lost tx_buffer batching. This measures the first one NOW, because it needs no
# encoder change at all - only the slot size, which is a build flag today and configuration since e94f6240
# - and because designing the encoder work without knowing this number would be choosing blind.
#
# THE ARITHMETIC. segment_bytes = header + slots x (16 + slot_bytes), and tt_SEGMENT_BYTES is fixed at
# 512 KiB, so a slot sized for p4 halves the ring:
#
#   slot 1472 (today, the Ethernet UDP payload)   ~352 slots
#   slot 2816 (holds a 2800 B sample whole)       ~185 slots
#
# WHAT IS MEASURED, and the first version of this script got it wrong: **samples, not datagrams.** `tx_shm`
# counts datagrams placed in the ring - an event - while `sent` counts samples, which is the item a user
# gets. The first run read tx_shm within 10% across the arms and called depth free, while `sent` had fallen
# 924,830 -> 770,453 -> 682,948. Judging an item question on an event counter is how a 60x error got
# published here on 2026-09-29.
#
# AND IT NEEDS REPETITIONS, because this harness's own run-to-run spread is as large as the effect. Two
# arms of the SAME geometry in the mismatch experiment read 817,921 and 687,641 - 16% apart - so a single
# run per arm cannot separate a 17% difference from the noise. REPS defaults to 3 and the verdict uses the
# median with the per-arm range printed beside it; a difference inside the ranges is reported as a draw.
#
# HOW TO READ IT, written before the run:
#   the arms' sample ranges OVERLAP        -> depth is not separable at this rate. 6e's p4 prediction then
#        stands on batching alone, and a slot sized for p4 is free in back-pressure terms.
#   big separably BELOW small              -> depth IS a cost. 6e's naive ~5,400 comes down by that much,
#        and whether `deep` recovers it says if the fix is a byte count or a design change.
#   deep separably below big               -> raising tt_SEGMENT_BYTES does NOT buy the depth back, which
#        would mean the cost is the slot size itself and not the slot count. That is the outcome that
#        would most change 6e, so it is named rather than left to be noticed.
#   any arm delivers fewer samples than it sent -> report that first, before any throughput number.
#   VOID - two arms compiled to the same geometry, so the -D did not reach the compiler and "no
#        difference" would be the run agreeing with itself.
#
# Two core processes in a private netns - never the default one, where a TickLE node reaches the rig over
# the 10.1.1.x management LAN.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
NS=tt-depthns
OUT=${OUT:-$HOME/rig_results_safe/slot_depth_cost}
BIG_SLOT=${BIG_SLOT:-2816}
BIG_SEG=${BIG_SEG:-1048576}
REPS=${REPS:-3}
rm -rf "$OUT"; mkdir -p "$OUT"
say() { echo "$*" | tee -a "$OUT/log.txt"; }
say "=== slot depth cost $(date -Is) big_slot=$BIG_SLOT big_seg=$BIG_SEG ==="

cat > "$OUT/geom.c" <<'EOF'
#include <stdio.h>
#include <tickle/config.h>
int main(void) {
    printf("slots=%u slot_bytes=%u bytes=%u\n", (unsigned)tt_SEGMENT_SLOTS, (unsigned)tt_SEGMENT_SLOT_BYTES,
           (unsigned)tt_SEGMENT_BYTES);
    return 0;
}
EOF

build_arm() { # build_arm <name> [extra-cflags...]
    local name=$1
    shift
    gcc -I"$REPO/include" "$@" -o "$OUT/geom_$name" "$OUT/geom.c" 2>>"$OUT/build_$name.log" || return 1
    make -C "$REPO/platform/linux" clean >/dev/null 2>&1
    make -C "$REPO/platform/linux" perf CFLAGS="-O2 -DNDEBUG -Wall -Wextra $*" >>"$OUT/build_$name.log" 2>&1 || return 1
    cp "$REPO/platform/linux/perf_client" "$OUT/client_$name"
    cp "$REPO/platform/linux/perf_server" "$OUT/server_$name"
    say "  arm $name: $("$OUT/geom_$name")"
}

build_arm small || { say "VOID: the default build failed"; exit 1; }
build_arm big "-Dtt_SEGMENT_SLOT_BYTES=$BIG_SLOT" || { say "VOID: the big-slot build failed"; exit 1; }
build_arm deep "-Dtt_SEGMENT_SLOT_BYTES=$BIG_SLOT" "-Dtt_SEGMENT_BYTES=$BIG_SEG" || { say "VOID: the deep build failed"; exit 1; }
if [ "$("$OUT/geom_small")" = "$("$OUT/geom_big")" ]; then
    say "VOID: small and big compiled to the same geometry - the -D did not reach the compiler."
    exit 1
fi

sudo -n ip netns del "$NS" 2>/dev/null
sudo -n ip netns add "$NS" || { say "VOID: netns add failed"; exit 1; }
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -f /dev/shm/tickle-seg-192.168.10.* 2>/dev/null
    make -C "$REPO/platform/linux" clean >/dev/null 2>&1
}
trap cleanup EXIT
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" link add dummy0 type dummy
sudo -n ip -n "$NS" addr add 192.168.10.1/24 dev dummy0
sudo -n ip -n "$NS" link set dummy0 up
sudo -n ip -n "$NS" route add default dev dummy0
in_ns() { sudo -n ip netns exec "$NS" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups env HOME="$HOME" "$@"; }

run_arm() { # run_arm <name> <rep>
    rm -f /dev/shm/tickle-seg-192.168.10.* 2>/dev/null
    in_ns stdbuf -oL "$OUT/server_$1" -d 25 -I 20 >"$OUT/reader_$1_$2.log" 2>&1 &
    sleep 1
    in_ns stdbuf -oL "$OUT/client_$1" -d 15 -I 21 >"$OUT/writer_$1_$2.log" 2>&1 &
    wait
}
for rep in $(seq 1 "$REPS"); do
    for arm in small big deep; do
        say "--- $arm rep $rep/$REPS ---"
        run_arm "$arm" "$rep"
    done
done
say "=== done $(date -Is) ==="

field() { grep -oE "$2=[0-9]+" "$1" 2>/dev/null | tail -1 | cut -d= -f2; }
say ""
python3 - "$OUT" "$REPS" <<'PYEOF' | tee -a "$OUT/log.txt"
import re, sys, os, statistics as st
out, reps = sys.argv[1], int(sys.argv[2])
def grab(path, key):
    try:
        hits = re.findall(key + r"=(\d+)", open(path).read())
    except OSError:
        return None
    return int(hits[-1]) if hits else None

arms = {}
for arm in ("small", "big", "deep"):
    sent, recv, shm, full = [], [], [], []
    for rep in range(1, reps + 1):
        w, r = f"{out}/writer_{arm}_{rep}.log", f"{out}/reader_{arm}_{rep}.log"
        s_, r_ = grab(w, "sent"), grab(r, "received")
        if s_ is None or r_ is None:
            continue
        sent.append(s_); recv.append(r_)
        shm.append(grab(w, "tx_shm") or 0); full.append(grab(w, "shm_full_dropped") or 0)
    arms[arm] = (sent, recv, shm, full)

print("  arm     n  samples median   range                 tx_shm median  full_dropped  delivered")
for arm in ("small", "big", "deep"):
    sent, recv, shm, full = arms[arm]
    if not sent:
        print(f"  {arm:<7} 0  (no usable reps)"); continue
    ok = "all" if all(a == b for a, b in zip(sent, recv)) else "SHORT"
    print(f"  {arm:<7} {len(sent):<2} {st.median(sent):>14,.0f}   {min(sent):,}..{max(sent):,}"
          f"{'':>4} {st.median(shm):>13,.0f}  {st.median(full):>12,.0f}  {ok}")
print()

def ranges_overlap(a, b):
    return not (min(a) > max(b) or min(b) > max(a))

bad = [a for a in arms if arms[a][0] and any(x != y for x, y in zip(arms[a][0], arms[a][1]))]
if bad:
    print(f"LOSS FIRST: {', '.join(bad)} delivered fewer samples than sent. No throughput number from this")
    print("  run means anything until that is explained.")
elif not all(arms[a][0] for a in arms):
    print("VOID: an arm produced no usable repetition.")
else:
    s_, b_, d_ = arms["small"][0], arms["big"][0], arms["deep"][0]
    if ranges_overlap(s_, b_):
        print(f"DEPTH IS NOT SEPARABLE: small {min(s_):,}..{max(s_):,} and big {min(b_):,}..{max(b_):,} overlap.")
        print("  A slot sized for p4 costs nothing measurable in back-pressure at this rate, so 6e's p4")
        print("  prediction stands on batching alone. Judged on SAMPLES, which is the item; tx_shm counts")
        print("  datagrams and read 'within 10%' on a run where samples had fallen 17%.")
    elif st.median(b_) < st.median(s_):
        drop = (st.median(s_) - st.median(b_)) / st.median(s_) * 100
        line = f"DEPTH COSTS {drop:.1f}% OF SAMPLES: small {st.median(s_):,.0f} against big {st.median(b_):,.0f}, ranges separable."
        print(line)
        if ranges_overlap(d_, s_):
            print(f"  The deep arm recovers it ({st.median(d_):,.0f}, overlapping small), so the cost is slot COUNT and")
            print("  the fix is a byte count - ~1 MiB against iox-roudi's 216 MB - not a design change.")
        else:
            print(f"  And the deep arm does NOT recover it ({min(d_):,}..{max(d_):,}), so the cost is the slot SIZE")
            print("  itself rather than the slot count. That is the outcome that most changes 6e: a slot big")
            print("  enough for p4 is not free even with the depth restored.")
    else:
        print(f"BIG IS FASTER: {st.median(b_):,.0f} against small's {st.median(s_):,.0f}, ranges separable.")
        print("  Unexpected and worth explaining before it is used - a larger slot should not raise sample rate.")
PYEOF

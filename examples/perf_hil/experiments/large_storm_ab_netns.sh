#!/usr/bin/env bash
# large_storm_ab_netns.sh - the WB 1 MiB control (wifi_burst_loss_netns.sh, ctl arm, rel_large, offload off) for two
# trees, rep by rep in alternation, every node pinned to one CPU (PIN_CPU). 2026-10-23.
#
# Why pinned: the storm needs a reliable HEARTBEAT sent while a discovery summary is still batched in tx_buffer, which
# on an idle PC almost never happens (0 storms in 9 reps at load ~2) and under other sessions' load mostly does (8 of 9
# reps at load 5-50). Pinning the three nodes to one CPU makes them delay each other - their own contention, no load
# generator - and stormed 1 rep in 5 on the parent.
#
# PRE-REGISTERED (written before the run): per arm, the reps whose writer resent anything and the units resent. A rep
# is VOID without both RESULT lines or with any tc drop (nothing is configured to drop; a drop is the storm's own
# overflow, so it is printed beside the rep, not voided, when that rep resent first). The fix is shown when A storms
# (resends > 0) in at least 2 reps and B in none; A with fewer than 2 storming reps decides nothing (the trigger did
# not fire often enough to be told from chance) - run more reps.
#
# Usage: ARM_A=<tree dir> ARM_B=<tree dir> [REPS=6] [DUR=30] [PIN_CPU=7] [OUT=dir] large_storm_ab_netns.sh
#   Each tree dir holds src/ include/ examples/perf_hil/experiments/ examples/perf_hil/tickle/common/ (git archive).
set -uo pipefail
ARM_A=${ARM_A:?ARM_A=<tree dir>}
ARM_B=${ARM_B:?ARM_B=<tree dir>}
REPS=${REPS:-6}
DUR=${DUR:-30}
PIN_CPU=${PIN_CPU:-7}
OUT=${OUT:-$HOME/rig_results_safe/large_storm/ab_$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT" || exit 1
SUM="$OUT/summary.txt"
echo "=== large_storm_ab_netns $(date -Is) ARM_A=$ARM_A ARM_B=$ARM_B REPS=$REPS DUR=$DUR PIN_CPU=$PIN_CPU" | tee "$SUM"
for rep in $(seq 1 "$REPS"); do
    for arm in A B; do
        dir=$ARM_A
        [ "$arm" = B ] && dir=$ARM_B
        OUT="$OUT/$arm" REP_FIRST=$rep REPS=1 DUR=$DUR ARMS=ctl CELLS=rel_large NODE_ENV="TT_UDP_OFFLOAD=0 taskset -c $PIN_CPU" \
            LARGE_RSTATS=1 "$dir/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >>"$OUT/$arm.log" 2>&1
    done
done
python3 - "$OUT" <<'EOF' | tee -a "$SUM"
import pathlib, re, sys
out = pathlib.Path(sys.argv[1])
storms = {}
for arm in ("A", "B"):
    storms[arm] = 0
    for run in sorted((out / arm / "runs").glob("ctl-rel_large-*"), key=lambda p: int(p.name.rsplit("-", 1)[1])):
        ping = (run / "ping.log").read_text(errors="replace") if (run / "ping.log").exists() else ""
        echo = (run / "echo.log").read_text(errors="replace") if (run / "echo.log").exists() else ""
        counters = (run / "counters.txt").read_text(errors="replace") if (run / "counters.txt").exists() else ""
        load = (run / "meta.txt").read_text().split()[0] if (run / "meta.txt").exists() else "load=?"
        pub = re.search(r"^RESULT: role=pub .*?retransmitted=(\d+)", ping, re.M)
        sub = re.search(r"^RESULT: role=sub .*?delivered=(\d+).*?latency_median_us=([\d.]+) latency_p99_us=([\d.]+)", echo, re.M)
        drops = sum(int(d) for d in re.findall(r"dropped (\d+)", counters))
        timer = re.search(r"role=sub acknack_tx immediate=(\d+) timer=(\d+)", echo)
        if not pub or not sub:
            print(f"VOID  {arm} {run.name} {load}: no RESULT line")
            continue
        resent = int(pub.group(1))
        if drops and not resent:
            print(f"VOID  {arm} {run.name} {load}: {drops} tc drops with nothing resent")
            continue
        storms[arm] += resent > 0
        print(f"{arm} {run.name} {load} resent={resent} delivered={sub.group(1)} p50_us={sub.group(2)} p99_us={sub.group(3)} "
              f"drops={drops} acknack_immediate={timer.group(1) if timer else '?'} timer={timer.group(2) if timer else '?'}")
print(f"storming reps: A {storms['A']}, B {storms['B']}")
if storms["A"] < 2:
    print("READING: UNDECIDED - A stormed in fewer than 2 reps; the trigger did not fire often enough")
elif storms["B"] == 0:
    print("READING: FIX SHOWN - A stormed, B never did")
else:
    print("READING: NOT FIXED - B stormed too")
EOF
echo "=== large_storm_ab_netns ended $(date -Is)" | tee -a "$SUM"

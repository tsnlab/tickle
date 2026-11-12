#!/usr/bin/env bash
# batch_stage0.sh - stage 0 of the small-sample batching candidate: what does perf_client -B (batching) give on the
# rig for small samples, and at what latency cost? Measurement only: no library change, no wire change, no default
# change. Written 2026-11-12; the rule is registered in ~/claude_reports/SMALL_SAMPLE_BATCHING_STAGE0_2026-11-12.md
# and implemented in batch_stage0_read.py (a criterion no code enforces is a comment).
#
# Cells: BEST_EFFORT, CDR 16 / 100 / 1000 B (perf_client -s 10 / 94 / 994: CDR = 4 seq + 2 count + payload) x flood
# (-i 0), 10 kHz (-i 0.0001), 1 kHz (-i 0.001). Arms: A = no -B, B = -B, A2 = A again (noise control). REPS reps, the
# arms rotated per rep, the 9 cells inside a rep. perf_client on 10.1.1.214, perf_server on 10.1.1.213, over eth0.
# Why not s6_transport_cells.sh: it drives the perf_hil bench client (no -B, no -s), and its arms are framework builds
# on one host.
#
# Usage: [SHA=<rev>] [REPS=5] [DUR=20] [CELLS="s16_flood ..."] [OUT=dir] batch_stage0.sh
#   Takes the rig lock itself (rig_lock.sh, hil scope). Launch detached; the log ends "=== batch_stage0 ended rc=".
#   Per run it writes $OUT/<cell>_r<rep>_<arm>.txt, which batch_stage0_read.py reads.
set -uo pipefail
# shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$(dirname "${BASH_SOURCE[0]}")/pi_checkout.sh"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
export RIG_LOCK_WAIT="${RIG_LOCK_WAIT:-36000}"
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi

SHA=$(pi_full_sha "${SHA:-origin/main}") || exit 1
REPS=${REPS:-5}
DUR=${DUR:-20}
OUT=${OUT:-$HOME/rig_results_safe/batch_stage0_$(date +%Y%m%d)}
K=$HOME/.ssh/tickle_ci_ed25519
CLI=10.1.1.214
SRV=10.1.1.213
BCAST=192.168.10.255
SAVE=/home/ci/batch_stage0 # binaries outside ~/tickle, so a later checkout cannot replace them mid-run
ALL_CELLS="s16_flood s16_10k s16_1k s100_flood s100_10k s100_1k s1000_flood s1000_10k s1000_1k"
CELLS=${CELLS:-$ALL_CELLS}
T0=$(date +%s)
mkdir -p "$OUT" || exit 1
LOG="$OUT/run.log"
say() { echo "$*" | tee -a "$LOG"; }
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }

cell_args() { # cell -> "<-s payload> <-i interval>"; CDR = payload + 6
    case "$1" in
    s16_*) s=10 ;; s100_*) s=94 ;; s1000_*) s=994 ;;
    esac
    case "$1" in
    *_flood) i=0 ;; *_10k) i=0.0001 ;; *_1k) i=0.001 ;;
    esac
    echo "-s $s -i $i"
}

# A bench process still alive on either Pi (by /proc/<pid>/exe, never by name pattern).
leftover() {
    for h in $CLI $SRV; do
        sh_ "$h" "for e in /proc/[0-9]*/exe; do x=\$(readlink \$e 2>/dev/null); case \"\$x\" in $SAVE/*) echo \"$h:\$x\" ;; esac; done" </dev/null 2>/dev/null
    done | tr '\n' ' '
}
stop_server() { # signals only a pid whose exe is the saved perf_server
    sh_ "$SRV" "p=\$(cat /tmp/bs0_srv.pid 2>/dev/null) || exit 0
case \"\$(readlink /proc/\$p/exe 2>/dev/null)\" in $SAVE/perf_server) kill -INT \$p; for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -d /proc/\$p ] || break; sleep 0.5; done
 case \"\$(readlink /proc/\$p/exe 2>/dev/null)\" in $SAVE/perf_server) kill -KILL \$p ;; esac ;; esac; true" </dev/null >/dev/null 2>&1
}
on_exit() {
    local rc=$?
    stop_server
    say "=== batch_stage0 ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

say "=== batch_stage0 $(date -Is): sha=$SHA REPS=$REPS DUR=$DUR CELLS='$CELLS' harness=$(git -C "$REPO" rev-parse --short HEAD) ==="
say "    lock: $(cat /tmp/tickle-hil.lock 2>/dev/null)"

# ---------------------------------------------------------------------------------------------------------- build
build() { # build <host> <paced|none>
    sh_ "$1" "set -e
cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log
cd platform/linux && make -j4 BUILD_TYPE=release perf_client perf_server >/tmp/bs0_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/bs0_build.log; exit 1; }
rm -rf $SAVE && mkdir -p $SAVE && cp perf_client perf_server $SAVE/
cat > $SAVE/batch_paced_client.c <<'PACED_EOF'
$(cat "$REPO/examples/perf_hil/experiments/batch_paced_client.c")
PACED_EOF
if [ "$2" = paced ]; then
  cd ~/tickle/platform/linux
  cc -I../../include -I../../src -I../../examples/linux/perf -I../../examples/perf -O2 -DNDEBUG -Wall -Wextra -Werror -c -o $SAVE/paced.o $SAVE/batch_paced_client.c
  cc -O2 -DNDEBUG -L. -o $SAVE/batch_paced_client $SAVE/paced.o obj/release/examples/perf/Bulk.o obj/release/examples/linux/common/cli_opts.o -ltickle -lm
fi
echo BUILT \$(hostname) \$(git -C ~/tickle rev-parse --short HEAD) client=\$(sha256sum $SAVE/perf_client | cut -c1-16) server=\$(sha256sum $SAVE/perf_server | cut -c1-16) \$(grep -c -e '-O2' /tmp/bs0_build.log | sed 's/^/O2_lines=/')" </dev/null 2>&1
}
build $CLI paced >"$OUT/build_cli.txt" &
pc=$!
build $SRV none >"$OUT/build_srv.txt" &
ps_=$!
wait $pc
wait $ps_
sed 's/^/    /' "$OUT/build_cli.txt" "$OUT/build_srv.txt" | tee -a "$LOG"
grep -q BUILD_FAILED "$OUT/build_cli.txt" "$OUT/build_srv.txt" && { say "FATAL: build failed"; exit 1; }
[ "$(cat "$OUT/build_cli.txt" "$OUT/build_srv.txt" | grep -c '^BUILT')" = 2 ] || { say "FATAL: not both Pis BUILT"; exit 1; }
for h in $CLI $SRV; do
    say "PROBE $h: $(sh_ $h "echo host=\$(hostname) \$(timedatectl show -p NTPSynchronized) clk_tck=\$(getconf CLK_TCK) eth0=\$(cat /sys/class/net/eth0/operstate) speed=\$(cat /sys/class/net/eth0/speed)" </dev/null 2>&1)"
done
lo=$(leftover)
[ -z "$lo" ] || { say "FATAL: leftover bench process: $lo"; exit 1; }
say "    built in $(($(date +%s) - T0)) s"

# ---------------------------------------------------------------------------------------------------------- one run
run_one() { # cell arm rep
    local cell=$1 arm=$2 rep=$3 args extra="" bin f="$OUT/${1}_r${3}_${2}.txt"
    args=$(cell_args "$cell")
    [ "$arm" = B ] && extra="-B"
    # perf_client for the flood cells; the paced cells use batch_paced_client (perf_client's -i does not pace: see its
    # header). CLIENT=perf_client forces perf_client everywhere (the reference runs that show the tool's own pacing).
    bin=perf_client
    case "$cell" in *_flood) ;; *) [ "${CLIENT:-}" = perf_client ] || bin=batch_paced_client ;; esac
    {
        echo "META cell=$cell arm=$arm rep=$rep bin=$bin args='$args $extra -d $DUR' sha=$SHA start=$(date -Is)"
    } >"$f"
    sh_ "$SRV" "rm -f /tmp/bs0_srv.pid /tmp/bs0_srv.log
cd $SAVE
nohup sh -c 'echo \$\$ > /tmp/bs0_srv.pid; exec ./perf_server -b $BCAST' >/tmp/bs0_srv.log 2>&1 </dev/null &
sleep 0.3; test -s /tmp/bs0_srv.pid" </dev/null >/dev/null 2>&1 || { echo "VOID server did not start" >>"$f"; return 1; }
    sleep 3 # the server's first announce reaches the client before it publishes
    sh_ "$CLI" "cd $SAVE
ifc() { echo \"\$1 \$(cat /sys/class/net/eth0/statistics/tx_packets) \$(cat /sys/class/net/eth0/statistics/tx_bytes) \$(cat /sys/class/net/eth0/statistics/rx_packets) \$(cat /sys/class/net/eth0/statistics/rx_bytes)\"; }
ifc IFC_CLI_BEFORE
bash -c 'TIMEFORMAT=\"CLI_TIME real=%R user=%U sys=%S\"; time ./$bin -b $BCAST $args $extra -d $DUR' >/tmp/bs0_cli.log 2>/tmp/bs0_cli.time
echo CLI_RC=\$?
ifc IFC_CLI_AFTER
cat /tmp/bs0_cli.time
grep -E '^RESULT:|messages sent' /tmp/bs0_cli.log | sed 's/^/CLI_/'" </dev/null >>"$f" 2>&1
    sleep 1
    sh_ "$SRV" "ifc() { echo \"\$1 \$(cat /sys/class/net/eth0/statistics/tx_packets) \$(cat /sys/class/net/eth0/statistics/tx_bytes) \$(cat /sys/class/net/eth0/statistics/rx_packets) \$(cat /sys/class/net/eth0/statistics/rx_bytes)\"; }
p=\$(cat /tmp/bs0_srv.pid)
case \"\$(readlink /proc/\$p/exe 2>/dev/null)\" in $SAVE/perf_server) echo SRV_PROC \$(sed 's/.*) //' /proc/\$p/stat | awk '{print \"utime_ticks=\"\$12\" stime_ticks=\"\$13}') ;; *) echo SRV_PROC gone ;; esac
ifc IFC_SRV_AFTER" </dev/null >>"$f" 2>&1
    stop_server
    sh_ "$SRV" "grep -E '^RESULT:|one-way latency|messages received' /tmp/bs0_srv.log | sed 's/^/SRV_/'" </dev/null >>"$f" 2>&1
    echo "END $(date -Is)" >>"$f"
    grep -q '^CLI_RESULT:' "$f" && grep -q '^SRV_RESULT:' "$f"
}

# ---------------------------------------------------------------------------------------------------------- the loop
ARMS=(A B A2)
for rep in $(seq 1 "$REPS"); do
    rot=$(((rep - 1) % 3))
    order=("${ARMS[@]:rot}" "${ARMS[@]:0:rot}")
    say "--- rep $rep/$REPS arm order ${order[*]} ---"
    for cell in $CELLS; do
        for arm in "${order[@]}"; do
            if run_one "$cell" "$arm" "$rep"; then
                say "  $cell r$rep $arm ok $(grep -h '^CLI_RESULT' "$OUT/${cell}_r${rep}_${arm}.txt" | cut -c1-110)"
            else
                say "  $cell r$rep $arm NO RESULT (see $OUT/${cell}_r${rep}_${arm}.txt)"
            fi
            lo=$(leftover)
            [ -z "$lo" ] || { say "FATAL: leftover after $cell r$rep $arm: $lo"; exit 1; }
        done
    done
done
say "=== all runs done in $(($(date +%s) - T0)) s ==="
python3 -I "$REPO/examples/perf_hil/experiments/batch_stage0_read.py" "$OUT" | tee "$OUT/verdict.txt" | tee -a "$LOG"

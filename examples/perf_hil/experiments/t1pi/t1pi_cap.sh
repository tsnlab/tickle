#!/usr/bin/env bash
# T1-Pi capacity curve (WIFI_DIAG_PLAN.md section 2): plain UDP, no TickLE. Runs on the PC, drives both Pis over ssh. SIMULATION ONLY.
#   MODE=grid   a1: per (arm, rep) one topology, CELLS in random order (seeded), 5 s each
#   MODE=fresh  a2: per (arm, rep, cell) a fresh topology, one cell
#   MODE=ctrl   a3: per rep one topology of the ctl arm with VARIANT=wire (sender in the AP namespace: no wmediumd, no hwsim) or VARIANT=fix54 (STA rate set to 54M)
#   OUT=$HOME/rig_results_safe/wifi_diag_20261117/cap  ARMS="ctl o60 o200" REPS=5 DUR=5
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../../../.." && pwd)
if [ -z "${T1PI_LOCKED:-}" ]; then
    T1PI_LOCKED=1 RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-900} exec flock /tmp/rig.lock "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
SET=cap
MODE=${MODE:-grid}
VARIANT=${VARIANT:-sta}
OUT=${OUT:-$HOME/rig_results_safe/wifi_diag_20261117/cap_$MODE${VARIANT:+_$VARIANT}}
mkdir -p "$OUT"
. "$S/t1pi_lib.sh"
ARMS=${ARMS:-"ctl o60 o200"}
REPS=${REPS:-5}
DUR=${DUR:-5}
SEEDSTR=${SEEDSTR:-wdiag1}
RD='$HOME/t1pi_run/cap'
GRIDCELLS() { # B R SIZE per line
    local b r
    for b in 1 4 16 64; do for r in 20 40 80 120 200 400 800; do echo "$b $r 1472"; done; done
    echo "91 23 1472"; echo "91 91 1472"; echo "91 182 1472"
    for sz in 100 700; do for b in 1 16; do for r in 80 400; do echo "$b $r $sz"; done; done; done
}
FRESHCELLS() { echo "16 400 1472"; echo "91 91 1472"; echo "1 200 1472"; }

push_tools() {
    for h in cli srv; do
        $h "mkdir -p $RD"
    done
    cli_in "cat > $RD/udpcap.py" <"$S/t1pi_udpcap.py"
    srv_in "cat > $RD/udpcap.py" <"$S/t1pi_udpcap.py"
    cli_in "cat > $RD/sampler.py" <"$S/t1pi_sampler.py"
}

start_sampler() { # name
    cli "rm -f $RD/stop; setsid nohup sudo -n timeout -k 5 3000 python3 $RD/sampler.py $RD/$1.rc $RD/stop >/dev/null 2>&1 </dev/null &"
}
stop_sampler() { # name destfile
    cli "touch $RD/stop"
    sleep 2.5
    cli "sudo -n cat $RD/$1.rc" >"$2"
}

# run_cell <outfile> <cellid> <B> <R> <SIZE>
run_cell() {
    local f=$1 id=$2 b=$3 r=$4 sz=$5 src=$6 nsn=t1p_sta
    [ "$VARIANT" = wire ] && nsn=t1p_ap
    srv "sudo -n ip netns exec t1p_srv timeout -k 3 $((DUR + 10)) python3 $RD/udpcap.py rx --bind $IP_FAR --cell $id --burst $b --rxdur $((DUR + 3))" >"$f.rx" 2>&1 &
    local rxp=$!
    sleep 0.8
    cli "sudo -n ip netns exec $nsn timeout -k 3 $((DUR + 10)) python3 $RD/udpcap.py tx --bind $src --dst $IP_FAR --cell $id --size $sz --burst $b --rate $r --dur $DUR" >"$f.tx" 2>&1
    wait $rxp
    printf '{"cell":%s,"burst":%s,"rate":%s,"size":%s,"tx":%s,"rx":%s}\n' "$id" "$b" "$r" "$sz" "$(tail -1 "$f.tx" | grep '^{' || echo null)" "$(tail -1 "$f.rx" | grep '^{' || echo null)" >>"$f.jsonl"
    rm -f "$f.tx" "$f.rx"
}

IP_FAR=10.90.0.3
trap session_end EXIT
session_start
log "T1PICAP_START mode=$MODE variant=$VARIANT arms='$ARMS' reps=$REPS dur=$DUR"
for rep in $(seq 1 "$REPS"); do
    for arm in $ARMS; do
        case $MODE in
        grid) cells=$(GRIDCELLS | python3 -c "import sys,random;L=sys.stdin.read().split('\n')[:-1];random.Random('$SEEDSTR$arm$rep').shuffle(L);print('\n'.join(L[:int('${LIMIT:-1000}')]))") ;;
        ctrl) cells=$(FRESHCELLS) ;;
        fresh) cells=$(FRESHCELLS) ;;
        esac
        if [ $MODE = fresh ]; then groups=3; else groups=1; fi
        for g in $(seq 1 $groups); do
            run=$OUT/${arm}_rep${rep}_g$g
            bring_up "$run" "$arm" || { log "HARNESS_FAULT $run no topology"; continue; }
            src=10.90.0.2
            if [ "$VARIANT" = wire ]; then
                cli "sudo -n ip -n t1p_ap addr add 10.90.0.4/24 dev br0"; src=10.90.0.4
            fi
            if [ "$VARIANT" = fix54 ]; then
                cli "sudo -n ip netns exec t1p_sta iw dev hwsta set bitrates legacy-2.4 54 2>&1; sudo -n ip netns exec t1p_ap iw dev hwap set bitrates legacy-2.4 54 2>&1; true" >"$run.fix54.txt" 2>&1
            fi
            srv "sudo -n ip -n t1p_srv link set lo up"
            cli "sudo -n ip -n t1p_sta link set lo up"
            push_tools
            start_sampler "$arm"
            sleep 2
            if [ $MODE = fresh ]; then clist=$(echo "$cells" | sed -n "${g}p"); else clist="$cells"; fi
            echo "$clist" | while read -r b r sz; do
                [ -z "$b" ] && continue
                run_cell "$run.c" "$((RANDOM + 1))" "$b" "$r" "$sz" "$src"
                sleep 1.5
            done
            stop_sampler "$arm" "$run.rc"
            down_both
            check_state "$arm/rep$rep/g$g" || { log "ABORT: wlan0/eth0/modules state changed"; exit 2; }
            mv "$run.c.jsonl" "$run.jsonl" 2>/dev/null
            log "done $arm rep$rep g$g"
        done
    done
done
log "T1PICAP_END"

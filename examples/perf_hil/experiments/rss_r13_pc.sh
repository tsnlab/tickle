#!/usr/bin/env bash
# rss_r13_pc.sh - docs/RESULTS.md rmw row R13 (peak RSS, tput Array1k RELIABLE KEEP_ALL depth 1000, the larger of
# perf_test's publisher and subscriber ru_maxrss) on the PC, per build, so a change in it can be bisected over commits.
#
# Each build is a pc_preflight_build.sh-shaped directory (install/ + perf/install/). Every rep runs every build and arm
# once, build order rotated per rep, through rmw_samehost_cell.sh in ONE private netns with a private /dev/shm per
# cell (the preflight's shape). Arms: tickle, and cyclonedds as the control - rmw_tickle's code cannot touch its
# memory, but the perf_test binary and the generated interface libraries of each build can, so a delta in the
# control is the harness's share and the tickle delta minus it is rmw_tickle's.
#
# Reading (rss_r13_pc.py): per build and arm, n runs and the median of sub ru_maxrss (R13's value), sub VmHWM before the
# publisher starts (idle: creation and discovery only) and the sampler's largest sub VmHWM. A run whose perf_test
# delivered nothing, or that has no ru_maxrss row, is VOID and not counted. --by-held splits each build's runs by the
# subscriber's reorder_held_peak: a run that declined samples and held the ones behind them touched part of the
# RELIABLE subscription's reorder slots (1024 x 1720 B on the PC), ~0.6-1.2 MB resident by the number of such
# episodes, and that, not the build, decides most of R13's run-to-run spread on the PC (2026-10-21: held runs
# 16,968-17,996 kB, unheld 16,488-16,884).
#
# Instrument: perf_test's ru_maxrss on its last row is max(the hiwater mark at the last munmap, the RSS then), not the
# run's peak; the sampler's VmHWM (same rule) reads up to ~500 kB higher mid-run.
#
# Usage: BUILDS="label=dir ..." REPS=3 DUR=10 ARMS="tickle cyclonedds" OUT=<dir> rss_r13_pc.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CELL=$HERE/rmw_samehost_cell.sh
BUILDS=${BUILDS:?label=dir ...}
REPS=${REPS:-3}
DUR=${DUR:-10}
ARMS=${ARMS:-"tickle cyclonedds"}
OUT=${OUT:?results directory}
DOMAIN=${DOMAIN:-83}
mkdir -p "$OUT" || exit 1
# A build may carry one rmw_tickle environment variable as label=dir@VAR=value (a treatment, e.g.
# RMW_TICKLE_EXECUTOR_HOLDOVER=0). The cell scrubs RMW_TICKLE_*, so a copy of the cell exports EXTRA_ENV after its
# scrub and records it as extra_env= in meta.txt.
case "$BUILDS" in *@*)
    awk '{print} /^\*\) meta "fatal=unknown_arm"; exit 1 ;;$/ {print "esac"; print "[ -n \"${EXTRA_ENV:-}\" ] && export \"${EXTRA_ENV?}\" && meta \"extra_env=$EXTRA_ENV\""; print "case x in x) ;;"}' \
        "$CELL" >"$OUT/cell_extra.sh" || exit 1
    # shellcheck disable=SC2016 # the literal line the copy must contain
    grep -q '^\[ -n "${EXTRA_ENV' "$OUT/cell_extra.sh" || { echo "cell copy has no EXTRA_ENV export"; exit 1; }
    CELL=$OUT/cell_extra.sh
    ;;
esac
ns=rss13_$$
cleanup() { sudo -n ip netns del "$ns" 2>/dev/null; echo "RSS_R13 END $(date -Is)"; }
trap cleanup EXIT
sudo -n ip netns add "$ns" || { echo "cannot create netns $ns"; exit 1; }
sudo -n ip -n "$ns" link set lo up multicast on
sudo -n ip -n "$ns" route add default dev lo
read -r -a blist <<<"$BUILDS"
nb=${#blist[@]}
for rep in $(seq 1 "$REPS"); do
    for i in $(seq 0 $((nb - 1))); do
        b=${blist[$(((i + rep - 1) % nb))]}
        label=${b%%=*} dir=${b#*=} extra=""
        case "$dir" in *@*) extra=${dir#*@} dir=${dir%%@*} ;; esac
        for a in $ARMS; do
            cd_=$OUT/${label}_${a}_r$rep
            sudo -n ip netns exec "$ns" env -i PATH="$PATH" HOME="$HOME" ARM="$a" KIND=tput MSG=Array1k QOS=reliable \
                WAIT=block DUR="$DUR" CELL_DIR="$cd_" DOMAIN="$DOMAIN" ROS_SETUP=/opt/ros/lyrical/setup.bash \
                OVERLAYS="$dir/install/local_setup.bash $dir/perf/install/local_setup.bash" TICKLE_BCAST=127.255.255.255 \
                PERF_TEST="$dir/perf/install/performance_test/lib/performance_test/perf_test" EXTRA_ENV="$extra" \
                OWNER="$(id -u):$(id -g)" bash -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm && bash "$0"; rc=$?; chown -R "$OWNER" "$CELL_DIR"; exit $rc' "$CELL"
            echo "$(date +%T) rep $rep $label $a: cell exit $?"
        done
    done
done

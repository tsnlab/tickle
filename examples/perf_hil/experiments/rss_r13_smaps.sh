#!/usr/bin/env bash
# rss_r13_smaps.sh - one R13 cell (tput Array1k RELIABLE, rmw_tickle) per build, with the subscriber's and publisher's
# /proc/PID/smaps saved where their Rss was largest (polled every 0.25 s while they run), so a change in R13 can be
# put down to mappings: the rmw library's text, heap, the shared-memory segment, thread stacks.
# Usage: BUILDS="label=dir ..." DUR=10 OUT=<dir> rss_r13_smaps.sh   (results: <OUT>/<label>/{sub,pub}.smaps.max)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CELL=${CELL:-$HERE/rmw_samehost_cell.sh} # with label=dir@VAR=value builds: CELL=<rss_r13_pc.sh's cell_extra.sh>
BUILDS=${BUILDS:?label=dir ...}
DUR=${DUR:-10}
OUT=${OUT:?results directory}
ARM=${ARM:-tickle}
mkdir -p "$OUT" || exit 1
ns=rss13s_$$
cleanup() { sudo -n ip netns del "$ns" 2>/dev/null; echo "RSS_R13_SMAPS END $(date -Is)"; }
trap cleanup EXIT
sudo -n ip netns add "$ns" || { echo "cannot create netns $ns"; exit 1; }
sudo -n ip -n "$ns" link set lo up multicast on
sudo -n ip -n "$ns" route add default dev lo
# The watcher runs as root beside the cell (a root process's smaps is not readable by this user).
# shellcheck disable=SC2016 # expanded by the inner shell
WATCH='w() { local role=$1 pid best=0 r; while [ ! -s "$CELL_DIR/$role.pid" ]; do [ -f "$CELL_DIR/sampler.stop" ] && return; sleep 0.05; done
  pid=$(cat "$CELL_DIR/$role.pid"); sleep 0.3
  while [ -d /proc/$pid ]; do r=$(awk "/^Rss:/ {print \$2}" /proc/$pid/smaps_rollup 2>/dev/null); r=${r:-0}
    if [ "$r" -gt "$best" ]; then cat /proc/$pid/smaps > "$CELL_DIR/$role.smaps.tmp" 2>/dev/null && mv "$CELL_DIR/$role.smaps.tmp" "$CELL_DIR/$role.smaps.max"; best=$r; fi
    [ -f "$CELL_DIR/$role.smaps.idle" ] || cp "$CELL_DIR/$role.smaps.max" "$CELL_DIR/$role.smaps.idle" 2>/dev/null
    sleep 0.25; done; }'
for rep in ${REPS_LIST:-1}; do # builds interleaved within each rep
    for b in $BUILDS; do
        label=${b%%=*} dir=${b#*=} extra=""
        case "$dir" in *@*) extra=${dir#*@} dir=${dir%%@*} ;; esac
        cd_=$OUT/${label}_r$rep
        sudo -n ip netns exec "$ns" env -i PATH="$PATH" HOME="$HOME" ARM="$ARM" KIND=tput MSG=Array1k QOS=reliable \
            WAIT=block DUR="$DUR" CELL_DIR="$cd_" DOMAIN=84 ROS_SETUP=/opt/ros/lyrical/setup.bash \
            OVERLAYS="$dir/install/local_setup.bash $dir/perf/install/local_setup.bash" TICKLE_BCAST=127.255.255.255 \
            PERF_TEST="$dir/perf/install/performance_test/lib/performance_test/perf_test" OWNER="$(id -u):$(id -g)" \
            WATCH="$WATCH" EXTRA_ENV="$extra" \
            bash -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm || exit 1; eval "$WATCH"
                (sleep 0.5; w sub) & ws=$!; (sleep 0.5; w pub) & wp=$!
                bash "$0"; rc=$?; wait $ws $wp; chown -R "$OWNER" "$CELL_DIR"; exit $rc' "$CELL"
        echo "$(date +%T) $label r$rep: cell exit $?"
    done
done

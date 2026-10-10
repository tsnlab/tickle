#!/usr/bin/env bash
# stepA_perf_pc.sh - stepA_perf_rig.sh's cells and modes on THIS PC, in private network namespaces with private
# /dev/shm, written in the rig harness's file layout so stepA_perf_read.py reads both (and this is its fixture).
# 2026-10-10. PC only: no rig, no lock. Needs passwordless `sudo -n ip`.
#
#   x_p2, x_p3  reliable_throughput -Q -N <405|368> -B 100 -d $DUR --warmup-s 1 --cooldown-s 1, two namespaces joined by
#               a veth, each with its own /dev/shm (a tmpfs mounted inside `ip netns exec`, so the two cannot find each
#               other's segment): the cross-host path. Client (publisher) on CPU_C, server on CPU_S.
#   s_p2        reliable_latency -Q -i 0.005 -W 4096 -C 4096 -I 0.001, both roles in one namespace sharing one private
#               /dev/shm, BENCH_IFACE=lo, a dummy link carrying the bench's 192.168.10.255: the same-host segment path.
#   modes plain / stat (perf stat, + ld_blocks_partial.address_alias and ld_blocks.store_forward when this CPU has
#   them) / record (perf record -g -e cycles) / strace (strace -f -c), blocks A B B A, REPS per block.
# The reading is stepA_perf_read.py's (its header). On this PC the timing metrics are noisy - other sessions load it -
# so its counts (instructions, system calls) are what it is for; cycles are read with the A/A floor like anything else.
#
# Usage: A=<commit|@dir> B=<commit|@dir> [REPS=2] [DUR=3] [DUR_S=3] [CPU_C=12] [CPU_S=13] [OUT=prefix] stepA_perf_pc.sh
#   OUT defaults to ~/rig_results_safe/stepA_perf_pc_<stamp> ($OUT.txt, $OUT.d/). Launch detached; ends "=== ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
A=${A:?A=<commit or @dir>}
B=${B:?B=<commit or @dir>}
REPS=${REPS:-2}
DUR=${DUR:-3}
DUR_S=${DUR_S:-3}
CPU_C=${CPU_C:-12}
CPU_S=${CPU_S:-13}
FREQ=${FREQ:-1999}
OUT=${OUT:-$HOME/rig_results_safe/stepA_perf_pc_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
WORK=$(mktemp -d /tmp/stepA_perf_pc.XXXXXX) || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
on_exit() {
    local rc=$? ns
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== stepA_perf_pc ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
say "=== stepA_perf_pc $(date -Is): A=$A B=$B REPS=$REPS DUR=$DUR DUR_S=$DUR_S harness=$(git -C "$REPO" rev-parse --short HEAD) ==="

EVENTS="instructions:u,instructions:k,cycles:u,cycles:k,context-switches,cpu-migrations,page-faults,raw_syscalls:sys_enter"
# Read whole first: `perf list | grep -q` under pipefail fails when grep closes the pipe early (perf gets SIGPIPE), and
# the event was silently left out of the first run.
PERF_LIST=$(perf list 2>/dev/null)
for ev in ld_blocks_partial.address_alias ld_blocks.store_forward; do
    grep -qw "$ev" <<<"$PERF_LIST" && EVENTS="$EVENTS,$ev"
done
say "    perf events: $EVENTS"

# ------------------------------------------------------------------------------------------------ builds
build_arm() { # $1 label, $2 commit or @dir
    local label=$1 src=$2 tree="$WORK/$1/src" log="$OUT.d/build_$1.log" v
    mkdir -p "$tree" "$WORK/$label/home"
    if [ "${src:0:1}" = @ ]; then
        (cd "${src:1}" && git ls-files -z | tar --null -T - -cf -) | tar -x -C "$tree"
        echo "source: tree ${src:1} at $(git -C "${src:1}" rev-parse HEAD) + diff $(git -C "${src:1}" diff HEAD | sha256sum | cut -c1-12)" >"$log"
    else
        git -C "$REPO" archive "$src" | tar -x -C "$tree"
        echo "source: commit $(git -C "$REPO" rev-parse "$src")" >"$log"
    fi
    for v in reliable_throughput_p2 reliable_throughput_p3 reliable_latency_p2; do
        (cd "$tree/examples/perf_hil/tickle" && env -u TICKLE_EXTRA_CFLAGS HOME="$WORK/$label/home" ./build.sh "${v%_p[0-9]}" "${v##*_}") \
            >>"$log" 2>&1 || { echo "BUILD_FAILED $v" >>"$log"; return 1; }
        mkdir -p "$WORK/bin/$label/$v"
        cp "$tree/examples/perf_hil/tickle/$v/client" "$tree/examples/perf_hil/tickle/$v/server" "$WORK/bin/$label/$v/"
        echo "BUILT $label $v $src client=$(sha256sum "$WORK/bin/$label/$v/client" | cut -c1-16)" >>"$OUT.d/build_${label}_cli.txt"
    done
}
build_arm A "$A" &
pa=$!
build_arm B "$B" &
pb=$!
wait $pa
ra=$?
wait $pb
rb=$?
[ $ra = 0 ] && [ $rb = 0 ] || { say "FATAL: a build failed - $OUT.d/build_*.log"; exit 1; }
cat "$OUT.d/build_A_cli.txt" "$OUT.d/build_B_cli.txt" | sed 's/^/    /' | tee -a "$SUM"
say "    built in $(($(date +%s) - T0)) s"

# ------------------------------------------------------------------------------------------------ runs
prefix_for() { # $1 mode, $2 output file prefix (for one role)
    case "$1" in
    plain) echo "" ;;
    stat) echo "perf stat -x, -o $2.perf -e $EVENTS --" ;;
    record) echo "perf record -q -g -e cycles -F $FREQ -o $2.data --" ;;
    strace) echo "strace -f -c -o $2.strace" ;;
    esac
}
render_report() { # $1 perf.data, $2 report file (the rig harness's command, rendered as this user)
    [ -f "$1" ] || return 0
    {
        perf report -i "$1" --no-children -g none -q -t ';' -F period,sample,dso,sym --stdio 2>/dev/null
        echo "# Samples: $(perf report -i "$1" --stdio 2>/dev/null | grep -m1 '^# Samples' | sed 's/.*Samples: *//')"
    } >"$2"
    rm -f "$1"
}
NSID=0
run_one() { # $1 block, $2 arm, $3 cell, $4 mode, $5 rep
    local blk=$1 arm=$2 cell=$3 mode=$4 rep=$5 f v d
    f="$OUT.d/b${blk}_${arm}_${cell}_${mode}_r${rep}"
    NSID=$((NSID + 1))
    local na="spc$$-${NSID}a" nb="spc$$-${NSID}b" va="spc$$v${NSID}a" vb="spc$$v${NSID}b"
    echo "$na" >>"$NS_LIST"
    case "$cell" in
    x_*)
        v=reliable_throughput_${cell#x_}
        local n
        n=$([ "$cell" = x_p2 ] && echo 405 || echo 368)
        local args="-Q -N $n -B 100 -d $DUR --warmup-s 1 --cooldown-s 1"
        echo "$nb" >>"$NS_LIST"
        if ! { sudo -n ip netns add "$na" && sudo -n ip netns add "$nb" && sudo -n ip link add "$va" type veth peer name "$vb" &&
            sudo -n ip link set "$va" netns "$na" && sudo -n ip link set "$vb" netns "$nb" &&
            sudo -n ip -n "$na" addr add 192.168.10.1/24 brd + dev "$va" && sudo -n ip -n "$nb" addr add 192.168.10.2/24 brd + dev "$vb" &&
            sudo -n ip -n "$na" link set lo up && sudo -n ip -n "$nb" link set lo up &&
            sudo -n ip -n "$na" link set "$va" up && sudo -n ip -n "$nb" link set "$vb" up; }; then
            echo "no veth" >"$f.note"
            return 0
        fi
        d="$WORK/bin/$arm/$v"
        # shellcheck disable=SC2024 # the logs are this user's, written by this shell
        sudo -n ip netns exec "$nb" sh -c "mount -t tmpfs -o size=64m spcshm /dev/shm || exit 9
cd $d && exec env BENCH_IFACE=$vb taskset -c $CPU_S ./server $args" >"$f.server.log" 2>&1 &
        local sp=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$na" sh -c "mount -t tmpfs -o size=64m spcshm /dev/shm || exit 9
cd $d && env BENCH_IFACE=$va taskset -c $CPU_C $(prefix_for "$mode" "$WORK/c") ./client $args; echo client_rc=\$?
chown -R $(id -u):$(id -g) $WORK" >"$f.client.log" 2>&1
        sleep 1
        local p
        for p in $(sudo -n ip netns pids "$nb"); do
            # The server runs as root: its /proc/<pid>/exe is read through the same `ip netns exec` that may signal it.
            [ "$(sudo -n ip netns exec "$nb" readlink "/proc/$p/exe" 2>/dev/null)" = "$d/server" ] &&
                sudo -n ip netns exec "$nb" kill -INT "$p"
        done
        wait $sp
        sudo -n ip netns del "$na"
        sudo -n ip netns del "$nb"
        ;;
    s_*)
        v=reliable_latency_${cell#s_}
        d="$WORK/bin/$arm/$v"
        if ! { sudo -n ip netns add "$na" && sudo -n ip netns exec "$na" ip link set lo up &&
            sudo -n ip netns exec "$na" ip route add default dev lo && sudo -n ip netns exec "$na" ip link add bench0 type dummy &&
            sudo -n ip netns exec "$na" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0 &&
            sudo -n ip netns exec "$na" ip link set bench0 up; }; then
            echo "no netns" >"$f.note"
            return 0
        fi
        # The server is the launching shell's own child ($!), stopped by that pid once the client is done.
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$na" sh -c "mount -t tmpfs -o size=256m spcshm /dev/shm || exit 9
cd $d
env BENCH_IFACE=lo taskset -c $CPU_S $(prefix_for "$mode" "$WORK/s") ./server -Q -d $((DUR_S + 40)) > $f.server.log 2>&1 &
sp=\$!
sleep 1
env BENCH_IFACE=lo taskset -c $CPU_C $(prefix_for "$mode" "$WORK/c") ./client -Q -d $DUR_S -i 0.005 -W 4096 -C 4096 -I 0.001; echo client_rc=\$?
for c in \$sp \$(cat /proc/\$sp/task/*/children 2>/dev/null); do [ \"\$(readlink /proc/\$c/exe)\" = $d/server ] && kill -INT \$c; done
wait \$sp
chown -R $(id -u):$(id -g) $WORK $OUT.d" >"$f.client.log" 2>&1
        sudo -n ip netns del "$na"
        ;;
    esac
    case "$mode" in
    stat)
        [ -f "$WORK/c.perf" ] && mv "$WORK/c.perf" "$f.client.perf"
        [ -f "$WORK/s.perf" ] && mv "$WORK/s.perf" "$f.server.perf"
        ;;
    strace)
        [ -f "$WORK/c.strace" ] && mv "$WORK/c.strace" "$f.client.strace"
        [ -f "$WORK/s.strace" ] && mv "$WORK/s.strace" "$f.server.strace"
        ;;
    record)
        render_report "$WORK/c.data" "$f.client.report"
        render_report "$WORK/s.data" "$f.server.report"
        ;;
    esac
    echo "$(date +%T) b$blk $arm $cell $mode r$rep $(grep -m1 '^RESULT' "$f.client.log" | tr ' ' '\n' | grep -E '^(sent|rtt_p50_ms|cpu_s_per_Msample|drained)=' | tr '\n' ' ')" >>"$SUM"
}
MODES=(plain stat record strace)
blk=0
for arm in A B B A; do
    blk=$((blk + 1))
    say "--- block $blk arm $arm $(date -Is) ---"
    for rep in $(seq 1 "$REPS"); do
        for cell in x_p2 x_p3 s_p2; do
            for k in 0 1 2 3; do
                run_one $blk $arm $cell "${MODES[$(((k + rep + blk) % 4))]}" "$rep"
            done
        done
    done
done
say "--- runs done at $(($(date +%s) - T0)) s ---"
python3 "$HERE/stepA_perf_read.py" "$OUT.d" | tee -a "$SUM"

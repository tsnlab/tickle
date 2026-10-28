#!/usr/bin/env bash
# zenoh_fair_preflight.sh - the zenoh-pico arm of zenoh_fair_queue.sh, run briefly on this PC first, in two private
# network namespaces joined by a veth (rig_preflight.sh has no zenohpico arm; this is its counterpart for that one).
# Each of the four scenarios at p1 and p4, at a LOW rate (-i 0.0002 throughput, -i 0.01 latency) so it adds no real load
# to the PC: the job is to show that the binaries build against the pinned release (zenohpico/build.sh), take the
# campaign's argv (--warmup-s/--cooldown-s, -W/-C/-I/-T) and print every field the campaign and zenoh_fair_read.py read.
#
# CHECKS (implemented in check() below): a client and a server RESULT line; window=ok on every line; the transport
# field the campaign's qos gate requires (transport=tcp on reliable_*, transport=udp_multicast on best_effort_*);
# instrument=ok on the client; throughput: server received == client sent (a low-rate run on a clean veth loses
# nothing, so any shortfall is a harness defect, not loss); latency: rtt_p50_ms and rtt_p99_ms present and measured ==
# measured_sent. Exit 0 only if every check passed.
# Usage: zenoh_fair_preflight.sh      env: OUT (default /home/semih/zenoh_fair_results/preflight_<stamp>), SIZES
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZP="$HERE/../zenohpico"
OUT=${OUT:-/home/semih/zenoh_fair_results/preflight_$(date +%Y%m%d_%H%M%S)}
SIZES=${SIZES:-"p1 p4"}
BASE=/home/semih/zenoh_fair_results/pc
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
NS1=zfp1 NS2=zfp2
mkdir -p "$OUT"
fails=0
down() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap down EXIT
up() {
    down
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" &&
        sudo -n ip link add zfpv1 netns "$NS1" type veth peer name zfpv2 netns "$NS2" &&
        sudo -n ip -n "$NS1" addr add 10.79.0.1/24 dev zfpv1 && sudo -n ip -n "$NS2" addr add 10.79.0.2/24 dev zfpv2 || return 1
    local n
    for n in 1 2; do
        sudo -n ip -n "zfp$n" link set "zfpv$n" up && sudo -n ip -n "zfp$n" link set lo up &&
            sudo -n ip -n "zfp$n" route add 224.0.0.0/4 dev "zfpv$n" || return 1
    done
}
check() { # $1 name, $2 scen, $3 client log, $4 server log
    local c s why=""
    c=$(grep '^RESULT' "$3" | tail -1)
    s=$(grep '^RESULT' "$4" | tail -1)
    [ -n "$c" ] || why+=" no_client_RESULT"
    [ -n "$s" ] || why+=" no_server_RESULT"
    case "$c" in *window=ok*) ;; *) why+=" client_window" ;; esac
    case "$c" in *instrument=ok*) ;; *) why+=" client_instrument" ;; esac
    case "$2:$c" in reliable_*:*transport=tcp*) ;; best_effort_*:*transport=udp_multicast*) ;; *) why+=" transport" ;; esac
    case "$2" in
    *_throughput)
        case "$s" in *window=ok*) ;; *) why+=" server_window" ;; esac
        local sent recv
        sent=$(grep -oE ' sent=[0-9]+' <<<"$c" | head -1 | cut -d= -f2)
        recv=$(grep -oE ' received=[0-9]+' <<<"$s" | head -1 | cut -d= -f2)
        [ -n "$sent" ] && [ "$sent" = "$recv" ] || why+=" sent=${sent:-?}_received=${recv:-?}" ;;
    *_latency)
        case "$c" in *rtt_p50_ms=*rtt_p99_ms=*) ;; *) why+=" no_quantiles" ;; esac
        local ms mr
        ms=$(grep -oE 'measured_sent=[0-9]+' <<<"$c" | cut -d= -f2)
        mr=$(grep -oE ' measured=[0-9]+' <<<"$c" | cut -d= -f2)
        [ -n "$ms" ] && [ "$ms" = "$mr" ] && [ "$ms" -gt 0 ] || why+=" measured=${mr:-?}/${ms:-?}" ;;
    esac
    if [ -z "$why" ]; then echo "PASS $1"; else echo "FAIL $1:$why"; fails=$((fails + 1)); fi
}
for size in $SIZES; do
    for scen in best_effort_throughput reliable_throughput best_effort_latency reliable_latency; do
        name=${scen}_$size
        if ! (cd "$ZP" && ZENOH_BUILD_JOBS=2 ZENOH_PICO_PREFIX="$BASE/install_rel" ZENOH_PICO_SRC="$BASE/src_rel" \
            nice -n 19 ./build.sh "$scen" "$size") >"$OUT/build_$name.log" 2>&1; then
            echo "FAIL $name: build (see $OUT/build_$name.log)"; fails=$((fails + 1)); continue
        fi
        bin="$ZP/$name"
        senv="BENCH_IFACE=zfpv1" cenv="BENCH_IFACE=zfpv2"
        case $scen in reliable_*)
            senv="BENCH_IFACE=zfpv1 BENCH_ZENOH_LISTEN=tcp/10.79.0.1:7447"
            cenv="BENCH_IFACE=zfpv2 BENCH_ZENOH_CONNECT=tcp/10.79.0.1:7447" ;;
        esac
        case $scen in
        *_throughput) args="-i 0.0002 -d 2 --warmup-s 1 --cooldown-s 1" ;;
        *) args="-i 0.01 -d 2 -W 100 -C 100 -I 0.001 -T 5" ;;
        esac
        up || { echo "FAIL $name: netns"; fails=$((fails + 1)); continue; }
        # shellcheck disable=SC2086,SC2024 # word lists on purpose; the log is written as this user on purpose
        sudo -n ip netns exec "$NS1" env $senv nice -n 19 stdbuf -oL "$bin/server" $args \
            >"$OUT/${name}_server.log" 2>&1 &
        sp=$!
        sleep 1
        # shellcheck disable=SC2086
        timeout 60 sudo -n ip netns exec "$NS2" env $cenv nice -n 19 stdbuf -oL "$bin/client" $args \
            >"$OUT/${name}_client.log" 2>&1
        sleep 1
        # The server is the only process in its private namespace, so the namespace's PID list names it exactly.
        for p in $(sudo -n ip netns pids "$NS1"); do
            # root-owned: its /proc/PID/exe is read as root, through the same ip netns exec
            [ "$(sudo -n ip netns exec "$NS1" readlink "/proc/$p/exe" 2>/dev/null)" = "$(readlink -f "$bin/server")" ] &&
                sudo -n ip netns exec "$NS1" kill -INT "$p"
        done
        wait "$sp"
        check "$name" "$scen" "$OUT/${name}_client.log" "$OUT/${name}_server.log"
    done
done
echo "ZENOH_FAIR_PREFLIGHT fails=$fails out=$OUT"
[ "$fails" = 0 ]

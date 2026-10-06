#!/usr/bin/env bash
# rig_preflight.sh - every cell shape a rig job is about to run, run briefly on this PC first, in private network
# namespaces, with each RESULT line checked. Exit 0 only if every check passed.
#
# WHY (the user, 2026-10-06): rig hours went to harness bugs a one-minute local run would have shown - a drain loop
# that ended at the first ACKNACK (every TickLE throughput row drained=timeout, 3ae721aa), a latency client that
# stopped after ~124 pings (46ee6a83), pre-match samples counted as loss. None needs the rig to show itself; each
# needs the bench to run once and its RESULT line to be READ. So campaign_sweep.sh, campaign_ab_chain.sh,
# s6_transport_cells.sh and fair_*_remeasure.sh call this once, before they take the rig lock, so a broken harness
# never holds the rig. PREFLIGHT=0 skips it there.
#
# WHAT IT RUNS. The commit the rig job will deploy (SHA=, exported with git archive, so neither this checkout's
# untracked files nor its build objects are touched), built by each framework's own build.sh inside the export. Without
# SHA it exports this checkout's tracked files as they are in the working tree; PREFLIGHT_SRC=<dir> uses a tree in
# place (for showing it catch a bug). The builds run with HOME inside the work directory, so TickLE's core prefixes
# ($HOME/tickle_local_install*) are private to the run, for any commit, old ones included.
# Then each cell, for each framework in FWS, as one job:
#   PREFLIGHT_TOPO=veth   (default; campaign cells, cross-host on the rig) the client in one namespace and the server
#                         in another, joined by a veth pair 192.168.10.1/.2 with broadcast 192.168.10.255 (the
#                         address the TickLE benches hard-code). Each process gets a private tmpfs on /dev/shm, so
#                         TickLE cannot find the other's segment and the samples cross the link, as across two Pis.
#                         The cell's network condition is put on the client's veth egress, as on the rig's eth0.
#   PREFLIGHT_TOPO=samens (s6 / fair_samehost cells, same-host on the rig) both processes in one namespace, sharing
#                         one private /dev/shm, so the same-host transports are what carries the samples.
# Short on purpose: throughput -d $PF_DUR (1 s) between a $PF_EDGE_S (0.25 s) warm-up and cool-down; latency
# $PF_LAT_EDGE (512) warm-up and cool-down round trips, then -d $PF_LAT_D (1 s) at -i $PF_LAT_I (0.005), so more than
# a thousand pings in all. These replace the job's own -d/-i/window arguments; its QoS arguments are kept verbatim.
#
# WHAT IT CHECKS, per job (implemented in check_job below, not only listed here):
#   - the client exited 0 within its timeout, and the server stopped on SIGINT and exited 0;
#   - one RESULT line from the client and, except for the DDS latency servers (which print none), from the server;
#   - every required field present and numeric (required_fields);
#   - window=ok on every line that carries window=, and window= present on every throughput line and latency client;
#   - instrument=ok on every client line and throughput server line (veth only: a same-host pair moves its data off
#     the interface by design);
#   - recv>0;  sent/recv consistent: recv <= sent always; a RELIABLE KEEP_ALL throughput cell must deliver everything,
#     recv == sent, lost == 0 (a DDS server may count up to write_fail= sequence gaps: refused writes), and its
#     windows must agree (|win_recv - win_sent| <= 2%);
#   - drained=acked on every reliable_throughput client;
#   - latency: warmup= and cooldown= are what was asked, measured > 0, measured_sent >= 80% of -d / (-i + rtt), and
#     at N0 every measured ping came back (measured == measured_sent);
#   - nothing in either log, outside the RESULT lines, says terminate, timeout/timed out, abort, segmentation fault,
#     core dumped or sanitizer.
# A framework that cannot be built on this PC is a printed SKIP, never a pass: FastDDS 2.14 (the rig's) is not here.
# CycloneDDS is, as ROS lyrical's 11.0.1 (the rig runs jazzy's 0.10.5 or rolling's 11.0.1), so its harness is run.
#
# Usage: rig_preflight.sh 'scenario:payload:net:common args:tickle-only args' ...
#   net is N0..N3 as in campaign_sweep.sh. Example: 'reliable_throughput:p1:N0:-N 2048 -B 100:-Q'
#   env: SHA FWS PREFLIGHT_TOPO PREFLIGHT_SRC PF_JOBS(4) PF_DUR PF_EDGE_S PF_LAT_D PF_LAT_I PF_LAT_EDGE TICKLE_P4_PATH
#        PREFLIGHT_OUT (default ~/rig_results_safe/preflight/<stamp>_<sha8>; every job's logs and the summary)
# Needs passwordless `sudo -n ip` (tc runs inside the namespaces, through `ip netns exec`, so nothing else is shaped).
set -uo pipefail

# ------------------------------------------------------------------------------------------------ one job, as root
# Re-entered through `sudo -n ip netns exec <client ns> env ... bash rig_preflight.sh --job`, so this part runs as root
# inside the client's namespace. It starts the server, captures its PID from $! (never by name), runs the client
# under a timeout, stops the server with SIGINT and records both exit statuses. Every wait in it is bounded.
if [ "${1:-}" = "--job" ]; then
    # J_* come from the env given to `ip netns exec ... env`.
    mount -t tmpfs -o size=512m tmpfs /dev/shm || { echo "server_rc=- client_rc=- note=no_private_shm" >"$J_STATUS"; exit 1; }
    if [ -n "$J_NETEM" ]; then
        # shellcheck disable=SC2086 # the netem arguments are deliberately word-split
        tc qdisc add dev "$J_CIFACE" root netem $J_NETEM || { echo "server_rc=- client_rc=- note=netem_failed" >"$J_STATUS"; exit 1; }
    fi
    export LD_LIBRARY_PATH="$J_LDP"
    srv_env=("BENCH_IFACE=$J_SBENCH")
    [ -n "$J_SURI" ] && srv_env+=("CYCLONEDDS_URI=$J_SURI")
    cli_env=("BENCH_IFACE=$J_CBENCH")
    [ -n "$J_CURI" ] && cli_env+=("CYCLONEDDS_URI=$J_CURI")
    if [ -n "$J_SNS" ]; then
        # Another namespace, its own mount namespace (ip netns exec makes one) and its own /dev/shm. ip, sh and env
        # each exec the next, so $! is the server's own PID.
        # shellcheck disable=SC2016,SC2086 # $1/$@ are the inner sh's; J_ARGS is deliberately word-split into argv
        ip netns exec "$J_SNS" sh -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm && cd "$1" && shift && exec "$@"' \
            sh "$J_DIR" env "${srv_env[@]}" ./server $J_ARGS >"$J_SLOG" 2>&1 </dev/null &
    else
        # shellcheck disable=SC2086 # J_ARGS is deliberately word-split into argv
        (cd "$J_DIR" && exec env "${srv_env[@]}" ./server $J_ARGS) >"$J_SLOG" 2>&1 </dev/null &
    fi
    spid=$!
    sleep "$J_START_WAIT"
    # shellcheck disable=SC2086 # J_ARGS is deliberately word-split into argv
    (cd "$J_DIR" && exec timeout -s INT -k 5 "$J_CTIMEOUT" env "${cli_env[@]}" ./client $J_ARGS) >"$J_CLOG" 2>&1 </dev/null
    crc=$?
    stop=sigint
    case "$(readlink "/proc/$spid/exe" 2>/dev/null)" in
    */server) kill -INT "$spid" ;;
    *) stop=gone_before_sigint ;;
    esac
    for _ in $(seq 1 100); do
        kill -0 "$spid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$spid" 2>/dev/null; then
        stop=killed_after_10s
        kill -KILL "$spid"
    fi
    wait "$spid"
    src=$?
    echo "server_rc=$src client_rc=$crc server_stop=$stop" >"$J_STATUS"
    exit 0
fi

# ------------------------------------------------------------------------------------------------ the driver
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$HERE/$(basename "${BASH_SOURCE[0]}")"
REPO="$(cd "$HERE/../../.." && pwd)"
FWS=${FWS:-tickle cyclonedds fastdds}
TOPO=${PREFLIGHT_TOPO:-veth}
PF_JOBS=${PF_JOBS:-4}
PF_DUR=${PF_DUR:-1}
PF_EDGE_S=${PF_EDGE_S:-0.25}
PF_LAT_D=${PF_LAT_D:-1}
PF_LAT_I=${PF_LAT_I:-0.005}
PF_LAT_EDGE=${PF_LAT_EDGE:-512}
CYCLONE_LIB=${CYCLONE_LIB:-/opt/ros/lyrical/lib/x86_64-linux-gnu}
CYCLONE_BIN=${CYCLONE_BIN:-/opt/ros/lyrical/bin}
case "$TOPO" in veth | samens) ;; *) echo "PREFLIGHT_TOPO must be veth or samens" >&2; exit 2 ;; esac
[ "$#" -gt 0 ] || { sed -n '2,50p' "$SELF" | sed 's/^# \{0,1\}//'; exit 2; }
T0=$(date +%s)

if [ -n "${PREFLIGHT_SRC:-}" ]; then
    LABEL="src-$(basename "$PREFLIGHT_SRC")"
elif [ -n "${SHA:-}" ]; then
    FULL_SHA=$(git -C "$REPO" rev-parse --verify -q "${SHA}^{commit}") || {
        git -C "$REPO" fetch -q origin "$SHA" 2>/dev/null
        FULL_SHA=$(git -C "$REPO" rev-parse --verify -q "${SHA}^{commit}")
    } || { echo "PREFLIGHT FAIL: commit $SHA is not in $REPO and could not be fetched" >&2; exit 1; }
    LABEL=${FULL_SHA:0:8}
else
    LABEL="worktree-$(git -C "$REPO" rev-parse --short HEAD)"
fi
OUTD=${PREFLIGHT_OUT:-$HOME/rig_results_safe/preflight/$(date +%Y%m%d-%H%M%S)_${LABEL}_$$}
mkdir -p "$OUTD/jobs" || exit 1
SUMMARY="$OUTD/summary.txt"
: >"$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }

# The work directory: kept in a variable from mktemp, the same command, and removed only by that name.
WORK=$(mktemp -d /tmp/tickle_preflight.XXXXXX) || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
# shellcheck disable=SC2329 # invoked by the EXIT trap below, which shellcheck cannot see
cleanup() {
    local ns pids
    # Every namespace this run created, from its own list: whatever is still inside is ours by construction.
    while read -r ns; do
        [ -n "$ns" ] || continue
        pids=$(sudo -n ip netns pids "$ns" 2>/dev/null | tr '\n' ' ')
        # shellcheck disable=SC2086
        [ -n "${pids// /}" ] && sudo -n ip netns exec "$ns" kill -KILL $pids 2>/dev/null
        sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    # The jobs ran as root, so their files are root's, in directories that are ours: removable.
    [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf "$WORK"
    return 0
}
trap cleanup EXIT
# Interrupted: stop the job subshells first (their PIDs are this shell's own jobs), or one that was setting up would go
# on to run a whole job after cleanup() had already swept the namespaces - seen in testing, a server outliving the run.
# shellcheck disable=SC2329 # invoked by the INT/TERM trap below
on_signal() {
    local j
    for j in $(jobs -p); do kill -TERM "$j" 2>/dev/null; done
    wait 2>/dev/null
    exit 130
}
trap on_signal INT TERM

# ------------------------------------------------------------------------------------------------ source tree
if [ -n "${PREFLIGHT_SRC:-}" ]; then
    SRC=$(cd "$PREFLIGHT_SRC" && pwd) || exit 1
    say "=== rig preflight $(date -Is): source $SRC (in place), topology $TOPO, FWS='$FWS' ==="
else
    SRC="$WORK/src"
    mkdir -p "$SRC"
    if [ -n "${FULL_SHA:-}" ]; then
        git -C "$REPO" archive "$FULL_SHA" | tar -x -C "$SRC" || { echo "PREFLIGHT FAIL: git archive $FULL_SHA" >&2; exit 1; }
        say "=== rig preflight $(date -Is): commit $FULL_SHA, topology $TOPO, FWS='$FWS' ==="
    else
        (cd "$REPO" && git ls-files -z | tar --null -T - -cf - 2>/dev/null) | tar -x -C "$SRC" 2>/dev/null
        say "=== rig preflight $(date -Is): $REPO working tree (tracked files), topology $TOPO, FWS='$FWS' ==="
    fi
fi
PH="$SRC/examples/perf_hil"
[ -d "$PH/tickle" ] || { say "PREFLIGHT FAIL: no examples/perf_hil/tickle in $SRC"; exit 1; }
say "    output: $OUTD"

# ------------------------------------------------------------------------------------------------ cells and builds
# spec = scenario:payload:net:common:tickle_extra
declare -a SPECS=()
for spec in "$@"; do
    IFS=':' read -r scen payload net _common _extra <<<"$spec"
    case "$scen:$payload:$net" in
    *_throughput:p[1-4]:N[0-3] | reliable_latency:p[1-4]:N[0-3]) ;;
    *) say "PREFLIGHT FAIL: cannot read cell spec '$spec' (want scenario:pN:NN:args:tickle_args)"; exit 1 ;;
    esac
    SPECS+=("$spec")
done
mapfile -t SPECS < <(printf '%s\n' "${SPECS[@]}" | awk '!seen[$0]++')
mapfile -t VARIANTS < <(for s in "${SPECS[@]}"; do IFS=':' read -r a b _ <<<"$s"; echo "${a}_${b}"; done | sort -u)
say "    ${#SPECS[@]} cell(s), ${#VARIANTS[@]} variant(s): ${VARIANTS[*]}"

FAILS=()
SKIPPED=()
declare -A FW_OK=()
build_fw() { # $1 fw
    local fw=$1 v scen size log
    case "$fw" in
    fastdds)
        # The rig's harness is written against FastDDS 2.14's API; this PC has none of it.
        if ! compgen -G "/opt/ros/*/lib/libfastrtps.so.2.14*" >/dev/null && ! compgen -G "/usr/lib/*/libfastrtps.so.2.14*" >/dev/null; then
            SKIPPED+=("fastdds: FastDDS 2.14 is not installed on this PC, so its harness cannot be built or run here")
            say "SKIP fastdds: FastDDS 2.14 is not on this PC - its cells are NOT preflighted"
            return 0
        fi ;;
    cyclonedds)
        if [ ! -x "$CYCLONE_BIN/idlc" ] && ! command -v idlc >/dev/null; then
            SKIPPED+=("cyclonedds: no idlc on this PC")
            say "SKIP cyclonedds: no idlc on this PC - its cells are NOT preflighted"
            return 0
        fi ;;
    tickle) ;;
    *) FAILS+=("unknown framework $fw"); return 0 ;;
    esac
    for v in "${VARIANTS[@]}"; do
        scen=${v%_p[0-9]}
        size=${v##*_}
        log="$OUTD/build_${fw}_$v.log"
        # HOME points into the work directory for the build: TickLE's build.sh installs its core under $HOME, and the
        # prefixes there belong to whatever checkout last built them - a preflight must neither use nor refill them.
        if ! (cd "$PH/$fw" && HOME="$WORK/home" PATH="$CYCLONE_BIN:$PATH" \
            TICKLE_P4_PATH="${TICKLE_P4_PATH:-frag}" ./build.sh "$scen" "$size") >"$log" 2>&1; then
            FAILS+=("$fw $v: build failed (see $log)")
            say "FAIL build $fw $v - $(tail -1 "$log")"
            continue
        fi
        if [ ! -x "$PH/$fw/$v/client" ] || [ ! -x "$PH/$fw/$v/server" ]; then
            FAILS+=("$fw $v: build reported success but $PH/$fw/$v/{client,server} are missing")
            continue
        fi
    done
    FW_OK[$fw]=1
}
for fw in $FWS; do build_fw "$fw"; done
say "    built in $(($(date +%s) - T0)) s"

# ------------------------------------------------------------------------------------------------ the checks
field() { # $1 line, $2 name -> value or empty
    grep -oE "(^| )$2=[^ ]*" <<<"$1" | head -1 | sed 's/^ //; s/^[^=]*=//'
}
num_ok() { [[ "$1" =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; }
required_fields() { # $1 scenario, $2 role
    case "$1:$2" in
    reliable_throughput:client) echo "sent write_fail send_mbps win_s win_sent win_send_mbps warmup_s cooldown_s" ;;
    reliable_throughput:server) echo "recv lost win_s win_recv win_recv_mbps warmup_s cooldown_s" ;;
    best_effort_throughput:client) echo "sent send_mbps win_s win_sent win_send_mbps warmup_s cooldown_s" ;;
    best_effort_throughput:server) echo "recv lost win_s win_recv win_recv_mbps warmup_s cooldown_s" ;;
    reliable_latency:client) echo "sent recv rtt_min_ms rtt_avg_ms rtt_max_ms warmup cooldown measured measured_sent" ;;
    reliable_latency:server) echo "" ;;
    esac
}
# Appends "<why>" lines to $1 (a file) for everything wrong with one job.
check_job() { # $1 problems file, $2 fw, $3 scen, $4 net, $5 keep_all(0/1), $6 clog, $7 slog, $8 status
    local P=$1 fw=$2 scen=$3 net=$4 keep_all=$5 clog=$6 slog=$7 status=$8
    local st crc src stop cline sline n role line f v
    st=$(cat "$status" 2>/dev/null)
    [ -n "$st" ] || { echo "the job left no status (it did not finish)" >>"$P"; return; }
    crc=$(field "$st" client_rc); src=$(field "$st" server_rc); stop=$(field "$st" server_stop)
    case "$st" in *note=*) echo "job setup failed: $(field "$st" note)" >>"$P"; return ;; esac
    [ "$crc" = 0 ] || echo "client exit status $crc$([ "$crc" = 124 ] && echo ' (killed by the preflight timeout: it hung)')" >>"$P"
    [ "$stop" = sigint ] || echo "server stop: $stop" >>"$P"
    [ "$src" = 0 ] || echo "server exit status $src" >>"$P"
    n=$(grep -c '^RESULT:' "$clog" 2>/dev/null)
    [ "$n" = 1 ] || echo "client printed $n RESULT lines, want 1" >>"$P"
    cline=$(grep '^RESULT:' "$clog" 2>/dev/null | head -1)
    sline=$(grep '^RESULT:' "$slog" 2>/dev/null | head -1)
    if [ -z "$sline" ] && ! { [ "$scen" = reliable_latency ] && [ "$fw" != tickle ]; }; then
        echo "server printed no RESULT line" >>"$P"
    fi
    for role in client server; do
        line=$cline
        [ "$role" = server ] && line=$sline
        [ -n "$line" ] || continue
        for f in $(required_fields "$scen" "$role"); do
            v=$(field "$line" "$f")
            if [ -z "$v" ]; then echo "$role: no $f= field" >>"$P"
            elif ! num_ok "$v"; then echo "$role: $f=$v is not a number" >>"$P"; fi
        done
        v=$(field "$line" window)
        if [ -n "$v" ]; then
            [ "$v" = ok ] || echo "$role: window=$v" >>"$P"
        elif [ "$scen" != reliable_latency ] || [ "$role" = client ]; then
            echo "$role: no window= field" >>"$P"
        fi
        # The TickLE latency server's line carries no BenchStats fields, so instrument= is read where it is printed.
        if [ "$TOPO" = veth ] && { [ "$role" = client ] || [ "$scen" != reliable_latency ]; }; then
            v=$(field "$line" instrument)
            [ "$v" = ok ] || echo "$role: instrument=${v:-absent}" >>"$P"
        fi
    done
    [ -n "$cline" ] || return
    local sent recv lost ws wr wf
    if [ "$scen" = reliable_latency ]; then
        sent=$(field "$cline" sent); recv=$(field "$cline" recv)
        local meas msent want
        meas=$(field "$cline" measured); msent=$(field "$cline" measured_sent)
        num_ok "$recv" && [ "${recv%.*}" -gt 0 ] || echo "client recv=${recv:-absent}, want > 0" >>"$P"
        num_ok "$sent" && num_ok "$recv" && [ "${recv%.*}" -gt "${sent%.*}" ] && echo "client recv=$recv > sent=$sent" >>"$P"
        [ "$(field "$cline" warmup)" = "$PF_LAT_EDGE" ] || echo "client warmup=$(field "$cline" warmup), asked $PF_LAT_EDGE" >>"$P"
        [ "$(field "$cline" cooldown)" = "$PF_LAT_EDGE" ] || echo "client cooldown=$(field "$cline" cooldown), asked $PF_LAT_EDGE" >>"$P"
        # A ping waits for its pong and then -i, so a run sends about -d / (-i + RTT): under N2, 66 in 1 s.
        want=$(awk -v d="$PF_LAT_D" -v i="$PF_LAT_I" -v r="$(field "$cline" rtt_avg_ms)" \
            'BEGIN{p = i + r / 1000.0; printf "%d", 0.8 * d / p}')
        if num_ok "$meas" && num_ok "$msent"; then
            [ "${meas%.*}" -gt 0 ] || echo "client measured=0" >>"$P"
            [ "${msent%.*}" -ge "$want" ] || echo "client measured_sent=$msent, want >= $want (80% of -d / (-i + rtt))" >>"$P"
            [ "${meas%.*}" -le "${msent%.*}" ] || echo "client measured=$meas > measured_sent=$msent" >>"$P"
            if [ "$net" = N0 ] && [ "$meas" != "$msent" ]; then
                echo "client measured=$meas of measured_sent=$msent on a lossless link" >>"$P"
            fi
        fi
    else
        sent=$(field "$cline" sent); recv=$(field "$sline" recv); lost=$(field "$sline" lost)
        ws=$(field "$cline" win_sent); wr=$(field "$sline" win_recv)
        if num_ok "$sent" && num_ok "$recv"; then
            [ "${recv%.*}" -gt 0 ] || echo "server recv=0" >>"$P"
            [ "${recv%.*}" -le "${sent%.*}" ] || echo "server recv=$recv > client sent=$sent" >>"$P"
            if [ "$keep_all" = 1 ]; then
                [ "$recv" = "$sent" ] || echo "RELIABLE KEEP_ALL delivered recv=$recv of sent=$sent" >>"$P"
                # The DDS servers count sequence gaps, and a write refused after max_blocking_time leaves one: a gap
                # up to write_fail= is a refused write, not a lost sample. TickLE's lost= must be 0 outright.
                wf=$(field "$cline" write_fail)
                { num_ok "$wf" && [ "$fw" != tickle ]; } || wf=0
                num_ok "$lost" && [ "${lost%.*}" -le "${wf%.*}" ] ||
                    echo "RELIABLE KEEP_ALL server lost=$lost (client write_fail=$wf)" >>"$P"
                if num_ok "$ws" && num_ok "$wr"; then
                    awk -v a="$ws" -v b="$wr" 'BEGIN{d=a-b; if (d<0) d=-d; exit !(a > 0 && d <= 0.02 * a)}' ||
                        echo "windows disagree: client win_sent=$ws, server win_recv=$wr" >>"$P"
                fi
            fi
        fi
        if [ "$scen" = reliable_throughput ]; then
            v=$(field "$cline" drained)
            [ "$v" = acked ] || echo "client drained=${v:-absent}, want acked" >>"$P"
        fi
    fi
    local bad
    bad=$(cat "$clog" "$slog" 2>/dev/null | grep -v '^RESULT:' |
        grep -iE 'terminat|timeout|timed out|abort|segmentation fault|core dumped|sanitizer' | head -3)
    [ -z "$bad" ] || echo "log says: $(tr '\n' '|' <<<"$bad")" >>"$P"
}

# ------------------------------------------------------------------------------------------------ running jobs
netem_for() {
    case "$1" in
    N0) echo "" ;; N1) echo "loss 5%" ;; N2) echo "delay 10ms 2ms" ;; N3) echo "delay 1ms reorder 5% 50%" ;;
    esac
}
run_job() { # $1 id, $2 fw, $3 spec
    local id=$1 fw=$2 spec=$3 scen payload net common extra
    IFS=':' read -r scen payload net common extra <<<"$spec"
    local jd
    jd="$OUTD/jobs/$(printf '%03d' "$id")_${fw}_${scen}_${payload}_${net}"
    mkdir -p "$jd"
    local nsa="pf$$-${id}a" nsb="pf$$-${id}b" va="pf$$v${id}a" vb="pf$$v${id}b"
    local args window keep_all=0
    if [ "$scen" = reliable_latency ]; then
        window="-W $PF_LAT_EDGE -C $PF_LAT_EDGE -I 0.001"
        args="$common -i $PF_LAT_I -d $PF_LAT_D $window"
    else
        window="--warmup-s $PF_EDGE_S --cooldown-s $PF_EDGE_S"
        args="$common -d $PF_DUR $window"
    fi
    [ "$fw" = tickle ] && [ -n "$extra" ] && args="$extra $args"
    if [ "$scen" = reliable_throughput ]; then
        if [ "$fw" = tickle ]; then
            case " $args " in *" -Q "*) keep_all=1 ;; esac
        else
            case " $args " in *" -K "*) ;; *) keep_all=1 ;; esac
        fi
    fi
    echo "$nsa" >>"$NS_LIST"
    sudo -n ip netns add "$nsa" || { echo "cannot create namespace $nsa" >"$jd/problems"; return; }
    sudo -n ip -n "$nsa" link set lo up
    local sns="" ciface siface
    if [ "$TOPO" = veth ]; then
        echo "$nsb" >>"$NS_LIST"
        if ! { sudo -n ip netns add "$nsb" && sudo -n ip link add "$va" type veth peer name "$vb" &&
            sudo -n ip link set "$va" netns "$nsa" && sudo -n ip link set "$vb" netns "$nsb" &&
            sudo -n ip -n "$nsa" addr add 192.168.10.1/24 brd + dev "$va" &&
            sudo -n ip -n "$nsb" addr add 192.168.10.2/24 brd + dev "$vb" &&
            sudo -n ip -n "$nsb" link set lo up && sudo -n ip -n "$nsa" link set "$va" up &&
            sudo -n ip -n "$nsb" link set "$vb" up; }; then
            echo "cannot build the veth pair" >"$jd/problems"
            return
        fi
        sns=$nsb; ciface=$va; siface=$vb
    else
        if ! { sudo -n ip -n "$nsa" link add "$va" type dummy && sudo -n ip -n "$nsa" link set "$va" multicast on &&
            sudo -n ip -n "$nsa" addr add 192.168.10.1/24 brd + dev "$va" && sudo -n ip -n "$nsa" link set "$va" up; }; then
            echo "cannot build the namespace's link" >"$jd/problems"
            return
        fi
        ciface=$va; siface=$va
    fi
    # netem only between two namespaces: a same-host pair has no link of its own to shape.
    local netem=""
    [ "$TOPO" = veth ] && netem=$(netem_for "$net")
    local dir="$PH/$fw/${scen}_${payload}" uri_c="" uri_s="" ldp="" wait_s=1
    if [ "$fw" = cyclonedds ]; then
        # As the rig's run_scenario.sh, with the namespace's own link named instead of eth0.
        uri_c="<CycloneDDS><Domain><General><Interfaces><NetworkInterface name=\"$va\"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>"
        uri_s=$uri_c
        [ "$TOPO" = veth ] && uri_s=${uri_c//$va/$vb}
        ldp=$CYCLONE_LIB
        wait_s=2
    fi
    # The client's timeout: warm-up + measured + cool-down + match and drain allowances, generously.
    local ct=60
    [ "$net" = N2 ] && ct=120
    echo "fw=$fw scen=$scen payload=$payload net=$net topo=$TOPO keep_all=$keep_all args='$args'" >"$jd/job"
    local cbench=$ciface sbench=$siface
    [ "$TOPO" = samens ] && cbench=lo sbench=lo
    # The job log is written by this shell, as the invoking user, and not by the sudo'd process: intended.
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$nsa" env J_STATUS="$jd/status" J_CLOG="$jd/client.log" J_SLOG="$jd/server.log" \
        J_DIR="$dir" J_SNS="$sns" J_CIFACE="$ciface" J_NETEM="$netem" J_ARGS="$args" J_CBENCH="$cbench" \
        J_SBENCH="$sbench" J_CURI="$uri_c" J_SURI="$uri_s" J_LDP="$ldp" J_START_WAIT="$wait_s" J_CTIMEOUT="$ct" \
        bash "$SELF" --job \
        >"$jd/job.log" 2>&1
    : >"$jd/problems"
    check_job "$jd/problems" "$fw" "$scen" "$net" "$keep_all" "$jd/client.log" "$jd/server.log" "$jd/status"
    sudo -n ip netns del "$nsa" 2>/dev/null
    [ "$TOPO" = veth ] && sudo -n ip netns del "$nsb" 2>/dev/null
}

id=0
for spec in "${SPECS[@]}"; do
    for fw in $FWS; do
        [ "${FW_OK[$fw]:-0}" = 1 ] || continue
        id=$((id + 1))
        while [ "$(jobs -rp | wc -l)" -ge "$PF_JOBS" ]; do wait -n; done
        run_job "$id" "$fw" "$spec" &
    done
done
wait

# ------------------------------------------------------------------------------------------------ verdict
njobs=0
for jd in "$OUTD"/jobs/*/; do
    [ -f "$jd/job" ] || continue
    njobs=$((njobs + 1))
    desc=$(sed -E "s/ args='.*//" "$jd/job")
    if [ ! -f "$jd/problems" ]; then
        FAILS+=("$desc: the job did not run to its checks")
        say "FAIL  $desc: the job did not run to its checks"
    elif [ -s "$jd/problems" ]; then
        FAILS+=("$desc: $(paste -sd';' "$jd/problems")")
        say "FAIL  $desc"
        sed 's/^/        /' "$jd/problems" | tee -a "$SUMMARY"
        say "        logs: $jd"
    else
        say "ok    $desc  | $(grep -h '^RESULT:' "$jd/client.log" | grep -oE ' (sent|recv|win_send_mbps|measured|rtt_avg_ms|drained)=[^ ]*' | tr -d '\n')"
    fi
done
[ "$njobs" -gt 0 ] || [ "${#FAILS[@]}" -gt 0 ] || FAILS+=("no job ran at all - nothing was checked")
for s in "${SKIPPED[@]}"; do say "SKIP  $s"; done
el=$(($(date +%s) - T0))
if [ "${#FAILS[@]}" -gt 0 ]; then
    say "PREFLIGHT FAIL: ${#FAILS[@]} problem(s) in $njobs job(s), ${el} s. Do not take the rig until these are fixed:"
    for f in "${FAILS[@]}"; do say "  - $f"; done
    exit 1
fi
say "PREFLIGHT PASS: $njobs job(s), every check passed, ${el} s (summary $SUMMARY)"
exit 0

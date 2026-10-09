#!/usr/bin/env bash
# rmw_samehost_cell.sh - ONE same-host rmw run on the host it runs on: one arm, one cell, one repetition.
#
# Driven by rmw_samehost.sh, which copies it to the Pi (or runs it in a private namespace on the PC for the
# preflight) and reads what it leaves in $CELL_DIR with rmw_samehost_summary.py. It decides nothing itself: it runs
# the two processes, records what they did and what the host saw, and every reading rule lives in the summary.
#
# Two kinds of cell:
#   rtt   pong_node, then ping_node (rmw_tickle/rmw_perf_pingpong) with --stamps, so every round trip's send and
#         reply times (CLOCK_MONOTONIC) are on file and the summary can drop the warm-up and cool-down round trips
#         by sequence number. A sampler reads every process's CPU (/proc/PID/task/*/schedstat), its VmHWM and the
#         host's interface counters (/proc/net/dev) every SAMPLE_S on the same clock, so CPU and the transport
#         witness are taken over exactly the measured round trips.
#   tput  apex perf_test: subscriber (-p 0 -s 1), then publisher (-p 1 -s 0) at -r 0. CPU is perf_test's own
#         getrusage rows, windowed by the summary; the interface counters are read around the publisher's life.
#
# Every arm runs on its rmw's shipped defaults (docs/TESTING.md section 5): no profile, URI or session file, no
# interface restriction, nothing pinned - except arm tickle's TICKLE_BROADCAST_ADDR (see the case below; the shipped
# rmw_tickle runs as its own arm, tickle_shipped). The process environment is scrubbed of every variable that steers a
# middleware, and the process starts in an empty directory, so Fast DDS cannot pick up a DEFAULT_FASTRTPS_PROFILES.xml
# from its working directory. What each process actually received is read back from /proc/PID/environ.
#
# Inputs (environment): ARM (tickle|tickle_loans|tickle_shipped|fastdds|cyclonedds|zenoh), TICKLE_BCAST (arm tickle), KIND (rtt|tput), MSG (rtt: bench|array1k; tput:
# Array1k|Array4k), QOS (best_effort|reliable), WAIT (rtt: block|poll), DUR (rtt: ping -d; tput: publisher
# --max_runtime), RTT_I (ping -i), POLL_ARGS, CELL_DIR (created fresh), ROS_SETUP (setup.bash), OVERLAYS
# (local_setup.bash files, in order), PINGPONG (directory of ping_node/pong_node), PERF_TEST, ZENOHD, DOMAIN,
# SAMPLE_S.
set -uo pipefail
: "${ARM:?}" "${KIND:?}" "${MSG:?}" "${QOS:?}" "${DUR:?}" "${CELL_DIR:?}" "${ROS_SETUP:?}" "${DOMAIN:?}"
WAIT=${WAIT:-block}
RTT_I=${RTT_I:-0.001}
POLL_ARGS=${POLL_ARGS:-}
OVERLAYS=${OVERLAYS:-}
SAMPLE_S=${SAMPLE_S:-0.1}
PINGPONG=${PINGPONG:-}
PERF_TEST=${PERF_TEST:-}
ZENOHD=${ZENOHD:-}
TBCAST=${TICKLE_BCAST:-} # read before the scrub below unsets every TICKLE_* variable

rm -rf "$CELL_DIR"
mkdir -p "$CELL_DIR/cwd" || exit 1
META="$CELL_DIR/meta.txt"
meta() { echo "$*" >>"$META"; }
mono_ns() { python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC))'; }
meta "arm=$ARM kind=$KIND msg=$MSG qos=$QOS wait=$WAIT dur=$DUR rtt_i=$RTT_I poll_args=${POLL_ARGS// /_} domain=$DOMAIN host=$(hostname) tickle_bcast=$TBCAST"

# ---- the environment: ROS, then nothing else that steers a middleware ------------------------------------------
set +u
# shellcheck disable=SC1090 # paths are inputs
source "$ROS_SETUP"
for o in $OVERLAYS; do
    # shellcheck disable=SC1090
    source "$o"
done
set -u
for v in $(env | sed -n 's/^\(FASTRTPS_[A-Z_]*\|FASTDDS_[A-Z_]*\|RMW_FASTRTPS_[A-Z_]*\|CYCLONEDDS_[A-Z_]*\|ZENOH_[A-Z_]*\|TICKLE_[A-Z_]*\|RMW_TICKLE_[A-Z_]*\|PINGPONG_[A-Z_]*\|ROS_LOCALHOST_ONLY\|ROS_AUTOMATIC_DISCOVERY_RANGE\|ROS_STATIC_PEERS\|ROS_DISABLE_LOANED_MESSAGES\|SKIP_DEFAULT_XML_FILE\|LD_PRELOAD\)=.*/\1/p'); do
    unset "$v"
done
case "$ARM" in
# tickle: TICKLE_BROADCAST_ADDR names the link (TICKLE_BCAST), as every rig rmw harness sets it; without it rmw_tickle
# cannot learn its own address, so it never sees a peer as same-host and never builds its segment (tickle_shipped).
tickle) export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR="${TBCAST:?arm tickle needs TICKLE_BCAST}" ;;
# tickle_loans: arm tickle with loaned takes opted into. rcl (jazzy and later) leaves a subscription's
# disable_loaned_message true unless ROS_DISABLE_LOANED_MESSAGES=0, so without it rclcpp never takes a loan even from
# an rmw whose can_loan_messages is set; and rclcpp's publish(const T &) never borrows one (ab_loans.sh's header).
# Since 2026-10-09 it opts into the publish side too: the ping/pong nodes publish through a loan
# (PINGPONG_LOANED_PUBLISH=1; perf_test never borrows), built in the ring slot where rmw_tickle can
# (RMW_TICKLE_LOAN_PUBLISH_SLOTS=1, docs/RMW.md "Loaned messages"; a build without it ignores both).
tickle_loans) export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR="${TBCAST:?arm tickle_loans needs TICKLE_BCAST}" \
    ROS_DISABLE_LOANED_MESSAGES=0 PINGPONG_LOANED_PUBLISH=1 RMW_TICKLE_LOAN_PUBLISH_SLOTS=1 ;;
tickle_shipped) export RMW_IMPLEMENTATION=rmw_tickle ;;
fastdds) export RMW_IMPLEMENTATION=rmw_fastrtps_cpp ;;
cyclonedds) export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp ;;
zenoh) export RMW_IMPLEMENTATION=rmw_zenoh_cpp ;;
*) meta "fatal=unknown_arm"; exit 1 ;;
esac
export ROS_DOMAIN_ID=$DOMAIN
export ROS_LOG_DIR="$CELL_DIR/roslog"
cd "$CELL_DIR/cwd" || exit 1

# ---- what was running before: leftovers of any kind make the run VOID (the summary reads these counts) ---------
count_exe() { # count_exe <suffix>...: processes whose /proc/PID/exe ends in one of the names
    local n=0 p e s
    for p in /proc/[0-9]*; do
        e=$(readlink "$p/exe" 2>/dev/null) || continue
        for s in "$@"; do
            case "$e" in */"$s") n=$((n + 1)); break ;; esac
        done
    done
    echo "$n"
}
meta "leftovers=$(count_exe ping_node pong_node perf_test rmw_zenohd) roudi_procs=$(count_exe iox-roudi)"

# ---- launch: the PID is the process's own, written by itself just before exec ---------------------------------
# <role>.lpid is what was exec'd; <role>.pid, which the sampler and identity() read, is the process itself - the
# same PID, except for the ping, which runs under timeout and gets its <role>.pid from parentage below.
launch() { # launch <role> <cmd...>
    local role=$1
    shift
    # shellcheck disable=SC2016 # $$, $0 and $@ are the inner bash's
    setsid bash -c 'echo $$ > "$0"; exec "$@"' "$CELL_DIR/$role.lpid" "$@" >"$CELL_DIR/$role.log" 2>&1 </dev/null &
    for _ in $(seq 1 50); do
        [ -s "$CELL_DIR/$role.lpid" ] && break
        sleep 0.1
    done
    [ "$role" = ping ] || cp "$CELL_DIR/$role.lpid" "$CELL_DIR/$role.pid"
    meta "launch role=$role pid=$(cat "$CELL_DIR/$role.lpid" 2>/dev/null) t_mono_ns=$(mono_ns)"
}
pid_of() { cat "$CELL_DIR/$1.pid" 2>/dev/null; }
alive() { # alive <role> <exe suffix>
    local p
    p=$(pid_of "$1")
    [ -n "$p" ] && case "$(readlink "/proc/$p/exe" 2>/dev/null)" in */"$2") return 0 ;; esac
    return 1
}
identity() { # identity <role>: the rmw libraries the process has mapped, and the treatment it received
    local p
    p=$(pid_of "$1")
    meta "maps role=$1 libs=$(grep -o '/[^ ]*librmw_[a-z_]*\.so' "/proc/$p/maps" 2>/dev/null | sort -u | tr '\n' ',')"
    meta "treat role=$1 env=$(tr '\0' '\n' <"/proc/$p/environ" 2>/dev/null | grep -E '^(RMW_|FASTRTPS_|FASTDDS_|CYCLONEDDS_|ZENOH_|TICKLE_|PINGPONG_|ROS_DOMAIN_ID|ROS_LOCALHOST_ONLY|ROS_AUTOMATIC_DISCOVERY_RANGE|ROS_STATIC_PEERS|ROS_DISABLE_LOANED_MESSAGES|LD_PRELOAD)' | sort | tr '\n' ';' | tr ' ' '_')"
}
stop() { # stop <role> <exe suffix>: SIGINT, so rmw_tickle prints its traffic line; KILL only after 10 s
    local p how=sigint
    p=$(pid_of "$1")
    alive "$1" "$2" || { meta "stop role=$1 how=gone_before_stop"; return 0; }
    kill -INT "$p"
    for _ in $(seq 1 100); do
        [ -d "/proc/$p" ] || break
        sleep 0.1
    done
    if alive "$1" "$2"; then
        how=killed_after_10s
        kill -KILL "$p"
    fi
    meta "stop role=$1 how=$how"
}
netdev() { # one line: every interface's rx/tx packets and bytes, and lo's alone
    awk -F'[: ]+' 'NR > 2 { sub(/^ +/, ""); i = $1; rb = $2; rp = $3; tb = $10; tp = $11
        ap += rp + tp; ab += rb + tb; if (i == "lo") { lp += rp + tp; lb += rb + tb } }
        END { printf "all_pkts=%d all_bytes=%d lo_pkts=%d lo_bytes=%d", ap, ab, lp, lb }' /proc/net/dev
}

# The sampler: every SAMPLE_S, on CLOCK_MONOTONIC, each role's summed thread run time and VmHWM, and the interface
# counters. Roles are read from $CELL_DIR/<role>.pid as they appear; a process that has exited stops being read and
# keeps its last values. It ends when $CELL_DIR/sampler.stop exists. Each S line is followed by an F line with each
# role's minor/major page faults and voluntary/involuntary context switches (the summary reads only S lines).
cat >"$CELL_DIR/sampler.py" <<'PY'
import os, sys, time
d, period, roles = sys.argv[1], float(sys.argv[2]), sys.argv[3].split(",")
def cpu(pid):
    s = 0
    for t in os.listdir(f"/proc/{pid}/task"):
        with open(f"/proc/{pid}/task/{t}/schedstat") as f:
            s += int(f.read().split()[0])
    return s
def hwm(pid):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    return 0
def faults(pid):
    # minflt/majflt of the whole process (/proc/PID/stat fields 10 and 12, after the comm's closing paren), then
    # voluntary/involuntary context switches summed over its threads.
    with open(f"/proc/{pid}/stat") as f:
        v = f.read().rsplit(")", 1)[1].split()
    vc = ic = 0
    for t in os.listdir(f"/proc/{pid}/task"):
        with open(f"/proc/{pid}/task/{t}/status") as f:
            for line in f:
                if line.startswith("voluntary_ctxt_switches:"):
                    vc += int(line.split()[1])
                elif line.startswith("nonvoluntary_ctxt_switches:"):
                    ic += int(line.split()[1])
    return f"{v[7]}/{v[9]}/{vc}/{ic}"
def net():
    ap = ab = lp = lb = 0
    with open("/proc/net/dev") as f:
        for line in list(f)[2:]:
            name, rest = line.split(":", 1)
            v = rest.split()
            p, b = int(v[1]) + int(v[9]), int(v[0]) + int(v[8])
            ap += p; ab += b
            if name.strip() == "lo":
                lp += p; lb += b
    return ap, ab, lp, lb
out = open(f"{d}/sampler.txt", "w")
pids = {}
while not os.path.exists(f"{d}/sampler.stop"):
    t = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    fields = []
    ffields = []
    for r in roles:
        if r not in pids:
            try:
                with open(f"{d}/{r}.pid") as f:
                    pids[r] = int(f.read().strip())
            except (OSError, ValueError):
                continue
        try:
            fields.append(f"{r}={cpu(pids[r])}/{hwm(pids[r])}")
            ffields.append(f"{r}={faults(pids[r])}")
        except (OSError, ValueError, IndexError):
            pass
    ap, ab, lp, lb = net()
    out.write(f"S {t} all_pkts={ap} all_bytes={ab} lo_pkts={lp} lo_bytes={lb} " + " ".join(fields) + "\n")
    if ffields:
        out.write(f"F {t} " + " ".join(ffields) + "\n")
    out.flush()
    time.sleep(period)
out.close()
PY

ROUTER=""
if [ "$ARM" = zenoh ]; then
    # rmw_zenoh_cpp's shipped session config connects to a router on tcp/localhost:7447; the router runs on its
    # shipped config too, as its own process, started directly (not through the `ros2 run` wrapper, whose PID is not
    # the daemon's). Its CPU and memory are part of zenoh's figures.
    ROUTER=router
    launch router "$ZENOHD"
    for _ in $(seq 1 50); do
        ss -ltn 2>/dev/null | grep -q ':7447 ' && break
        sleep 0.1
    done
    if ss -ltn 2>/dev/null | grep -q ':7447 '; then meta "router=listening"; else meta "router=not_listening"; fi
fi

if [ "$KIND" = rtt ]; then
    flag=""
    [ "$QOS" = reliable ] && flag="--reliable"
    waitargs="--wait $WAIT"
    [ "$WAIT" = poll ] && waitargs="$waitargs $POLL_ARGS"
    python3 "$CELL_DIR/sampler.py" "$CELL_DIR" "$SAMPLE_S" "ping,pong${ROUTER:+,router}" &
    sampler=$!
    meta "net_before $(netdev)"
    # shellcheck disable=SC2086 # flag is deliberately word-split
    launch pong "$PINGPONG/pong_node" $flag -m "$MSG"
    sleep 4
    alive pong pong_node && identity pong
    # shellcheck disable=SC2086 # flag and waitargs are deliberately word-split
    launch ping timeout -s INT -k 5 $((${DUR%.*} + 40)) "$PINGPONG/ping_node" -i "$RTT_I" -d "$DUR" $flag $waitargs \
        --stamps "$CELL_DIR/stamps.txt" -m "$MSG"
    # timeout execs nothing: the ping is its child. Found by parentage from timeout's own PID, checked by exe.
    tpid=$(cat "$CELL_DIR/ping.lpid")
    for _ in $(seq 1 50); do
        c=$(cat "/proc/$tpid/task/$tpid/children" 2>/dev/null)
        for q in $c; do
            case "$(readlink "/proc/$q/exe" 2>/dev/null)" in */ping_node) echo "$q" >"$CELL_DIR/ping.pid.real" ;; esac
        done
        [ -s "$CELL_DIR/ping.pid.real" ] && break
        sleep 0.1
    done
    if [ -s "$CELL_DIR/ping.pid.real" ]; then
        mv "$CELL_DIR/ping.pid.real" "$CELL_DIR/ping.pid"
        meta "ping_pid=$(pid_of ping) via_timeout=$tpid"
        sleep 2
        identity ping
    else
        meta "ping_pid=none"
    fi
    while [ -d "/proc/$tpid" ]; do sleep 0.2; done
    meta "net_after $(netdev)"
    meta "pong_hwm_kb=$(awk '/VmHWM/ {print $2}' "/proc/$(pid_of pong)/status" 2>/dev/null)"
    stop pong pong_node
else
    common=(-c ROS2 -t "$MSG" --dds_domain_id "$DOMAIN")
    if [ "$QOS" = reliable ]; then
        common+=(--reliable --history_depth 1000) # KEEP_ALL is perf_test's default history; the depth is echoed
    else
        common+=(--keep_last --history_depth 1) # docs/TESTING.md section 5 rule 1: KEEP_LAST 1, the DDS default
    fi
    rosargs=(--ros-args --param start_type_description_service:=false)
    python3 "$CELL_DIR/sampler.py" "$CELL_DIR" 0.5 "pub,sub${ROUTER:+,router}" &
    sampler=$!
    launch sub "$PERF_TEST" "${common[@]}" -p 0 -s 1 --expected_num_pubs 1 --max_runtime $((DUR + 6)) "${rosargs[@]}"
    sleep 2
    alive sub perf_test && identity sub
    meta "net_before $(netdev)"
    launch pub "$PERF_TEST" "${common[@]}" -r 0 -p 1 -s 0 --expected_num_subs 1 --max_runtime "$DUR" "${rosargs[@]}"
    sleep 2
    alive pub perf_test && identity pub
    p=$(pid_of pub)
    for _ in $(seq 1 $((DUR * 5 + 200))); do
        [ -d "/proc/$p" ] || break
        sleep 0.2
    done
    meta "net_after $(netdev)"
    alive pub perf_test && { meta "pub_overran=1"; stop pub perf_test; }
    s=$(pid_of sub)
    for _ in $(seq 1 100); do
        [ -d "/proc/$s" ] || break
        sleep 0.2
    done
    alive sub perf_test && { meta "sub_overran=1"; stop sub perf_test; }
fi
[ -n "$ROUTER" ] && stop router rmw_zenohd
touch "$CELL_DIR/sampler.stop"
wait "$sampler" 2>/dev/null
meta "leftovers_after=$(count_exe ping_node pong_node perf_test rmw_zenohd)"
meta "done=1"

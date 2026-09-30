#!/usr/bin/env bash
# zenoh_cells.sh - zenoh-pico's cells, measured with a subscriber that is actually alive, outside the DDS QoS gate.
#
# WHY THIS EXISTS RATHER THAN A CAMPAIGN CELL. campaign_sweep.sh voids any row whose RESULT line does not echo the QoS
# it was asked for (keep_all=, max_blocking_ms= and the rest). Those are DDS QoS names; zenoh-pico has no such
# settings and cannot echo them, so every zenoh reliable row voids with VOID(qos...) while the measurement underneath
# it is perfectly good. The gate is right for the three DDS-shaped frameworks and wrong for this one, so the cell is
# run here instead of being smuggled past the gate.
#
# AND A SECOND REASON, found 2026-09-29 while fixing the first. run_scenario.sh forwards the same "$@" to both sides,
# so -d 5 gives the SERVER a five-second lifetime while the client spends three of them settling and then measures for
# five: the subscriber is gone before the measured window opens. Both earlier zenoh runs were made that way and both
# had server received=0. Over TCP that was loud - no session, so the publisher discarded locally and counted
# 15,326,932 puts at 1,863 Mbps over a 1 Gbps link with 745 bytes on the wire. Over multicast it was SILENT: the
# publisher transmits to the group whether or not anyone listens, so every client-side number looked plausible while
# the row's loss_pct=0.000 was computed as 0 lost of 0 received. A loss rate divided out of nothing reads exactly like
# a perfect one. So this script starts the server itself, with DUR+40, and every rep prints sent beside received.
#
# WHAT IS BEING MEASURED, and it is not the same mechanism as ours. zenoh-pico's Z_RELIABILITY_RELIABLE means
# monotonic sequence numbers, not retransmission - its own source says so on both transports (ZENOH_PICO_PLAN.md
# section 1). Over TCP the *transport* supplies the reliability instead, which is why this cell is TCP peer-to-peer
# (subscriber listens, publisher connects, no router) and why the RESULT line is required to state
# transport=tcp. The user's instruction, 2026-09-29: measure zenoh's reliable performance in the configuration where
# its reliability is real. So this compares two different mechanisms that make the same promise to an application -
# TCP's stream repair against DDS's and ours - rather than comparing like with like, and the row must say so.
#
# HOW TO READ IT, written before the run:
#   - zenoh-pico is REFERENCE MATERIAL, not a competitor (the user's decision, 2026-09-29). There is no pass or fail
#     here and no win or loss: a cell where it beats us is information. The only failure available is a measurement
#     that does not mean what it says.
#   - Any rep whose RESULT line lacks transport=tcp is VOID: it means the TCP endpoints were not applied and the run
#     silently fell back to multicast, which would be a best-effort number wearing a reliable label - the single most
#     misleading thing this script could produce.
#   - Any rep with sent < 100000 is VOID, as in every other harness here.
#   - Fewer than 3 usable reps and the cell is VOID and reported as such, not averaged over what survived.
#   - The DDS figures for the same cell are NOT re-measured here; they come from the campaign that has them. TickLE's
#     figure for this cell is deliberately absent, because on this commit it is still subject to the shared-memory
#     attach regression - a number taken now would understate it by half and would be worse than no number.
# Usage: zenoh_reliable_cell.sh [REPS] [DUR]      Output: $OUT (default /tmp/zenoh_reliable_cell.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
REPS=${REPS:-${1:-3}}
DUR=${DUR:-${2:-5}}
SCENS=${SCENS:-"reliable_throughput best_effort_throughput"}
SIZES=${SIZES:-"p1 p2 p3 p4"}
# Extra client arguments, for the latency cells: -i sets the ping interval, and the round trip is the measurement
# rather than the rate, so those cells run -i 0.1 -d 10 the way the other three frameworks' latency cells do.
CLI_ARGS=${CLI_ARGS:-}
# Network shaping, same conditions and the same discipline as campaign_sweep.sh: applied on the client's eth0, read
# back rather than trusted to an exit status (tc qdisc del legitimately fails when there is nothing to delete), and
# cleared by an EXIT trap. A run that starts with netem already on the interface refuses, because silently clearing
# it would hide that an earlier run died holding the rig shaped.
NET=${NET:-N0}
OUT=${OUT:-/tmp/zenoh_cells.txt}
# Per-rep diagnostics from BOTH sides, kept because this harness used to throw them away: it captured the run with
# `./client ... 2>&1 | grep '^RESULT'`, so anything that was not a RESULT line died at the far end of the pipe and
# never crossed the network. Keeping them is necessary but NOT sufficient, and the difference cost an hour to find:
#   - zenoh-pico 1.10.1 compiles every one of its own log calls OUT unless the library is built with -DZENOH_DEBUG,
#     which our measurement build does not pass. So in a measurement build there is nothing to keep.
#   - what logging it does have prints with printf, i.e. to STDOUT, so "keep stderr" would have been the wrong fix
#     even in a build that logs. This keeps the whole stream, both descriptors, in a file.
#   - our own client writes nothing to stderr at all; write_fail is our counter and is already in the RESULT line.
# To ask WHY a link dies, run against a diagnostic build, which has a prefix of its own so its numbers can never be
# mistaken for measurements (see zenohpico/build.sh):
#   ZENOH_DEBUG=1 ZENOH_PICO_PREFIX=$HOME/zenohpico_install_dbg ZENOH_PICO_SRC=$HOME/zenohpico_src_dbg
# Start at level 1 (errors only): a closing link is an error, and level 3 puts a printf on the data path, which
# changes the thing being measured. Escalate only if level 1 says nothing.
DIAG=${DIAG:-/tmp/zenoh_cells_diag}
# Forwarded verbatim to build.sh ON THE RIG HOSTS. Without this a ZENOH_DEBUG set here stays here: the build runs
# over ssh and ssh carries no environment, so the diagnostic build would silently be an ordinary one - a run that
# looks like the experiment and cannot answer it. For the diagnostic arm:
#   ZBUILD_ENV="ZENOH_DEBUG=1 ZENOH_PICO_PREFIX=\$HOME/zenohpico_install_dbg ZENOH_PICO_SRC=\$HOME/zenohpico_src_dbg"
# Note that the scenario binaries themselves are rebuilt into the same ${SCEN}_${SIZE} directory either way, so
# after a diagnostic run the next measurement run must rebuild them - which build.sh does unconditionally, and the
# prefix marker refuses to serve the wrong logging level, so a stale logging library cannot be linked unnoticed.
ZBUILD_ENV=${ZBUILD_ENV:-}
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
tc_netem_present() { sh_ "$CLIENT" "tc qdisc show dev eth0" 2>/dev/null | grep -q netem; }
tc_apply() {
    case "$1" in
    N0) sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
        if tc_netem_present; then
            echo "FATAL: netem still on $CLIENT eth0 after del - the rig is left shaped" >&2
            sh_ "$CLIENT" "tc qdisc show dev eth0" >&2 || true
            return 1
        fi ;;
    N1) sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss 5%" ;;
    N2) sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem delay 10ms 2ms" ;;
    N3) sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem delay 1ms reorder 5% 50%" ;;
    *)  echo "unknown network condition $1" >&2; return 1 ;;
    esac
}
tc_describe() {
    case "$1" in
    N0) echo "none" ;; N1) echo "loss 5%" ;; N2) echo "delay 10ms jitter 2ms" ;; N3) echo "delay 1ms reorder 5%" ;;
    esac
}
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
mkdir -p "$DIAG"
if [ "$NET" != N0 ] && tc_netem_present; then
    say "REFUSING TO START: $CLIENT eth0 already has netem on it, so the baseline would not be unshaped."
    sh_ "$CLIENT" "tc qdisc show dev eth0" | tee -a "$OUT"
    say "A previous run was probably SIGKILLed. Clear it with: sudo -n tc qdisc del dev eth0 root"
    exit 1
fi
if [ "$NET" != N0 ]; then
    tc_apply "$NET" || { say "FATAL could not apply $NET"; exit 1; }
    tc_netem_present || { say "FATAL $NET was applied but no netem is on the interface"; exit 1; }
fi
say "  network: $NET ($(tc_describe "$NET"))"
say "=== zenoh-pico cells $(date -Is) reps=$REPS dur=$DUR scens='$SCENS' sizes='$SIZES' zbuild_env='$ZBUILD_ENV' ==="

for SCEN in $SCENS; do
  for SIZE in $SIZES; do
    for h in "$CLIENT" "$SERVER"; do
        out=$(sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard origin/main
cd examples/perf_hil/zenohpico && env $ZBUILD_ENV ./build.sh $SCEN $SIZE > /tmp/zp_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/zp_build.log; exit 0; }
sha256sum ${SCEN}_${SIZE}/client | cut -c1-16" </dev/null 2>&1)
        # The whole output, not tail -1. build.sh's failure path echoes BUILD_FAILED and THEN five lines of the
        # compiler's diagnostics, so taking the last line took "compilation terminated." and the marker was never
        # seen: the guard could not fire, and nine reps ran against binaries that did not exist. Third time tonight
        # for this shape, and the first two were other people's.
        case "$out" in *BUILD_FAILED*|*"No such file"*|*error:*)
            say "FATAL build failed for $SCEN $SIZE on $h:"; say "$out"; exit 1;;
        esac
        out=$(printf '%s' "$out" | tail -1)
        case "$out" in [0-9a-f][0-9a-f]*) ;; *) say "FATAL no sha256 for $SCEN $SIZE on $h: $out"; exit 1;; esac
        say "  $SCEN $SIZE built on $h, client sha256=$out"
    done
  done
done

# The server is started DIRECTLY rather than through run_scenario.sh, and with a far longer life than the client.
# run_scenario.sh forwards the same "$@" to both sides, so -d 5 gives the server a five-second lifetime while the
# client spends three of those settling and then measures for five - the server is gone before the measured window
# opens. First run through run_scenario.sh: the client reported sent=15,326,932 at 1,863 Mbps over a 1 Gbps link
# while wire_tx_bytes was 745. A publisher with no matching subscriber discards locally and still counts the put.
# p1_layout_check.sh already gives its server DUR+40 for this reason; this does the same.
SRV_DUR=$((DUR + 40))
srv_pid=""
kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    sh_ "$SERVER" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in *zenohpico/*/server*) kill -INT $srv_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" </dev/null >/dev/null
    srv_pid=""
}
# ONE EXIT trap for the whole script. bash REPLACES an EXIT handler rather than appending to it, so when this
# `trap kill_server EXIT` was added below a `trap tc_apply N0 EXIT` set earlier, the netem restore was silently
# discarded and a completed run left the rig shaped at delay 10ms - which would have corrupted every later
# measurement on it, ours and CI's alike. The next two runs refused to start because of it, which is the only
# reason it was noticed within the minute. Everything that must happen on the way out happens here.
on_exit() {
    kill_server
    if [ "$NET" != N0 ]; then
        tc_apply N0 || echo "RIG LEFT SHAPED - clear it before any further measurement" >&2
    fi
}
trap on_exit EXIT

for SCEN in $SCENS; do
  for SIZE in $SIZES; do
    say "### $SCEN $SIZE ###"
    # Endpoints only for the reliable cell: that one is TCP peer-to-peer, which is the only configuration where
    # zenoh-pico's reliability is real. Best-effort stays on UDP multicast, where both sides promise the same thing.
    # Any reliable_* cell, not just the throughput one: matching the exact string left reliable_latency with no
    # endpoints, and the harness then refused to open rather than quietly running the cell over multicast and
    # labelling it reliable. The refusal was right; the condition was wrong.
    case "$SCEN" in
    reliable_*) ;;
    *) SRV_ENV=""; CLI_ENV="" ;;
    esac
    if [ "${SCEN#reliable_}" != "$SCEN" ]; then
        SRV_ENV="BENCH_ZENOH_LISTEN=tcp/192.168.10.2:7447"
        CLI_ENV="BENCH_ZENOH_CONNECT=tcp/192.168.10.2:7447"
    else
        SRV_ENV=""; CLI_ENV=""
    fi
    for r in $(seq 1 "$REPS"); do
        say "--- $SCEN rep $r/$REPS $(date -Is) ---"
        kill_server
        srv_pid=$(sh_ "$SERVER" "cd ~/tickle/examples/perf_hil/zenohpico/${SCEN}_${SIZE} && rm -f /tmp/zrc_server.pid
(setsid sh -c 'echo \$\$ > /tmp/zrc_server.pid; exec env $SRV_ENV taskset -c 1-3 stdbuf -oL ./server -d $SRV_DUR' > /tmp/zrc_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/zrc_server.pid" </dev/null)
        cline=$(sh_ "$CLIENT" "cd ~/tickle/examples/perf_hil/zenohpico/${SCEN}_${SIZE} && env $CLI_ENV taskset -c 1-3 stdbuf -oL ./client -d $DUR $CLI_ARGS > /tmp/zp_client.log 2>&1; grep '^RESULT' /tmp/zp_client.log" </dev/null)
        [ -n "$cline" ] && say "scen=$SCEN size=$SIZE net=$NET rep$r $cline"
        kill_server
        sline=$(sh_ "$SERVER" "grep '^RESULT' /tmp/zrc_server.log | tail -1" </dev/null)
        [ -n "$sline" ] && say "scen=$SCEN size=$SIZE net=$NET rep$r $sline"
        say "  $SCEN $SIZE rep$r client sent=$(printf '%s' "$cline" | grep -oE 'sent=[0-9]+' | cut -d= -f2) server received=$(printf '%s' "$sline" | grep -oE 'received=[0-9]+' | cut -d= -f2)"
        # Both sides' own words, retained per rep. A failure to READ a log gets its own line: "I could not look" and
        # "the run said nothing" are different findings, and if they print the same the absence of a diagnosis reads
        # as the absence of a problem.
        for side in client server; do
            case $side in
            client) dh=$CLIENT; rlog=/tmp/zp_client.log ;;
            server) dh=$SERVER; rlog=/tmp/zrc_server.log ;;
            esac
            dlog="$DIAG/${SCEN}_${SIZE}_${NET}_rep${r}_${side}.log"
            if ! sh_ "$dh" "cat $rlog" </dev/null >"$dlog" 2>/dev/null; then
                say "  !! could not read the $side log ($dh:$rlog): this rep has NO diagnostics, which is a hole in the"
                say "     evidence and not a quiet run. Do not read its RESULT line as unexplained."
                continue
            fi
            nd=$(grep -cv '^RESULT' "$dlog" 2>/dev/null || true); nd=${nd:-0}
            if [ "$nd" -eq 0 ]; then
                say "  $side said nothing beyond its RESULT line ($dlog)"
            else
                say "  $side said $nd line(s) besides RESULT; last 12 follow, whole log in $dlog"
                grep -v '^RESULT' "$dlog" | tail -12 | sed 's/^/    | /' | tee -a "$OUT"
            fi
        done
    done
  done
done
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, math, collections, statistics as st

# Client and server rows are paired by (scenario, rep), because the check this script exists for is that the
# subscriber was ALIVE: a client row on its own cannot show that, and both earlier zenoh runs proved it by looking
# entirely healthy on the client side with received=0 on the server.
cli, srv = {}, {}
for line in open(sys.argv[1]):
    m = re.match(r"scen=(\S+) size=(\S+) net=(\S+) rep(\d+) (RESULT:.*)", line.strip())
    if not m:
        continue
    scen = "%s %s %s" % (m.group(1), m.group(2), m.group(3))
    rep, rest = int(m.group(4)), m.group(5)
    f = dict(kv.split("=", 1) for kv in rest.split() if "=" in kv)
    (cli if f.get("role") == "client" else srv)[(scen, rep)] = f

def num(f, k, d=0.0):
    try:
        return float(str(f.get(k, d)).replace(",", ""))
    except ValueError:
        return d

rows = collections.defaultdict(list)
for key, c in sorted(cli.items()):
    scen, rep = key
    tag = "%s rep%d" % (scen, rep)
    sent = num(c, "sent")
    s_row = srv.get(key)
    if sent < 100000 and "latency" not in scen:
        print("VOID %s: sent=%d below 100000" % (tag, sent)); continue
    # The two checks that read the OUTCOME rather than the configuration. transport=tcp says what the harness was
    # ASKED for; these say what happened. The first run of this script counted 15,326,932 puts at 1,863 Mbps over a
    # 1 Gbps link with 745 bytes on the wire, and its line still said transport=tcp.
    latency = "latency" in scen
    if latency:
        # A latency cell sends ~100 pings, not 100000, and its pass condition is that the pongs came back.
        got = num(c, "recv")
        if got <= 0.0:
            print("VOID %s: recv=0 - no pong returned, so there is no round trip to report" % tag); continue
        c["_recv"] = got
        c["_loss_pct"] = num(c, "loss_pct")
        rows[scen].append(c)
        continue
    if num(c, "wire_bytes_per_sample") <= 0.0:
        print("VOID %s: wire_bytes_per_sample=0 - the samples never reached the wire" % tag); continue
    if num(c, "send_mbps") > 1000.0:
        print("VOID %s: send_mbps=%.1f exceeds the rig's 1 Gbps link" % (tag, num(c, "send_mbps"))); continue
    if s_row is None:
        print("VOID %s: no server row - nothing proves a subscriber was there" % tag); continue
    recv = num(s_row, "received")
    if recv <= 0.0:
        print("VOID %s: server received=0. The publisher measured itself talking to nobody, and over multicast that"
              " looks exactly like a healthy run - loss_pct is then 0 lost of 0 received." % tag); continue
    if scen.startswith("reliable_throughput") and c.get("transport") != "tcp":
        print("VOID %s: transport=%s, not tcp" % (tag, c.get("transport"))); continue
    c["_recv"] = recv
    c["_loss_pct"] = 100.0 * (sent - recv) / sent if sent else 0.0
    rows[scen].append(c)

def se(v):
    return 0.0 if len(v) < 2 else math.sqrt(st.pvariance(v) * len(v) / (len(v) - 1) / len(v))

for scen in sorted(rows):
    rs = rows[scen]
    print("\n=== zenoh-pico %s p1, %d usable reps ===" % (scen, len(rs)))
    if len(rs) < 3:
        print("VOID: fewer than 3 usable reps. The cell is not reported."); continue
    print("  transport=%s reliability=%s express=%s" % (rs[0].get("transport", "udp/multicast"),
          rs[0].get("reliability", "best_effort"), rs[0].get("express", "?")))
    if "latency" in scen:
        for k, label in (("rtt_avg_ms", "RTT mean ms"), ("rtt_max_ms", "RTT max ms"),
                         ("rtt_min_ms", "RTT min ms"), ("cpu_s_per_Msample", "CPU s/Msample"),
                         ("peak_rss_kb", "peak RSS kB"), ("_loss_pct", "loss %")):
            v = [num(f, k) for f in rs if k in f]
            if v:
                print("  %-18s %10.3f +- %.3f" % (label, st.mean(v), se(v)))
        print("  sent/recv: " + ", ".join("%d/%d" % (num(f, "sent"), f["_recv"]) for f in rs))
        continue
    for k, label in (("send_mbps", "send Mbps"), ("cpu_s_per_Msample", "CPU s/Msample"),
                     ("peak_rss_kb", "peak RSS kB"), ("wire_bytes_per_sample", "wire B/sample"),
                     ("wire_packets_per_sample", "wire pkts/sample"), ("_loss_pct", "loss %")):
        v = [num(f, k) for f in rs if k in f]
        if v:
            print("  %-18s %10.3f +- %.3f" % (label, st.mean(v), se(v)))
    print("  sent/received: " + ", ".join("%d/%d" % (num(f, "sent"), f["_recv"]) for f in rs))
print("\n  Reference material, not a competitor: no win or loss is claimed from these rows (user, 2026-09-29).")
print("  The reliable cell's reliability is TCP's stream repair, not per-datagram retransmission - a different")
print("  mechanism making the same promise, which the COMPARISON row must state.")
PYEOF

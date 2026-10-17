#!/usr/bin/env bash
# One publisher, two subscribers: one on the publisher's own host (the segment), one on the other Pi (the network).
# The user's question of 2026-10-05: does a stream that goes both ways at once stay correct, and what does each
# subscriber get compared with having the publisher to itself?
#
# Arms, rotated within every rep so drift lands on all of them:
#   MIXED        client + local subscriber on 10.1.1.214, remote subscriber on 10.1.1.213
#   LOCAL_ONLY   client + local subscriber only          (the shared-memory control)
#   REMOTE_ONLY  client + remote subscriber only          (the network control)
#   MIXED_NET    CycloneDDS only: MIXED with the local subscriber's shared memory switched off (the witness's control)
#
# Reading rules, written before the run and enforced below:
#   WITNESS (MIXED) - the client's tx_shm AND tx_udp are both > 0, the local subscriber's rx_shm share is >= 0.99 and the
#     remote one's rx_udp share >= 0.99. Otherwise the arm did not take the two paths it is named for: VOID.
#   CORRECTNESS - RELIABLE: every subscriber post_match_lost=0 and the client write_fail=0. BEST_EFFORT: each
#     subscriber's loss is reported beside its rate; a BEST_EFFORT loss is a result, not a failure.
#     post_match_lost, not lost: `lost` also counts the samples published before that subscriber was matched
#     (prematch_window), which a VOLATILE subscriber never gets by design. The first run read MIXED's local subscriber
#     as losing 70-75 RELIABLE samples per rep; all of them were prematch (first_seq=76) - the publisher started on the
#     remote subscriber's match and the local one matched a moment later. Reported apart, not counted as loss.
#   DELIVERY (added 2026-10-17, the reader rule of campaign_summary.py's delivery_verdict) - RELIABLE: every
#     subscriber's recv + prematch must equal the client's sent=, in every rep. A gap count alone cannot see samples
#     that never arrived after the last one that did. A framework failing it is listed as DELIVERY FAILED with its
#     figures, not scored. BEST_EFFORT is never flagged: dropping is what it promises.
#   GOODPUT (2026-10-17, campaign_summary.py G1-G3) - each subscriber's win_recv_mbps, the complete samples it took
#     inside the measured window of the sender's clock, printed beside the delivered rate when the bench reports a
#     window. Used only if the server's window=ok and its length matches the client's to one 2^22 ns bucket; otherwise
#     that rep's goodput is VOID and said. Files from benches without a window (2026-10-05) have the rate only.
#   PERFORMANCE - each MIXED subscriber's delivered rate against the same subscriber alone (LOCAL_ONLY / REMOTE_ONLY):
#     a difference counts only if the ranges do not overlap.
#   A rep where any expected RESULT line is missing is VOID and said, never read as zero.
#
# FRAMEWORK=fastdds runs the same three arms with FastDDS as shipped (data-sharing) plus its shared-memory and eth0 UDP
# transports (fastdds_shm_and_eth0.xml). It has no tx_shm/tx_udp counters, so the WITNESS rule is TickLE's only; for
# FastDDS the remote subscriber's eth0 packets per received sample (wire_rx_packets / recv) are printed instead, and must
# be > 0. FastDDS's subscriber uses the default reader QoS (KEEP_LAST 1), so on one host a fast writer overwrites what the
# reader has not taken yet: its delivered rate, not the client's send rate, is what this harness reports.
#
# FRAMEWORK=cyclonedds (2026-10-17, ROADMAP item 8): the CycloneDDS benches of examples/perf_hil/cyclonedds, with
# shared memory as the same-host cells run it (s6_transport_cells.sh's ON arm): iceoryx's iox-roudi on the publisher's
# host, taken from beside the libddsc the client's own RPATH names, and CYCLONEDDS_URI with SharedMemory enabled and
# eth0 as the network interface for the client and the local subscriber. The remote subscriber has no daemon and runs
# the network-only URI. Each URI is written to a file on the Pi and passed as file://, so no XML is quoted through ssh.
# A fresh RouDi is started for every arm (all four, so the publisher's configuration is the same in each) and stopped
# after it. The client gets --readers 2 in the two-subscriber arms: it starts writing only when both are matched
# (common.h), so no subscriber has a prematch window by construction; the servers print first_seq= and first_seq - 1
# is still reported as prematch should it ever be non-zero. Both servers are stopped with SIGINT 1 s after the client
# returns (the client's own drain has waited for acknowledgements by then); their own idle cap would cost 15 s per arm.
# CycloneDDS prints nothing that says which transport carried a sample, so its witness is measured from outside:
#   ROUDI - RouDi reported ready before the arm's servers started and was alive when the client returned; otherwise
#     the arm may have run without shared memory: VOID.
#   LOCAL PATH - the publisher host's IP InOctets (/proc/net/netstat IpExt, every IP datagram the host received,
#     loopback and looped-back multicast included) over the client's run, divided by the local subscriber's
#     recv x sample_bytes. <= 0.25 reads as shared memory, >= 0.75 as the network, between as neither. MIXED and
#     LOCAL_ONLY must read shared memory: otherwise VOID. The remote subscriber's eth0 packets per sample must be > 0
#     in MIXED, as for FastDDS.
#   CONTROL - MIXED_NET runs the local subscriber with the network-only URI, so it must read >= 0.75. If any MIXED_NET
#     rep reads below that, or none is usable, the instrument has not been shown to see a network delivery and every
#     CycloneDDS MIXED rep is VOID. PC smoke in two netns (2026-10-17, lyrical's 11.0.1): MIXED 0.0003 and 0.00003,
#     MIXED_NET 1.07 and 1.10.
#   READERS - the client's readers_at_start must equal readers_wanted: otherwise VOID.
#
# Usage: SHA=<sha> FRAMEWORK=tickle|fastdds|cyclonedds SCEN=best_effort_throughput SIZE=p2 CLI_ARGS="" REPS=3 DUR=5 \
#        OUT=<file> mixed_delivery.sh
# ANALYSE_ONLY=1 OUT=<file> FRAMEWORK=... SCEN=... re-reads an existing file without touching the rig.
set -uo pipefail
# shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$(dirname "${BASH_SOURCE[0]}")/pi_checkout.sh"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
ANALYSE_ONLY=${ANALYSE_ONLY:-0}
export RIG_LOCK_SCOPE=hil
if [ "$ANALYSE_ONLY" != 1 ] && [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${SHA:-$(git -C "$REPO" rev-parse --short HEAD)}
FRAMEWORK=${FRAMEWORK:-tickle}
SCEN=${SCEN:-best_effort_throughput}
SIZE=${SIZE:-p2}
CLI_ARGS=${CLI_ARGS:-}
REPS=${REPS:-3}
DUR=${DUR:-5}
OUT=${OUT:-$HOME/rig_results_safe/mixed_delivery.txt}
K=$HOME/.ssh/tickle_ci_ed25519
PUB_HOST=10.1.1.214
REMOTE_HOST=10.1.1.213
SAVE=/tmp/mixdel_$FRAMEWORK
FDIR=/home/ci/tickle/examples/perf_hil/fastdds
FENV=""
[ "$FRAMEWORK" = fastdds ] && FENV="env BENCH_IFACE=eth0 LD_LIBRARY_PATH=/opt/ros/jazzy/lib FASTRTPS_DEFAULT_PROFILES_FILE=$FDIR/fastdds_shm_and_eth0.xml"
case "$FRAMEWORK" in tickle | fastdds | cyclonedds) ;; *) echo "FRAMEWORK='$FRAMEWORK': tickle, fastdds or cyclonedds" >&2; exit 2 ;; esac
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
say() { echo "$*" | tee -a "$OUT"; }

# ---------------------------------------------------------------- CycloneDDS: URIs, RouDi, the witness counter
CDDS_URI_OFF='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>'
CDDS_URI_ON='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery><SharedMemory><Enable>true</Enable></SharedMemory></Domain></CycloneDDS>'
declare -A CDDS_LIB=()
CDDS_ROUDI=""
roudi_pid=""
# The prefix each host's client RPATH names (build.sh bakes it in), and on the publisher's host the RouDi beside it -
# s6_transport_cells.sh's reasoning: the daemon, the psmx plugin and the library then match by construction.
cdds_prepare() { # cdds_prepare <host>
    local h=$1 lib
    lib=$(sh_ "$h" "readelf -d $SAVE/client 2>/dev/null | sed -n 's/.*RPATH.*\[\(.*\)\]/\1/p' | head -1" </dev/null)
    [ -n "$lib" ] || { say "FATAL the client on $h has no RPATH, so its CycloneDDS prefix cannot be derived"; return 1; }
    CDDS_LIB[$h]=$lib
    sh_ "$h" "printf '%s\n' '$CDDS_URI_ON' > $SAVE/cdds_on.xml && printf '%s\n' '$CDDS_URI_OFF' > $SAVE/cdds_off.xml" </dev/null ||
        { say "FATAL could not write the CycloneDDS URIs on $h"; return 1; }
    if [ "$h" = "$PUB_HOST" ]; then
        CDDS_ROUDI=${lib%/lib/*}/bin/iox-roudi
        sh_ "$h" "test -x $CDDS_ROUDI" </dev/null || { say "FATAL no iox-roudi at $CDDS_ROUDI on $h, beside the library the client links"; return 1; }
    fi
    say "  $h: client links $lib${CDDS_ROUDI:+; RouDi $CDDS_ROUDI}"
}
cenv() { # cenv <host> on|off: the environment a CycloneDDS process runs with
    echo "env LD_LIBRARY_PATH=${CDDS_LIB[$1]} BENCH_IFACE=eth0 CYCLONEDDS_URI=file://$SAVE/cdds_$2.xml"
}
roudi_on() {
    roudi_pid=$(sh_ "$PUB_HOST" "rm -f /tmp/mixdel_roudi.pid /tmp/mixdel_roudi.log
(setsid sh -c 'echo \$\$ >/tmp/mixdel_roudi.pid; exec env LD_LIBRARY_PATH=${CDDS_LIB[$PUB_HOST]} $CDDS_ROUDI' >/tmp/mixdel_roudi.log 2>&1 </dev/null &)
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q 'RouDi is ready for clients' /tmp/mixdel_roudi.log 2>/dev/null && break; sleep 1; done
cat /tmp/mixdel_roudi.pid" </dev/null)
    if ! sh_ "$PUB_HOST" "grep -q 'RouDi is ready for clients' /tmp/mixdel_roudi.log" </dev/null; then
        say "  RouDi did not report ready. Its log said:"
        sh_ "$PUB_HOST" "tail -5 /tmp/mixdel_roudi.log" </dev/null 2>/dev/null | sed 's/^/        | /' | tee -a "$OUT"
        roudi_off
        return 1
    fi
}
roudi_alive() { # by the pid it wrote and the executable behind it, never by name
    [ -n "$roudi_pid" ] && [ "$(sh_ "$PUB_HOST" "readlink /proc/$roudi_pid/exe 2>/dev/null" </dev/null)" = "$CDDS_ROUDI" ]
}
roudi_off() {
    [ -z "${roudi_pid:-}" ] && return 0
    sh_ "$PUB_HOST" "case \"\$(readlink /proc/$roudi_pid/exe 2>/dev/null)\" in */iox-roudi) kill -TERM $roudi_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$roudi_pid ] || break; sleep 1; done; true" </dev/null >/dev/null 2>&1
    roudi_pid=""
}
# Every IP octet the host received (IpExt InOctets), read on the host in the same shell as the client.
IN_OCTETS="awk '/^IpExt:/ {if (!h) {for (i = 1; i <= NF; i++) n[\$i] = i; h = 1} else print \$n[\"InOctets\"]}' /proc/net/netstat"

stop_servers() { # by the executable under $SAVE, never by pattern
    for h in "$PUB_HOST" "$REMOTE_HOST"; do
        sh_ "$h" "for p in /proc/[0-9]*; do case \"\$(readlink \$p/exe 2>/dev/null)\" in $SAVE/server) kill -INT \${p#/proc/};; esac; done" \
            </dev/null >/dev/null 2>&1
    done
}
on_exit() {
    stop_servers
    roudi_off
}

if [ "$ANALYSE_ONLY" != 1 ]; then
trap on_exit EXIT
: >"$OUT"
say "=== mixed delivery $(date -Is) sha=$SHA framework=$FRAMEWORK scen=$SCEN size=$SIZE cli_args='$CLI_ARGS' reps=$REPS dur=${DUR}s ==="
for h in "$PUB_HOST" "$REMOTE_HOST"; do
    out=$(sh_ "$h" "set -e; cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/$FRAMEWORK && ./build.sh $SCEN $SIZE > /tmp/mixdel_build.log 2>&1 || { echo BUILD_FAILED; tail -3 /tmp/mixdel_build.log; exit 0; }
mkdir -p $SAVE && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/ && sha256sum $SAVE/client | cut -c1-16" </dev/null 2>&1 | tail -1)
    case "$out" in *BUILD_FAILED* | *FAILED* | *"No such file"*) say "FATAL build on $h: $out"; exit 1 ;; esac
    say "  built on $h: client sha256=$out"
    if [ "$FRAMEWORK" = cyclonedds ]; then cdds_prepare "$h" || exit 1; fi
done

start_server() { # start_server <host> <cpus> <tag> [on|off]
    local envs=$FENV
    [ "$FRAMEWORK" = cyclonedds ] && envs=$(cenv "$1" "$4")
    sh_ "$1" "cd $SAVE && rm -f /tmp/mixdel_$3.log && (setsid sh -c 'exec $envs taskset -c $2 ./server $CLI_ARGS -d $((DUR + 6))' > /tmp/mixdel_$3.log 2>&1 < /dev/null &)" </dev/null
}
read_server() { # read_server <host> <tag>
    sh_ "$1" "for i in \$(seq 1 25); do grep -q '^RESULT' /tmp/mixdel_$2.log && break; sleep 1; done; grep '^RESULT' /tmp/mixdel_$2.log" </dev/null
}

run_arm() { # run_arm <arm> <rep>
    local arm=$1 rep=$2 c l r envs=$FENV extra="" all roudi=none
    stop_servers
    if [ "$FRAMEWORK" = cyclonedds ]; then
        if ! roudi_on; then
            say "arm=$arm rep=$rep who=witness roudi=not_ready"
            return 0
        fi
        envs=$(cenv "$PUB_HOST" on)
        case "$arm" in MIXED | MIXED_NET) extra="--readers 2" ;; *) extra="--readers 1" ;; esac
    fi
    case "$arm" in
    MIXED | LOCAL_ONLY) start_server "$PUB_HOST" 1 local on ;;
    MIXED_NET) start_server "$PUB_HOST" 1 local off ;;
    esac
    case "$arm" in MIXED | MIXED_NET | REMOTE_ONLY) start_server "$REMOTE_HOST" 1-3 remote off ;; esac
    sleep 2
    all=$(sh_ "$PUB_HOST" "cd $SAVE && o0=\$($IN_OCTETS) && $envs taskset -c 2 ./client $CLI_ARGS -d $DUR $extra > /tmp/mixdel_client.log 2>&1; o1=\$($IN_OCTETS)
grep '^RESULT' /tmp/mixdel_client.log; echo \"WITNESS in_octets=\$((o1 - o0))\"; grep -v '^RESULT' /tmp/mixdel_client.log | grep -v deprecated | tail -3 | sed 's/^/CLIENT_SAID /'" </dev/null)
    c=$(printf '%s\n' "$all" | grep '^RESULT' | head -1)
    say "arm=$arm rep=$rep who=client ${c:-NORESULT}"
    [ -z "$c" ] && printf '%s\n' "$all" | grep '^CLIENT_SAID' | sed 's/^/       | /' | tee -a "$OUT"
    if [ "$FRAMEWORK" = cyclonedds ]; then
        roudi_alive && roudi=alive || roudi=dead
        sleep 1
        stop_servers # SIGINT: each server prints its RESULT and exits
        say "arm=$arm rep=$rep who=witness roudi=$roudi $(printf '%s\n' "$all" | sed -n 's/^WITNESS //p')"
    fi
    case "$arm" in MIXED | MIXED_NET | LOCAL_ONLY) l=$(read_server "$PUB_HOST" local); say "arm=$arm rep=$rep who=local ${l:-NORESULT}" ;; esac
    case "$arm" in MIXED | MIXED_NET | REMOTE_ONLY) r=$(read_server "$REMOTE_HOST" remote); say "arm=$arm rep=$rep who=remote ${r:-NORESULT}" ;; esac
    stop_servers
    roudi_off
}

ARMS="MIXED LOCAL_ONLY REMOTE_ONLY"
[ "$FRAMEWORK" = cyclonedds ] && ARMS="MIXED LOCAL_ONLY REMOTE_ONLY MIXED_NET"
for rep in $(seq 1 "$REPS"); do
    if [ "$FRAMEWORK" = cyclonedds ]; then # each arm in each position once over four reps
        read -ra a <<<"$ARMS"
        s=$(((rep - 1) % 4))
        order="${a[*]:s} ${a[*]:0:s}"
    else
        case $((rep % 3)) in
        1) order="MIXED LOCAL_ONLY REMOTE_ONLY" ;;
        2) order="LOCAL_ONLY REMOTE_ONLY MIXED" ;;
        *) order="REMOTE_ONLY MIXED LOCAL_ONLY" ;;
        esac
    fi
    for arm in $order; do run_arm "$arm" "$rep"; done
done
say "=== done $(date -Is) ==="
fi

if [ "$ANALYSE_ONLY" = 1 ]; then SINK=(cat); else SINK=(tee -a "$OUT"); fi
python3 - "$OUT" "$SCEN" "$FRAMEWORK" <<'PYEOF' | "${SINK[@]}"
import re, sys, statistics as st
path, scen = sys.argv[1], sys.argv[2]
framework = sys.argv[3] if len(sys.argv) > 3 else "tickle"
reliable = "reliable" in scen
BUCKET_S = (1 << 22) / 1e9
SHM_MAX, NET_MIN = 0.25, 0.75
rows, void = {}, []
for line in open(path):
    m = re.match(r"arm=(\S+) rep=(\d+) who=(\S+) (.*)", line.rstrip())
    if not m:
        continue
    arm, rep, who, rest = m.groups()
    if rest.startswith("NORESULT"):
        void.append(f"{arm} rep {rep}: no RESULT from {who}")
        continue
    f = dict(kv.split("=", 1) for kv in rest.split() if "=" in kv)
    rows.setdefault((arm, rep), {})[who] = f

def share(f, a, b):
    x, y = int(f.get(a, 0)), int(f.get(b, 0))
    return x / (x + y) if x + y else 0.0

def prematch(s):
    if "prematch_window" in s:
        return int(s["prematch_window"])
    if "first_seq" in s:                       # CycloneDDS: the samples before the first one it took
        return max(int(s["first_seq"]) - 1, 0)
    return 0

def local_ratio(w):
    """The CycloneDDS local-path witness: publisher-host IP octets over the local subscriber's payload octets."""
    s, wt = w.get("local"), w.get("witness", {})
    if s is None or "in_octets" not in wt:
        return None
    payload = int(s.get("recv", 0)) * int(s.get("sample_bytes", 0) or 0)
    return int(wt["in_octets"]) / payload if payload else None

def goodput(c, s):
    """The subscriber's windowed goodput (Mbps), or (None, why) - campaign_summary.py G3."""
    if "win_recv_mbps" not in s:
        return None, "no window in this bench"
    if s.get("window") != "ok":
        return None, f"server window={s.get('window')}"
    if "win_s" in c and abs(float(c["win_s"]) - float(s["win_s"])) > BUCKET_S:
        return None, f"window {s['win_s']} s against the client's {c['win_s']} s"
    return float(s["win_recv_mbps"]), ""

# The CycloneDDS witness's own control first: it decides whether any MIXED rep can be read.
witness_proven = True
if framework == "cyclonedds":
    net = []
    for (arm, rep), w in sorted(rows.items()):
        if arm != "MIXED_NET":
            continue
        r = local_ratio(w)
        roudi = w.get("witness", {}).get("roudi")
        print(f"  control (cyclonedds) MIXED_NET rep {rep}: publisher-host IP octets per local payload octet "
              f"{'n/a' if r is None else f'{r:.4f}'}  roudi={roudi}")
        if r is not None and roudi == "alive":
            net.append(r)
    if not net or min(net) < NET_MIN:
        witness_proven = False
        print(f"  CONTROL FAILED: MIXED_NET read {net} - the witness has not been shown to see a network delivery "
              f"(>= {NET_MIN}); every CycloneDDS MIXED rep is VOID")

stats, delivery, gp = {}, {}, {}
for (arm, rep), w in sorted(rows.items()):
    c = w.get("client")
    if c is None:
        if "witness" in w:                     # a CycloneDDS arm whose RouDi never came up runs no client at all
            void.append(f"{arm} rep {rep}: no client run (RouDi {w['witness'].get('roudi')})")
        continue
    el = float(c.get("elapsed_s", "0") or 0) or 1.0
    if arm in ("MIXED", "MIXED_NET") and framework != "tickle":
        # FastDDS's RESULT carries wire_rx_packets (the subscriber's own eth0 count), not a per-sample field - the
        # first run read a field that does not exist, got 0, and voided every MIXED rep for a reason that was ours.
        rf = w.get("remote", {})
        rp = int(rf.get("wire_rx_packets", "0") or 0) / max(int(rf.get("recv", "0") or 0), 1)
        print(f"  witness ({framework}) {arm} rep {rep}: remote eth0 packets per sample {rp:.3f}")
        if rp <= 0:
            void.append(f"{arm} rep {rep}: the remote subscriber saw no eth0 packets")
            continue
    if framework == "cyclonedds":
        wt = w.get("witness", {})
        if wt.get("roudi") != "alive":
            void.append(f"{arm} rep {rep}: RouDi {wt.get('roudi', 'not recorded')} - shared memory not shown")
            continue
        want, got = c.get("readers_wanted"), c.get("readers_at_start")
        if want is None or want != got:
            void.append(f"{arm} rep {rep}: client matched {got} of {want} readers at start")
            continue
        if arm in ("MIXED", "LOCAL_ONLY"):
            r = local_ratio(w)
            print(f"  witness (cyclonedds) {arm} rep {rep}: publisher-host IP octets per local payload octet "
                  f"{'n/a' if r is None else f'{r:.4f}'}")
            if r is None or r > SHM_MAX:
                void.append(f"{arm} rep {rep}: local path ratio {r} - not shared memory (<= {SHM_MAX})")
                continue
            if arm == "MIXED" and not witness_proven:
                void.append(f"MIXED rep {rep}: the witness's control (MIXED_NET) failed")
                continue
    if arm == "MIXED" and framework == "tickle":
        if not (int(c.get("tx_shm", 0)) > 0 and int(c.get("tx_udp", 0)) > 0):
            void.append(f"MIXED rep {rep}: tx_shm={c.get('tx_shm')} tx_udp={c.get('tx_udp')} - not both paths")
            continue
        if "local" in w and share(w["local"], "rx_shm", "rx_udp") < 0.99:
            void.append(f"MIXED rep {rep}: local rx_shm share {share(w['local'], 'rx_shm', 'rx_udp'):.3f}")
            continue
        if "remote" in w and share(w["remote"], "rx_udp", "rx_shm") < 0.99:
            void.append(f"MIXED rep {rep}: remote rx_udp share {share(w['remote'], 'rx_udp', 'rx_shm'):.3f}")
            continue
    sent = int(c.get("sent", 0))
    for who in ("local", "remote"):
        if who not in w:
            continue
        s = w[who]
        recv = int(s.get("recv", 0)); pre = prematch(s)
        lost = int(s.get("post_match_lost", s.get("lost", 0)))
        rate = recv / el / 1e3
        stats.setdefault((arm, who), []).append((rate, lost, float(s.get("loss_pct", 0)), int(c.get("write_fail", 0)), pre))
        delivery.setdefault((arm, who), []).append((recv, pre, sent))
        g, why = goodput(c, s)
        if g is None and why != "no window in this bench":
            void.append(f"{arm} rep {rep} {who}: goodput VOID - {why}")
        gp.setdefault((arm, who), []).append(g)
print()
if void:
    print("VOID reps:")
    for v in void:
        print("  " + v)
for (arm, who), v in sorted(stats.items()):
    r = [x[0] for x in v]
    g = [x for x in gp[(arm, who)] if x is not None]
    gtxt = f"  goodput {st.mean(g):8.1f} Mbps ({min(g):.1f}..{max(g):.1f}, n={len(g)})" if g else ""
    print(f"{arm:12} {who:6} n={len(v)} delivered {st.mean(r):8.1f} k/s ({min(r):.1f}..{max(r):.1f}){gtxt}"
          f"  post-match lost {[x[1] for x in v]}  prematch {[x[4] for x in v]}  loss% {[x[2] for x in v]}"
          f"  client write_fail {[x[3] for x in v]}")
    print(f"{'':12} {'':6} delivered of sent (recv+prematch / sent) {[f'{a}+{b}/{c}' for a, b, c in delivery[(arm, who)]]}")
print()
bad = [k for k, v in stats.items() if reliable and any(x[1] or x[3] for x in v)]
if reliable:
    print("CORRECTNESS: " + ("PASS - every RELIABLE subscriber lost 0 and the client gave up nothing" if not bad
                             else "FAIL - " + ", ".join(f"{a}/{w}" for a, w in sorted(bad))))
    short = sorted(k for k, v in delivery.items() if any(a + b != c for a, b, c in v))
    print("DELIVERY: " + ("PASS - every RELIABLE subscriber took every sample sent after its match, in every rep"
                          if not short else "DELIVERY FAILED - " + ", ".join(
                              f"{a}/{w} " + "; ".join(f"{x + y} of {z}" for x, y, z in delivery[(a, w)])
                              for a, w in short) + " (listed with its figures, not scored)"))
for who, alone in (("local", "LOCAL_ONLY"), ("remote", "REMOTE_ONLY")):
    a, b = stats.get(("MIXED", who)), stats.get((alone, who))
    if not a or not b or len(a) < 3 or len(b) < 3:
        print(f"{who}: fewer than 3 usable reps in MIXED or {alone} - no performance verdict")
        continue
    ra, rb = [x[0] for x in a], [x[0] for x in b]
    sep = min(ra) > max(rb) or max(ra) < min(rb)
    print(f"{who}: MIXED {st.mean(ra):.1f} k/s vs {alone} {st.mean(rb):.1f} k/s = {st.mean(ra)/st.mean(rb):.3f}x"
          f" ({'SEPARABLE' if sep else 'ranges overlap'})")
PYEOF

#!/usr/bin/env bash
# socket_marks_netns.sh - network profiles items D (traffic classes) and E (send time) on a veth pair between two private
# namespaces, 2026-10-22. The kernel's own counters say where each datagram went, not TickLE's: the sender's prio qdisc
# counts packets per band (the SO_PRIORITY each datagram carried, through its priority map), the receiver's ingress u32
# filters count the sender's packets per TOS byte (the DSCP; by source address, since a namespace's own broadcasts loop
# back through its ingress), and an ETF qdisc releases timed datagrams at their time.
#
# Arms (each a fresh namespace pair, a RELIABLE UInt64 publisher in A, its subscriber in B; socket_marks_node.c):
#   D0  control: every option 0. Sender root prio, priority map 3 3 1 2 3 0 3 ... (priority 0 -> band 1:4).
#   D1  classes: DATA (5, DSCP 46), REPAIR (3, DSCP 16), CONTROL (2, DSCP 8), same qdisc; band 1:1 (priority 5) has a
#       netem loss LOSS_PCT child, so RELIABLE resends exist; bands 1:2 (2) and 1:3 (3) are plain.
#   E0  control: DATA priority 5 to band 1:1 with a pfifo child, every other class priority 0 to band 1:2; the publisher
#       asks for a send time OFFSET_US ahead (tt_Publisher_publish_at()), with SO_TXTIME on.
#   E1  the same with an ETF child on band 1:1 (clockid CLOCK_TAI, delta DELTA_US): the datagrams leave at their time.
#
# PRE-REGISTERED READING (the python at the end):
#   VOID  an arm whose subscriber got fewer than 90% of COUNT samples, or a publisher line missing.
#   D0 PASS: the HAL marked nothing (marks=0, every class count 0), the classes' bands 1:1-1:3 carried 0 packets, and
#            every IP datagram the receiver saw had TOS 0.
#   D1 PASS: per-datagram marks on (marks has bit 1); REPAIR > 0 (the loss made resends); the sender's bands equal the
#            HAL's own counts - 1:1 sent plus its netem drops = DATA (a class's count is what it dequeued), 1:2 = CONTROL,
#            1:3 = REPAIR; the receiver's TOS counts equal what each band delivered: 0xb8 = 1:1, 0x40 = 1:3, 0x20 = 1:2;
#            no IP datagram of the sender's with TOS 0.
#   E PASS:  E0's arrival minus requested time near -OFFSET (p50 below -OFFSET/2): sent at once, the time ignored; E1's
#            p50 within [-DELTA_US - 100, -DELTA_US + 400] us (ETF in software releases a datagram DELTA ahead of its
#            time, for the device - a NIC with launch-time offload holds it to the time itself), its p99 - min below
#            500 us, and the ETF qdisc dropped nothing; ETF's band carried every DATA datagram, and the HAL timed at least
#            as many (a CONTROL datagram sent during a publish carries the time too; its band ignores it).
#   FALSIFIED (any arm's FAIL): a class lands in a band or a TOS it was not given, or the time does not move arrivals.
#
# Usage: [COUNT=400] [INTERVAL_US=2000] [LOSS_PCT=10] [OFFSET_US=2000] [DELTA_US=200] [OUT=prefix] socket_marks_netns.sh
#   Needs `sudo -n ip` (tc and the nodes run as root under `ip netns exec`: SO_TXTIME with CLOCK_TAI needs
#   CAP_NET_ADMIN). OUT defaults to ~/rig_results_safe/socket_marks_netns_<stamp> ($OUT.txt, $OUT.d/). The summary
#   always ends with "=== socket_marks_netns ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
COUNT=${COUNT:-400}
INTERVAL_US=${INTERVAL_US:-2000}
LOSS_PCT=${LOSS_PCT:-10}
OFFSET_US=${OFFSET_US:-2000}
DELTA_US=${DELTA_US:-200}
OUT=${OUT:-$HOME/rig_results_safe/socket_marks_netns_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
WORK=$(mktemp -d "$HOME/tmp_build/socket_marks_netns.XXXXXX") || exit 1
mkdir -p "$WORK/tmp" && export TMPDIR="$WORK/tmp"
NA="smk$$a"
NB="smk$$b"
on_exit() {
    local rc=$?
    sudo -n ip netns del "$NA" 2>/dev/null
    sudo -n ip netns del "$NB" 2>/dev/null
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== socket_marks_netns ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
say "=== socket_marks_netns $(date -Is): COUNT=$COUNT INTERVAL_US=$INTERVAL_US LOSS_PCT=$LOSS_PCT OFFSET_US=$OFFSET_US DELTA_US=$DELTA_US tree=$(git -C "$REPO" rev-parse --short HEAD) diff=$(git -C "$REPO" diff HEAD | sha256sum | cut -c1-12) kernel=$(uname -r) ==="

# The tree's library, release build, and the node against it.
(cd "$REPO/platform/linux" && nice -n 10 make -s -j2 BUILD_TYPE=release library) >"$OUT.d/build.log" 2>&1 || {
    say "FATAL: library build failed - $OUT.d/build.log"
    exit 1
}
gcc -O2 -Wall -I"$REPO/include" -I"$REPO/examples/uint64" -o "$WORK/node" "$HERE/socket_marks_node.c" \
    "$REPO/examples/uint64/UInt64.c" "$REPO/platform/linux/libtickle.a" -lpthread -lm >>"$OUT.d/build.log" 2>&1 || {
    say "FATAL: node build failed - $OUT.d/build.log"
    exit 1
}

xa() { sudo -n ip netns exec "$NA" "$@"; }
xb() { sudo -n ip netns exec "$NB" "$@"; }

setup() { # a fresh pair, IPv6 off (no neighbour discovery in the counts)
    sudo -n ip netns del "$NA" 2>/dev/null
    sudo -n ip netns del "$NB" 2>/dev/null
    sudo -n ip netns add "$NA" && sudo -n ip netns add "$NB" &&
        sudo -n ip link add smkva netns "$NA" type veth peer name smkvb netns "$NB" &&
        xa sysctl -qw net.ipv6.conf.all.disable_ipv6=1 && xb sysctl -qw net.ipv6.conf.all.disable_ipv6=1 &&
        sudo -n ip -n "$NA" addr add 192.168.77.1/24 brd + dev smkva &&
        sudo -n ip -n "$NB" addr add 192.168.77.2/24 brd + dev smkvb &&
        sudo -n ip -n "$NA" link set lo up && sudo -n ip -n "$NB" link set lo up &&
        sudo -n ip -n "$NA" link set smkva up && sudo -n ip -n "$NB" link set smkvb up &&
        xb tc qdisc add dev smkvb ingress &&
        xb tc filter add dev smkvb parent ffff: protocol ip prio 1 u32 match ip src 192.168.77.1/32 match ip tos 0xb8 0xff action ok &&
        xb tc filter add dev smkvb parent ffff: protocol ip prio 2 u32 match ip src 192.168.77.1/32 match ip tos 0x40 0xff action ok &&
        xb tc filter add dev smkvb parent ffff: protocol ip prio 3 u32 match ip src 192.168.77.1/32 match ip tos 0x20 0xff action ok &&
        xb tc filter add dev smkvb parent ffff: protocol ip prio 4 u32 match ip src 192.168.77.1/32 match ip tos 0x00 0xff action ok
}

# run ARM PUB_ARGS...: the subscriber in B (its own /dev/shm, so no segment joins the two), then the publisher in A.
run() {
    local arm=$1
    shift
    local seconds=$((COUNT * INTERVAL_US / 1000000 + 8))
    # shellcheck disable=SC2024 # the logs are this user's
    xb sh -c "mount -t tmpfs -o size=16m smkshm /dev/shm || exit 9; exec $WORK/node sub 192.168.77.255 $seconds" \
        >"$OUT.d/$arm.sub.log" 2>&1 &
    local sub=$!
    sleep 1
    # shellcheck disable=SC2024
    xa sh -c "mount -t tmpfs -o size=16m smkshm /dev/shm || exit 9; exec $WORK/node pub 192.168.77.255 $*" \
        >"$OUT.d/$arm.pub.log" 2>&1
    wait "$sub"
    xa tc -s -j class show dev smkva >"$OUT.d/$arm.classes.json"
    xa tc -s -j qdisc show dev smkva >"$OUT.d/$arm.qdiscs.json"
    xb tc -s -j filter show dev smkvb ingress >"$OUT.d/$arm.tos.json"
    say "$arm: $(grep -h '^PUB\|^SUB' "$OUT.d/$arm.pub.log" "$OUT.d/$arm.sub.log" | tr '\n' ' ')"
}

PRIOMAP=(3 3 1 2 3 0 3 3 3 3 3 3 3 3 3 3) # priority 5 -> 1:1, 2 -> 1:2, 3 -> 1:3, the rest -> 1:4
for arm in D0 D1; do
    setup || {
        say "FATAL: namespaces for $arm"
        exit 1
    }
    if ! { xa tc qdisc add dev smkva root handle 1: prio bands 4 priomap "${PRIOMAP[@]}" &&
        xa tc qdisc add dev smkva parent 1:1 handle 10: netem loss "${LOSS_PCT}%"; }; then
        say "FATAL: qdisc for $arm"
        exit 1
    fi
    if [ "$arm" = D0 ]; then
        run D0 "$COUNT" "$INTERVAL_US" 0 0 0 0 0 0 0 0
    else
        run D1 "$COUNT" "$INTERVAL_US" 0 0 5 46 3 16 2 8
    fi
done
for arm in E0 E1; do
    setup || {
        say "FATAL: namespaces for $arm"
        exit 1
    }
    child=(pfifo)
    if [ "$arm" = E1 ]; then
        child=(etf clockid CLOCK_TAI delta $((DELTA_US * 1000)))
    fi
    if ! { xa tc qdisc add dev smkva root handle 1: prio bands 2 priomap 1 1 1 1 1 0 1 1 1 1 1 1 1 1 1 1 &&
        xa tc qdisc add dev smkva parent 1:1 handle 10: "${child[@]}"; }; then
        say "FATAL: qdisc for $arm"
        exit 1
    fi
    run "$arm" "$COUNT" "$INTERVAL_US" "$OFFSET_US" 1 5 0 0 0 0 0
done

python3 - "$OUT.d" "$COUNT" "$OFFSET_US" "$DELTA_US" <<'PY' | tee -a "$SUM"
import json, re, sys
d, count, offset, delta = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])

def line(arm, who):
    for l in open(f"{d}/{arm}.{who}.log"):
        if l.startswith(who.upper()):
            return dict(re.findall(r"(\w+)=(-?[\d.]+)", l))
    return None

def bands(arm):
    out = {}
    for c in json.load(open(f"{d}/{arm}.classes.json")):
        out[c["handle"]] = c.get("stats", {}).get("packets", 0)
    return out

def qdisc_drops(arm, kind):
    return sum(q.get("drops", 0) for q in json.load(open(f"{d}/{arm}.qdiscs.json")) if q.get("kind") == kind)

def qdisc_packets(arm, kind):
    return sum(q.get("packets", 0) for q in json.load(open(f"{d}/{arm}.qdiscs.json")) if q.get("kind") == kind)

def tos(arm):
    out = {}
    for f in json.load(open(f"{d}/{arm}.tos.json")):
        opts = f.get("options", {})
        match = opts.get("match")
        actions = opts.get("actions", [])
        if match is None or not actions:
            continue
        value = int(match["value"], 16) >> 16 & 0xff if isinstance(match, dict) else None
        out[value] = actions[0].get("stats", {}).get("packets", 0)
    return out

verdicts = []
def judge(arm, ok, why):
    verdicts.append(ok)
    print(f"{arm}: {'PASS' if ok else 'FAIL'} - {why}")

for arm in ("D0", "D1", "E0", "E1"):
    pub, sub = line(arm, "pub"), line(arm, "sub")
    if pub is None or sub is None or int(sub.get("n", 0)) < 0.9 * count:
        print(f"{arm}: VOID - publisher line {pub is not None}, subscriber n={None if sub is None else sub.get('n')}")
        verdicts.append(False)
        continue
    b = bands(arm)
    t = tos(arm)
    print(f"{arm}: pub {pub} sub {sub} bands {b} tos {t}")
    if arm == "D0":
        quiet = all(int(pub[k]) == 0 for k in ("marks", "data", "repair", "control"))
        judge(arm, quiet and b.get("1:1", 0) == 0 and b.get("1:2", 0) == 0 and b.get("1:3", 0) == 0
              and t.get(0xb8, 0) == t.get(0x40, 0) == t.get(0x20, 0) == 0 and t.get(0, 0) > 0,
              "no marks; every datagram priority 0 (band 1:4) and TOS 0")
    elif arm == "D1":
        lost = qdisc_drops(arm, "netem")
        sent_11 = b.get("1:1", 0)
        judge(arm, int(pub["marks"]) & 1 == 1 and int(pub["repair"]) > 0
              and sent_11 + lost == int(pub["data"]) and b.get("1:2", 0) == int(pub["control"])
              and b.get("1:3", 0) == int(pub["repair"])
              and t.get(0x20, 0) == b.get("1:2", 0) and t.get(0x40, 0) == b.get("1:3", 0)
              and t.get(0xb8, 0) == sent_11 and t.get(0, 0) == 0,
              f"bands 1:1/1:2/1:3 = {sent_11}/{b.get('1:2', 0)}/{b.get('1:3', 0)} against the HAL's DATA/CONTROL/REPAIR "
              f"{pub['data']}/{pub['control']}/{pub['repair']}; TOS 0xb8/0x40/0x20/0 = {t.get(0xb8, 0)}/"
              f"{t.get(0x40, 0)}/{t.get(0x20, 0)}/{t.get(0, 0)}, netem dropped {lost}")
e0, e1 = line("E0", "sub"), line("E1", "sub")
p0, p1 = line("E0", "pub"), line("E1", "pub")
if None not in (e0, e1, p0, p1):
    ok0 = float(e0["p50"]) < -offset / 2 and int(p0["timed"]) >= int(p0["data"]) > 0
    judge("E0", ok0, f"pfifo: arrival - requested p50 {e0['p50']} us (sent at once, about -{offset:.0f}); "
          f"timed {p0['timed']} of DATA {p0['data']}")
    etf_drops = qdisc_drops("E1", "etf")
    ok1 = (-delta - 100 <= float(e1["p50"]) <= -delta + 400 and float(e1["p99"]) - float(e1["min"]) < 500
           and etf_drops == 0 and int(p1["timed"]) >= int(p1["data"]) > 0
           and qdisc_packets("E1", "etf") == int(p1["data"]))
    judge("E1", ok1, f"etf: arrival - requested min/p50/p99/max {e1['min']}/{e1['p50']}/{e1['p99']}/{e1['max']} us "
          f"(window [{-delta - 100:.0f}, {-delta + 400:.0f}]), etf drops {etf_drops}, etf packets "
          f"{qdisc_packets('E1', 'etf')}; timed {p1['timed']} of DATA {p1['data']}")
print("VERDICT=" + ("PASS" if verdicts and all(verdicts) else "FAIL"))
PY

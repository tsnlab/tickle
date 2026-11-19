#!/usr/bin/env bash
# shellcheck disable=SC1091,SC2029,SC2016,SC2086,SC2129,SC2012,SC2024,SC2034,SC2209  # experiment driver: ssh-side expansion, unquoted word lists and sudo redirects are deliberate
# Wi-Fi-like link emulated with tc netem (plan: PLAN.md). One link between a namespace on each Pi, carried by a GRE tap over the rig's
# Ethernet; the link's rate, delay, loss and queue are tc netem on the egress of each tap. NEVER touches wlan0 or eth0's configuration.
#   wtc_topo.sh cli up    on the client Pi (rpi-1): namespace wtc_sta, tap wtcgc, netem from $NETEM
#   wtc_topo.sh cli down
#   wtc_topo.sh srv up    on the server Pi (rpi-0): namespace wtc_srv, tap wtcgs, netem from $NETEM
#   wtc_topo.sh srv down
#   wtc_topo.sh modules load|unload   ip_gre / sch_netem / nf_tables once per session (what was not loaded before is unloaded again)
#   wtc_topo.sh snap      the state the runner compares before/after (wlan0, eth0, modules, namespaces, links)
# Env: WTC_DIR work dir, LOCAL_IP/REMOTE_IP (eth0 addresses of this/the other Pi), NETEM (tc netem arguments, e.g. "limit 100 delay 5ms rate 30mbit loss 1%").
# Needs passwordless sudo for: modprobe, ip, tc, nft, kill.
set -u
DIR="${WTC_DIR:-$HOME/wtc_run}"
BINDIR="${WTC_BIN:-$HOME/t1pi_bin}"
SUBNET=10.90.0
L2_MTU=1500 # the emulated link's MTU; the GRE tap needs 38 bytes more than eth0's 1500 allows, so its outer packet is fragmented on the
# wire (nopmtudisc) and DF is cleared on the inner packet in the tap's namespace (UDP sockets set DF, the tunnel would drop 1500 B datagrams)
STA_IP=$SUBNET.2
SRV_IP=$SUBNET.3
role=${1:-}
cmd=${2:-}
NETEM=${NETEM:-}
LOCAL_IP=${LOCAL_IP:-}
REMOTE_IP=${REMOTE_IP:-}

die() { echo "WTC_REFUSED: $*" >&2; exit 3; }

df_rules() {
    echo "table ip wtcdf { chain odf { type filter hook output priority -200; policy accept; ip frag-off == 0x4000 ip frag-off set 0; }; }"
}

reap_nodes() { # our node programs, by exact exe path
    local p exe
    for p in /proc/[0-9]*; do
        exe=$(sudo -n readlink "$p/exe" 2>/dev/null) || continue
        case "$exe" in "$BINDIR"/*) sudo -n kill "${p#/proc/}" 2>/dev/null ;; esac
    done
}

snap() {
    echo "== wlan0/eth0"
    ip -o addr show dev wlan0 | awk '{print $2,$3,$4}'
    ip -o addr show dev eth0 | awk '{print $2,$3,$4}'
    ip route show
    ip -d link show wlan0 | grep -v -E '^\s+(RX|TX)' | sed -n 1,2p
    ip -d link show eth0 | sed -n 1,2p
    tc qdisc show dev eth0
    tc qdisc show dev wlan0
    iw dev wlan0 info 2>&1 | grep -E 'ifindex|wdev|addr|ssid|type|channel'
    echo "== modules"
    lsmod | awk 'NR>1 {print $1}' | sort | grep -E 'netem|ip_gre|gre|ip_tunnel|nf_tables|nfnetlink|sch_' || true
    echo "== netns"
    ip netns list | awk '{print $1}' | sort
    echo "== links in init ns"
    ip -o link show | awk -F': ' '{print $2}' | sed 's/@.*//' | sort
}

tap_up() { # ns tap local remote ip
    local ns=$1 tap=$2 lip=$3 rip=$4 ip=$5
    ip netns list | grep -q "^$ns" && die "$ns exists: run down first"
    sudo -n ip netns add "$ns" || return 1
    sudo -n ip link add "$tap" type gretap local "$lip" remote "$rip" nopmtudisc || return 1
    sudo -n ip link set "$tap" netns "$ns" || return 1
    sudo -n ip -n "$ns" link set "$tap" mtu "$L2_MTU"
    sudo -n ip netns exec "$ns" nft -f - <<<"$(df_rules)" || return 1
    sudo -n ip -n "$ns" link set lo up
    sudo -n ip -n "$ns" addr add "$ip/24" dev "$tap"
    sudo -n ip -n "$ns" link set "$tap" up
    if [ -n "$NETEM" ]; then
        # shellcheck disable=SC2086
        sudo -n ip netns exec "$ns" tc qdisc add dev "$tap" root netem $NETEM || { echo "WTC_UP_FAILED netem '$NETEM'"; return 1; }
    fi
}

# setnetem ns tap: the tap's netem becomes $NETEM, in place (tc qdisc change: the queue and its packets stay) - a rate step.
setnetem() {
    sudo -n ip netns exec "$1" tc qdisc change dev "$2" root netem $NETEM || { echo "WTC_SETNETEM_FAILED '$NETEM'"; return 1; }
}

tap_down() { # ns tap
    reap_nodes
    ip netns list | grep -q "^$1" && sudo -n ip netns del "$1"
    ip link show "$2" >/dev/null 2>&1 && sudo -n ip link del "$2"
    return 0
}

modules() {
    local m pass
    mkdir -p "$DIR/run"
    case "$cmd" in
    load)
        lsmod | awk 'NR>1 {print $1}' | sort >"$DIR/run/modules_before"
        for m in ip_gre sch_netem; do lsmod | grep -q "^$m " || sudo -n modprobe "$m"; done
        sudo -n ip netns add wtc_warm && {
            sudo -n ip netns exec wtc_warm nft -f - <<<"$(df_rules)"
            sudo -n ip netns del wtc_warm
        }
        ;;
    unload)
        [ -f "$DIR/run/modules_before" ] || { echo "WTC_MODULES_UNLOAD nothing recorded"; return 0; }
        for pass in 1 2 3 4 5; do
            for m in $(lsmod | awk 'NR>1 {print $1}'); do
                grep -qx "$m" "$DIR/run/modules_before" && continue
                case "$m" in ip_gre | gre | ip_tunnel | erspan | sch_netem | nf_tables | nfnetlink | nft_* | nf_*) sudo -n modprobe -r "$m" 2>/dev/null ;; esac
            done
        done
        rm -f "$DIR/run/modules_before"
        ;;
    esac
    echo "WTC_MODULES_${cmd^^}"
}

case "$role $cmd" in
"modules "*) modules ;;
"cli up") [ -n "$LOCAL_IP" ] && [ -n "$REMOTE_IP" ] || die "LOCAL_IP and REMOTE_IP are required"
    mkdir -p "$DIR/run"; tap_up wtc_sta wtcgc "$LOCAL_IP" "$REMOTE_IP" "$STA_IP" && echo "WTC_CLI_UP sta=$STA_IP netem='$NETEM'" ;;
"cli down") tap_down wtc_sta wtcgc; echo WTC_CLI_DOWN ;;
"srv up") [ -n "$LOCAL_IP" ] && [ -n "$REMOTE_IP" ] || die "LOCAL_IP and REMOTE_IP are required"
    mkdir -p "$DIR/run"; tap_up wtc_srv wtcgs "$LOCAL_IP" "$REMOTE_IP" "$SRV_IP" && echo "WTC_SRV_UP srv=$SRV_IP netem='$NETEM'" ;;
"srv down") tap_down wtc_srv wtcgs; echo WTC_SRV_DOWN ;;
"snap "*) snap ;;
"cli setnetem") setnetem wtc_sta wtcgc && echo "WTC_CLI_NETEM $NETEM" ;;
"srv setnetem") setnetem wtc_srv wtcgs && echo "WTC_SRV_NETEM $NETEM" ;;
*) [ "$role" = snap ] && { snap; exit 0; }; echo "usage: $0 cli up|down|setnetem | srv up|down|setnetem | modules load|unload | snap"; exit 2 ;;
esac

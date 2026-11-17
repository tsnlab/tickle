#!/usr/bin/env bash
# T1-Pi topology (plan: PLAN.md). SIMULATION ONLY: mac80211_hwsim + wmediumd. NEVER touches the real wlan0 or eth0's config.
#   t1pi_topo.sh cli up [GE]   on the client Pi (rpi-1): 2 hwsim radios (STA, AP), wmediumd, wpa_supplicant, bridge + GRE tap
#   t1pi_topo.sh cli down      stop what `up` started, delete namespaces, unload mac80211_hwsim if `up` loaded it
#   t1pi_topo.sh srv up        on the server Pi (rpi-0): namespace t1p_srv with the other GRE tap end
#   t1pi_topo.sh srv down
#   t1pi_topo.sh modules load|unload   load bridge/ip_gre once per session (and unload what we loaded at the end)
#   t1pi_topo.sh snap          print the state the runner compares before/after (wlan0, eth0, modules, phys, namespaces)
# Env: T1P_DIR work dir (default $HOME/t1pi_run), WM wmediumd, LOCAL_IP/REMOTE_IP (eth0 addresses of this/the other Pi),
#      LOSS (steady link error when GE is empty), BAD_P, GE "good_ms,bad_ms,good_p[,...]", RETRY, SEED, TRACE, PIN_WM.
# Needs passwordless sudo for: modprobe, ip, iw, wpa_supplicant, wmediumd, kill, taskset.
set -u
DIR="${T1P_DIR:-$HOME/t1pi_run}"
WM="${WM:-/usr/local/bin/wmediumd}"
WPA=/usr/sbin/wpa_supplicant
BINDIR="${T1P_BIN:-$HOME/t1pi_bin}"
SUBNET=10.90.0
# The L2 segment has the Wi-Fi's MTU 1500 although the GRE tap needs 38 bytes more than eth0's 1500 allows: the tap's outer
# packet is fragmented on the wire (nopmtudisc) and the tunnel's DF check on the inner packet (which would drop a 1500-byte
# datagram, since UDP sockets set DF) is neutralised by clearing DF on the inner packet in the tunnel's namespace (df_rules).
L2_MTU=1500
STA_IP=$SUBNET.2
SRV_IP=$SUBNET.3
role=${1:-}
cmd=${2:-}
GE=${3:-}
BAD_P=${BAD_P:-0.9}
LOSS=${LOSS:-0.0}
RETRY=${RETRY-1}
SEED=${SEED-7}
PIN_WM=${PIN_WM-2}
LOCAL_IP=${LOCAL_IP:-}
REMOTE_IP=${REMOTE_IP:-}

die() { echo "T1PI_REFUSED: $*" >&2; exit 3; }
is_hwsim() { [[ "$(readlink -f "/sys/class/ieee80211/$1/device" 2>/dev/null)" == /sys/devices/virtual/mac80211_hwsim/* ]]; }
list_phys() { ls /sys/class/ieee80211 2>/dev/null | sort; }

# Stop a daemon from its pid file, only after /proc/<pid>/exe is the expected binary.
stop_pid() { # pidfile exe
    local pf=$1 exe=$2 pid
    [ -s "$pf" ] || return 0
    pid=$(cat "$pf")
    if [ -n "$pid" ] && [ "$(sudo -n readlink "/proc/$pid/exe" 2>/dev/null)" = "$(readlink -f "$exe")" ]; then
        sudo -n kill "$pid" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do [ -d "/proc/$pid" ] || break; sleep 0.5; done
        [ -d "/proc/$pid" ] && sudo -n kill -9 "$pid" 2>/dev/null
    fi
    rm -f "$pf"
}

# Clear DF on inner packets that carry exactly DF (frag-off 0x4000) in the namespace holding the GRE tap's bridge/output path.
df_rules() { # bridge | ip
    if [ "$1" = bridge ]; then
        echo "table bridge t1pdf { chain cdf { type filter hook forward priority -200; policy accept; ether type ip ip frag-off == 0x4000 ip frag-off set 0; }; }"
    else
        echo "table ip t1pdf { chain odf { type filter hook output priority -200; policy accept; ip frag-off == 0x4000 ip frag-off set 0; }; }"
    fi
}

# Kill leftover node programs of ours (exact exe path, never by name pattern).
reap_nodes() {
    local p exe
    for p in /proc/[0-9]*; do
        exe=$(sudo -n readlink "$p/exe" 2>/dev/null) || continue
        case "$exe" in "$BINDIR"/node_d100 | "$BINDIR"/node_base | "$BINDIR"/large) sudo -n kill "${p#/proc/}" 2>/dev/null ;; esac
    done
}

snap() {
    echo "== wlan0/eth0"
    ip -o addr show dev wlan0 | awk '{print $2,$3,$4}'
    ip -o addr show dev eth0 | awk '{print $2,$3,$4}'
    ip route show
    ip -d link show wlan0 | grep -v -E '^\s+(RX|TX)' | sed -n 1,2p
    ip -d link show eth0 | sed -n 1,2p
    iw dev wlan0 info 2>&1 | grep -E 'ifindex|wdev|addr|ssid|type|channel'
    echo "== modules"
    lsmod | awk 'NR>1 {print $1}' | sort | grep -E 'hwsim|mac80211|cfg80211|brcm|ip_gre|gre|bridge|ip_tunnel' || true
    echo "== phys"
    list_phys
    echo "== netns"
    ip netns list | awk '{print $1}' | sort
    echo "== links in init ns"
    ip -o link show | awk -F': ' '{print $2}' | sed 's/@.*//' | sort
    echo "== procs (ours: wmediumd, wpa_supplicant of $DIR)"
    local n_wm=0 n_wpa=0 p
    for p in /proc/[0-9]*; do
        case "$(readlink "$p/exe" 2>/dev/null)" in
        /usr/local/bin/wmediumd) n_wm=$((n_wm + 1)) ;;
        /usr/sbin/wpa_supplicant) tr '\0' ' ' <"$p/cmdline" 2>/dev/null | grep -q "$DIR/run" && n_wpa=$((n_wpa + 1)) ;;
        esac
    done
    echo "wmediumd=$n_wm wpa_t1pi=$n_wpa"
}

cli_up() {
    [ -n "$LOCAL_IP" ] && [ -n "$REMOTE_IP" ] || die "LOCAL_IP and REMOTE_IP (the eth0 addresses) are required"
    ip netns list | grep -q '^t1p_' && die "t1p_* namespaces exist: run down first"
    lsmod | grep -q '^mac80211_hwsim' && die "mac80211_hwsim is already loaded (not ours)"
    mkdir -p "$DIR/run"
    rm -f "$DIR"/run/*.pid
    list_phys >"$DIR/run/phys.before"
    sudo -n modprobe mac80211_hwsim radios=2 || die "modprobe mac80211_hwsim"
    : >"$DIR/run/hwsim_loaded"
    sleep 0.5
    local new=() p
    for p in $(list_phys); do grep -qx "$p" "$DIR/run/phys.before" || new+=("$p"); done
    [ "${#new[@]}" = 2 ] || { echo "T1PI_UP_FAILED new phys: ${new[*]}"; return 1; }
    for p in "${new[@]}"; do is_hwsim "$p" || { echo "T1PI_UP_FAILED: $p is not mac80211_hwsim"; return 1; }; done
    local phy_sta=${new[0]} phy_ap=${new[1]} ifn
    sudo -n ip netns add t1p_sta && sudo -n ip netns add t1p_ap || return 1
    # each hwsim phy has exactly one netdev; move the phy (not the name) into its namespace, then rename inside
    sudo -n iw phy "$phy_sta" set netns name t1p_sta || return 1
    sudo -n iw phy "$phy_ap" set netns name t1p_ap || return 1
    ifn=$(sudo -n ip netns exec t1p_sta iw dev | awk '/Interface/ {print $2}')
    [ "$(echo "$ifn" | wc -w)" = 1 ] || { echo "T1PI_UP_FAILED sta ifaces: $ifn"; return 1; }
    sudo -n ip -n t1p_sta link set "$ifn" name hwsta
    ifn=$(sudo -n ip netns exec t1p_ap iw dev | awk '/Interface/ {print $2}')
    [ "$(echo "$ifn" | wc -w)" = 1 ] || { echo "T1PI_UP_FAILED ap ifaces: $ifn"; return 1; }
    sudo -n ip -n t1p_ap link set "$ifn" name hwap
    local mac_sta mac_ap
    mac_sta=$(sudo -n ip -n t1p_sta -o link show hwsta | awk '{for(i=1;i<NF;i++) if($i=="link/ether") print $(i+1)}')
    mac_ap=$(sudo -n ip -n t1p_ap -o link show hwap | awk '{for(i=1;i<NF;i++) if($i=="link/ether") print $(i+1)}')
    echo "T1PI_PHYS sta=$phy_sta($mac_sta) ap=$phy_ap($mac_ap)"
    local links="(0, 1, $BAD_P)"
    [ -n "$GE" ] || links="(0, 1, $LOSS)"
    {
        echo "ifaces : { ids = [ \"$mac_sta\", \"$mac_ap\" ]; };"
        echo "model : { type = \"prob\"; ${GE:+ge = \"$GE\"; }${TRACE:+trace = \"$TRACE\"; }${RETRY:+retry = \"1\"; }${SEED:+seed = \"$SEED\"; }links = ( $links ); };"
    } >"$DIR/run/lanes.cfg"
    sudo -n sh -c "echo \$\$ >'$DIR/run/wm.pid'; exec ${PIN_WM:+taskset -c $PIN_WM} '$WM' -c '$DIR/run/lanes.cfg'" \
        >"$DIR/run/wm.log" 2>&1 </dev/null &
    sleep 2
    sudo -n readlink "/proc/$(cat "$DIR/run/wm.pid")/exe" | grep -q wmediumd || { echo "T1PI_UP_FAILED wmediumd"; return 1; }
    cat >"$DIR/run/ap.conf" <<CONF
ctrl_interface=$DIR/run/ctl_ap
ap_scan=2
network={
  ssid="t1pi"
  mode=2
  frequency=2437
  key_mgmt=NONE
}
CONF
    cat >"$DIR/run/sta.conf" <<CONF
ctrl_interface=$DIR/run/ctl_sta
network={
  ssid="t1pi"
  key_mgmt=NONE
  scan_freq=2437
}
CONF
    sudo -n ip netns exec t1p_ap "$WPA" -B -P "$DIR/run/wpa_ap.pid" -Dnl80211 -ihwap -c "$DIR/run/ap.conf" -f "$DIR/run/ap.log" >/dev/null 2>&1
    sudo -n ip netns exec t1p_sta "$WPA" -B -P "$DIR/run/wpa_sta.pid" -Dnl80211 -ihwsta -c "$DIR/run/sta.conf" -f "$DIR/run/sta.log" >/dev/null 2>&1
    # AP side: bridge hwap + the GRE tap (created in the init namespace so it binds to eth0's address, then moved)
    sudo -n ip link add t1pgt type gretap local "$LOCAL_IP" remote "$REMOTE_IP" nopmtudisc || return 1
    sudo -n ip link set t1pgt netns t1p_ap || return 1
    sudo -n ip -n t1p_ap link add br0 type bridge stp_state 0 forward_delay 0
    sudo -n ip -n t1p_ap link set t1pgt mtu "$L2_MTU"
    sudo -n ip netns exec t1p_ap nft -f - <<<"$(df_rules bridge)" || { echo "T1PI_UP_FAILED nft"; return 1; }
    sudo -n ip -n t1p_ap link set br0 mtu "$L2_MTU" up
    sudo -n ip -n t1p_ap link set t1pgt master br0 up
    sudo -n ip -n t1p_ap link set lo up
    sudo -n ip -n t1p_sta link set lo up
    sudo -n ip -n t1p_sta addr add "$STA_IP/24" dev hwsta
    local i
    for i in $(seq 1 30); do
        sudo -n ip netns exec t1p_sta iw dev hwsta link 2>/dev/null | grep -q Connected && break
        sleep 1
    done
    sudo -n ip netns exec t1p_sta iw dev hwsta link 2>/dev/null | grep -q Connected || { echo "T1PI_UP_FAILED not associated"; return 1; }
    sudo -n ip -n t1p_sta link set hwsta up
    sudo -n ip -n t1p_ap link set hwap master br0 || { echo "T1PI_UP_FAILED bridge hwap"; return 1; }
    echo "T1PI_CLI_UP ge='$GE' loss=$LOSS bad_p=$BAD_P retry=${RETRY:-off} seed=${SEED:-none} sta=$STA_IP"
}

cli_down() {
    reap_nodes
    stop_pid "$DIR/run/wpa_sta.pid" "$WPA"
    stop_pid "$DIR/run/wpa_ap.pid" "$WPA"
    stop_pid "$DIR/run/wm.pid" "$WM"
    local ns
    for ns in t1p_ap t1p_sta; do ip netns list | grep -q "^$ns" && sudo -n ip netns del "$ns"; done
    ip link show t1pgt >/dev/null 2>&1 && sudo -n ip link del t1pgt
    sleep 1
    if [ -f "$DIR/run/hwsim_loaded" ]; then
        sudo -n modprobe -r mac80211_hwsim && rm -f "$DIR/run/hwsim_loaded"
        lsmod | grep -q '^mac80211_hwsim' && echo "T1PI_DOWN_WARN mac80211_hwsim still loaded"
    fi
    echo T1PI_CLI_DOWN
}

srv_up() {
    [ -n "$LOCAL_IP" ] && [ -n "$REMOTE_IP" ] || die "LOCAL_IP and REMOTE_IP are required"
    ip netns list | grep -q '^t1p_srv' && die "t1p_srv exists: run down first"
    mkdir -p "$DIR/run"
    sudo -n ip netns add t1p_srv || return 1
    sudo -n ip link add t1pgs type gretap local "$LOCAL_IP" remote "$REMOTE_IP" nopmtudisc || return 1
    sudo -n ip link set t1pgs netns t1p_srv || return 1
    sudo -n ip -n t1p_srv link set t1pgs mtu "$L2_MTU"
    sudo -n ip netns exec t1p_srv nft -f - <<<"$(df_rules ip)" || return 1
    sudo -n ip -n t1p_srv link set lo up
    sudo -n ip -n t1p_srv addr add "$SRV_IP/24" dev t1pgs
    sudo -n ip -n t1p_srv link set t1pgs up
    echo "T1PI_SRV_UP srv=$SRV_IP"
}

srv_down() {
    reap_nodes
    ip netns list | grep -q '^t1p_srv' && sudo -n ip netns del t1p_srv
    ip link show t1pgs >/dev/null 2>&1 && sudo -n ip link del t1pgs
    echo T1PI_SRV_DOWN
}

# Modules the topology needs (bridge, ip_gre, nf_tables with the bridge/ip rules) are loaded once per session, before the
# baseline snapshot, and every module that was not loaded before is removed again at the end of the session.
modules() {
    local m pass
    mkdir -p "$DIR/run"
    case "$cmd" in
    load)
        lsmod | awk 'NR>1 {print $1}' | sort >"$DIR/run/modules_before"
        for m in bridge ip_gre; do lsmod | grep -q "^$m " || sudo -n modprobe "$m"; done
        sudo -n ip netns add t1p_warm && {
            sudo -n ip netns exec t1p_warm nft -f - <<<"$(df_rules bridge)
$(df_rules ip)"
            sudo -n ip netns del t1p_warm
        }
        ;;
    unload)
        [ -f "$DIR/run/modules_before" ] || { echo "T1PI_MODULES_UNLOAD nothing recorded"; return 0; }
        for pass in 1 2 3 4 5; do
            for m in $(lsmod | awk 'NR>1 {print $1}'); do
                grep -qx "$m" "$DIR/run/modules_before" && continue
                case "$m" in ip_gre | gre | ip_tunnel | erspan | bridge | stp | llc | nf_tables | nfnetlink | nft_* | nf_*) sudo -n modprobe -r "$m" 2>/dev/null ;; esac
            done
        done
        rm -f "$DIR/run/modules_before"
        ;;
    esac
    echo "T1PI_MODULES_${cmd^^}"
}

case "$role $cmd" in
"modules "*) modules ;;
"cli up") cli_up ;;
"cli down") cli_down ;;
"srv up") srv_up ;;
"srv down") srv_down ;;
"snap "*) snap ;;
*) [ "$role" = snap ] && { snap; exit 0; }; echo "usage: $0 cli up [GE] | cli down | srv up | srv down | snap"; exit 2 ;;
esac

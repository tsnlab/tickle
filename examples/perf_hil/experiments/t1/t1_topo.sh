#!/usr/bin/env bash
# T1 topology: mac80211_hwsim + wmediumd, simulation only. NEVER touches a real Wi-Fi device.
#   t1_topo.sh up LANES GE   - LANES lanes of 3 nodes (AP=ping, STA=echo, STA=idle); GE = "good_ms,bad_ms,good_p" or ""
#   t1_topo.sh down          - stop wmediumd and every wpa_supplicant of the lanes, delete the namespaces
# Lane L uses phys 3L..3L+2 (phy N has MAC 02:00:00:00:NN:00 in hex), namespaces t1_L_K, subnet 10.8L.0.0/24.
# Needs sudo -n for: ip, iw, wpa_supplicant, wmediumd, pkill -x wmediumd (and pkill -x wpa_supplicant for `down`).
set -u
DIR="${T1_DIR:-$HOME/tmp_build/wm/t1}" # work dir: run/ (configs, logs, sockets)
WM="${WM:-$HOME/tmp_build/wm/wmediumd/wmediumd/wmediumd}" # patched wmediumd binary
cmd=${1:-}
LANES=${2:-1}
GE=${3:-}
BAD_P=${BAD_P:-0.9}
PHY_BASE=${PHY_BASE:-0} # first phy of lane 0 (phys PHY_BASE.. are used)
LOSS=${LOSS:-0.0} # steady link error probability when GE is empty

mac() { printf '02:00:00:00:%02x:00' "$1"; }

# Simulation-only guard: the phy must be a mac80211_hwsim device.
is_hwsim() { [[ "$(readlink -f "/sys/class/ieee80211/phy$1/device" 2>/dev/null)" == /sys/devices/virtual/mac80211_hwsim/* ]]; }

up() {
    local L K n ns ids="" links="" a b
    [ "$LANES" -ge 1 ] && [ "$LANES" -le 5 ] || { echo "LANES 1..5"; exit 2; }
    mkdir -p "$DIR/run"
    for ((L = 0; L < LANES; L++)); do
        for K in 0 1 2; do
            n=$((PHY_BASE + 3 * L + K))
            is_hwsim "$n" || { echo "REFUSED: phy$n is not mac80211_hwsim"; exit 3; }
            ns=t1_${L}_$K
            sudo -n ip netns add "$ns" || exit 1
            sudo -n iw phy "phy$n" set netns name "$ns" || exit 1
            sudo -n ip -n "$ns" link set "wlan$n" up
            sudo -n ip -n "$ns" addr add "10.8$L.0.$((K + 1))/24" dev "wlan$n"
            ids="$ids${ids:+, }\"$(mac "$n")\""
        done
        # links inside the lane only (config index = position in ids)
        for a in 0 1 2; do
            for b in 0 1 2; do
                [ "$a" != "$b" ] && [ "$a" -lt "$b" ] && links="$links${links:+, }($((3 * L + a)), $((3 * L + b)), $BAD_P)"
            done
        done
        # shellcheck disable=SC2001 # kept as sed: the pattern is the number as a regex, as it always was
        [ -n "$GE" ] || links=$(echo "$links" | sed "s/$BAD_P/$LOSS/g")
    done
    {
        echo "ifaces : { ids = [ $ids ]; };"
        echo "model : { type = \"prob\"; ${GE:+ge = \"$GE\"; }${TRACE:+trace = \"$TRACE\"; }${RETRY:+retry = \"1\"; }${SEED:+seed = \"$SEED\"; }links = ( $links ); };"
    } >"$DIR/run/lanes.cfg"
    setsid nohup ${PIN_WM:+taskset -c $PIN_WM} sudo -n "$WM" -c "$DIR/run/lanes.cfg" >"$DIR/run/wm.log" 2>&1 </dev/null &
    sleep 2
    for ((L = 0; L < LANES; L++)); do
        n=$((PHY_BASE + 3 * L))
        cat >"$DIR/run/ap$L.conf" <<EOF
ctrl_interface=$DIR/run/ctl_ap$L
ap_scan=2
network={
  ssid="t1lane$L"
  mode=2
  frequency=2437
  key_mgmt=NONE
}
EOF
        setsid nohup sudo -n ip netns exec "t1_${L}_0" /usr/sbin/wpa_supplicant -Dnl80211 "-iwlan$n" \
            -c "$DIR/run/ap$L.conf" -f "$DIR/run/ap$L.log" >/dev/null 2>&1 </dev/null &
        echo "ctl_ap$L wlan$n" >>"$DIR/run/wpa.list"
        for K in 1 2; do
            cat >"$DIR/run/sta${L}_$K.conf" <<EOF
ctrl_interface=$DIR/run/ctl_sta${L}_$K
network={
  ssid="t1lane$L"
  key_mgmt=NONE
  scan_freq=2437
}
EOF
            echo "ctl_sta${L}_$K wlan$((n + K))" >>"$DIR/run/wpa.list"
            setsid nohup sudo -n ip netns exec "t1_${L}_$K" /usr/sbin/wpa_supplicant -Dnl80211 "-iwlan$((n + K))" \
                -c "$DIR/run/sta${L}_$K.conf" -f "$DIR/run/sta${L}_$K.log" >/dev/null 2>&1 </dev/null &
        done
    done
    # every STA associated, every lane pings: wait up to 30 s
    for ((i = 0; i < 30; i++)); do
        ok=1
        for ((L = 0; L < LANES; L++)); do
            for K in 1 2; do
                sudo -n ip netns exec "t1_${L}_$K" iw dev "wlan$((PHY_BASE + 3 * L + K))" link 2>/dev/null | grep -q Connected || ok=0
            done
        done
        [ $ok = 1 ] && { echo "T1_UP lanes=$LANES ge='$GE'"; return 0; }
        sleep 1
    done
    echo "T1_UP_FAILED"
    return 1
}

down() {
    local ns p
    sudo -n pkill -x wmediumd
    # stop only the wpa_supplicants this script started: wpa_cli terminate on their own control sockets
    if [ -f "$DIR/run/wpa.list" ]; then
        while read -r d i; do
            sudo -n /usr/sbin/wpa_cli -p "$DIR/run/$d" -i "$i" terminate >/dev/null 2>&1
        done <"$DIR/run/wpa.list"
        rm -f "$DIR/run/wpa.list"
        sleep 1
    fi
    # wpa_supplicant exits when its interface goes away: hand each phy back to the init namespace, then delete the ns.
    for ns in $(ip netns list | awk '/^t1/ {print $1}'); do
        for p in $(sudo -n ip netns exec "$ns" iw dev | awk '/^phy#/ {sub("phy#","",$1); print $1}'); do
            sudo -n ip netns exec "$ns" iw phy "phy$p" set netns 1
        done
    done
    sleep 2
    for ns in $(ip netns list | awk '/^t1/ {print $1}'); do sudo -n ip netns del "$ns"; done
    echo T1_DOWN
}

case "$cmd" in
up) rm -f "$DIR/run/wpa.list"; up ;;
down) down ;;
*) echo "usage: $0 up LANES [GE] | down"; exit 2 ;;
esac

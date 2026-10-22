#!/usr/bin/env bash
# Runs platform/linux/wr_check (the wait ring, hal_linux.c) in a private network namespace, so its context cannot reach
# the rig over this PC's management LAN (CLAUDE.md, "The rig and the network"). test_samehost.sh runs it on loopback in
# the default namespace, as it runs every same-host check; this is for running it by hand and for the mutant sweep
# (tests/mutants_wait_ring.py). The check bounds each of its waits with an alarm; this bounds the whole run as well.
# Usage: wr_check_netns.sh [binary]     Exit status is the binary's (124: the timeout).
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN=${1:-$REPO/platform/linux/wr_check}
NS=tickle_wrcheck_$$
cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT
sudo -n ip netns add "$NS" || { echo "FATAL cannot create netns"; exit 3; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
# The check's broadcast is 127.255.255.255, which lo carries; its datagrams stay in this namespace.
sudo -n ip netns exec "$NS" timeout 180 "$BIN"

#!/usr/bin/env bash
# Does binding the data socket to an interface's own address scope a *limited* broadcast
# (255.255.255.255) to that interface?
#
# Why a capture and not an assertion: the send succeeds either way and the sender cannot tell which
# link the kernel chose, so the claim is a negative - "nothing left by the other interface" - and
# only a capture on that other interface can establish it. rpi#1 is the one machine here with two
# links: eth0 (192.168.10.3, the test link) and wlan0 (10.1.1.214, the lab LAN, and the default
# route, which is where a limited broadcast goes when nothing scopes it).
#
# The control arm sends with no bind address and is EXPECTED to appear on wlan0: that is the leak
# the compiled-in 255.255.255.255 default causes, reproduced deliberately and bounded to ~2s of
# small datagrams so the comparison means something.
#
# rpi#1's ci user has NOPASSWD for exactly tcpdump, pkill, tc and apt-get install - nothing else.
# So each arm writes its own pcap filenames (no sudo rm), and the pcaps are read back through
# sudo tcpdump since tcpdump writes them as root.
#
# Lives in the repo, and writes its results outside any session scratchpad, deliberately: an
# earlier run of this experiment was launched detached from a session per CLAUDE.md's own rule 1,
# and then lost anyway when the session ended and took its scratchpad - script and output together
# - with it. Detaching the process is only half of outliving the session; the script and the
# results file have to outlive it too.
set -euo pipefail

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
HOST="ci@10.1.1.214"
SHA="${SHA:-$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse HEAD)}"
PORT=8282
ETH_ADDR=192.168.10.3
# Timestamped by default. A fixed filename means a run that never started leaves the PREVIOUS
# run's file in place, and a summary read from it is indistinguishable from a fresh result -
# which happened on 2026-09-24: an interleaved sweep sat blocked on the rig lock while its
# predecessor's numbers were about to be reported as the after-measurement. The tell was that
# they matched to three significant figures.
OUT="${OUT:-/tmp/tickle_bind_scope_result_$(date +%Y%m%d-%H%M%S).txt}"
ln -sfn "$OUT" "/tmp/tickle_bind_scope_result_latest.txt"
# Per-invocation, because tcpdump drops privileges after opening the device and then cannot
# overwrite a pcap left root-owned by an earlier run - which this host has no sudo rm to clear.
RUNID="$(date +%s)"

ssh_run() { ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new "$HOST" "$@"; }

: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }

say "=== bind-scoping capture, rpi#1, requested sha $SHA, $(date -Is) ==="
say "--- syncing and building examples ---"
# Single-quoted heredoc for everything that must expand ON THE RIG. An earlier version of this
# script echoed $(git rev-parse --short HEAD) inside a double-quoted ssh argument, so it reported
# THIS box's HEAD as if it were the rig's - the exact "verify the value the code sees, not the
# instruction you gave the toolchain" trap this project keeps re-learning.
ssh_run "SHA=$SHA bash -s" <<'REMOTE' | tee -a "$OUT"
set -e
cd ~/tickle
git fetch --quiet origin
git reset --hard --quiet "$SHA"
git clean -fdq
make examples >/tmp/bindscope_build.log 2>&1 || { tail -20 /tmp/bindscope_build.log; exit 1; }
test -x platform/linux/publisher
echo "rig HEAD is now $(git rev-parse --short HEAD)"
REMOTE

run_arm() {
    local tag="$1" label="$2" extra="$3"
    say ""
    say "--- arm $tag: $label   (publisher args: -b 255.255.255.255 $extra) ---"
    ssh_run "TAG=$tag PORT=$PORT RUNID=$RUNID EXTRA='$extra' bash -s" <<'REMOTE' | tee -a "$OUT"
set -e
# sudo runs the FIRST word, so `sudo -n timeout 12 tcpdump ...` asks sudo for `timeout`, which is
# not in this host's NOPASSWD list - it failed, and an earlier version of this script sent that
# failure to /dev/null and then reported eth0=0 wlan0=0 as if it were a measurement. Both halves
# of that are fixed here: sudo runs tcpdump directly, stopped afterwards with pkill (which IS in
# the list), and every error is kept and shown rather than discarded.
# -U: packet-buffered, so the file reflects what has actually been captured rather than whatever
# happens to have been flushed. Without it the pcap can be zero bytes while the capture is running
# perfectly well, which is how the readiness check below first read "did not start".
sudo -n tcpdump -U -i eth0  -n -p "udp port $PORT" -w "/tmp/cap_${RUNID}_${TAG}_eth0.pcap"  >"/tmp/td_${RUNID}_${TAG}_eth0.err"  2>&1 &
sudo -n tcpdump -U -i wlan0 -n -p "udp port $PORT" -w "/tmp/cap_${RUNID}_${TAG}_wlan0.pcap" >"/tmp/td_${RUNID}_${TAG}_wlan0.err" 2>&1 &
sleep 3
# The capture must be proven running before the publisher sends, or a silent capture is
# indistinguishable from a silent wire - which is exactly how the first run of this lied. tcpdump's
# own "listening on <if>" line is the proof: an empty pcap is not, since a capture with nothing in
# it yet legitimately has none.
for f in "/tmp/td_${RUNID}_${TAG}_eth0.err" "/tmp/td_${RUNID}_${TAG}_wlan0.err"; do
    if ! grep -q "listening on" "$f" 2>/dev/null; then
        echo "CAPTURE DID NOT START. tcpdump said:"
        cat "/tmp/td_${RUNID}_${TAG}_eth0.err" "/tmp/td_${RUNID}_${TAG}_wlan0.err" 2>/dev/null
        sudo -n pkill -x tcpdump || true
        exit 1
    fi
done
cd ~/tickle/platform/linux
# shellcheck disable=SC2086 # $EXTRA is deliberately word-split - it is either empty or "-a <addr>"
timeout 20 ./publisher -b 255.255.255.255 -I 52 -c 20 -i 0.1 $EXTRA >/tmp/bindscope_pub.log 2>&1 || true
sleep 1
sudo -n pkill -x tcpdump || true
wait 2>/dev/null || true
eth=$(sudo -n tcpdump -r "/tmp/cap_${RUNID}_${TAG}_eth0.pcap"  2>/dev/null | wc -l)
wlan=$(sudo -n tcpdump -r "/tmp/cap_${RUNID}_${TAG}_wlan0.pcap" 2>/dev/null | wc -l)
echo "packets on the wire: eth0=$eth wlan0=$wlan"
grep -E 'traffic:' /tmp/bindscope_pub.log | tail -1 || echo "(publisher printed no traffic line)"
REMOTE
}

run_arm A "control, no bind address" ""
run_arm B "test, data socket bound to $ETH_ADDR" "-a $ETH_ADDR"

say ""
say "=== how to read this: arm A is expected eth0=0 wlan0>0 - that is the leak, reproduced."
say "=== arm B establishes the claim only if wlan0=0 AND eth0>0. wlan0>0 in arm B refutes it."
say "=== eth0=0 in both arms means the publisher never sent, and neither arm says anything."

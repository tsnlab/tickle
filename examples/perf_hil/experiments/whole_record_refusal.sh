#!/usr/bin/env bash
# Which condition refuses a whole-record send? SHM_PLAN 6e(a) names seven, and four rig campaigns were
# spent trying to tell them apart from throughput, which cannot: "the mechanism did nothing" and "the
# mechanism never ran" produce the same number. This asks the publisher instead, while it is alive.
#
# OFF THE RIG, DELIBERATELY. Every one of the seven conditions is publisher-local state - whether the
# publisher batches, how many peers it knows, whether its tx_buffer is clear, whether the peer's segment
# is attached and at the expected address. None of them depends on the rig's hardware, its link or its
# load, so the question is answerable on this PC in seconds. The rig is needed only afterwards, for the
# throughput number, and only once the mechanism is shown to engage.
#
# ONE namespace, both processes, over lo: the segment path exists only between peers that share a host,
# so the veth pair that veth_c6_check.sh uses would rule it out by construction. A private namespace all
# the same, because TickLE nodes in this PC's default namespace reach the rig over the management LAN.
#
# HOW TO READ IT, written before the first run (2026-10-03):
#   one or more "Whole-record send refused: <cause>" lines  -> that is the answer, in the publisher's own
#        words. Each cause says itself once, so several lines mean several distinct causes were met.
#   no refusal line AND datagrams/sample near 1.0           -> the predicate granted; 6e(a) engages here
#        and the rig campaign is now worth spending.
#   no refusal line AND datagrams/sample near 2.0           -> the instrument is mute, NOT the predicate
#        silent. Check the control below before concluding anything at all.
#   CONTROL: the client log must contain "Node open at". It is printed unconditionally at startup, so its
#        absence means this script captured nothing and every reading above is void - which is exactly the
#        failure that wasted 2026-10-03, when the server's log was searched for a line only the publisher
#        prints and the empty result was nearly reported as "it did not refuse".
set -u
DIR=${1:?usage: whole_record_refusal.sh <p4 scenario dir>}
DUR=${DUR:-5}
NS=wholerec-ns-$$
OUT=${OUT:-$HOME/rig_results_safe/whole_record_refusal.txt}
CLIENT_LOG=/tmp/wholerec_client.log
SERVER_LOG=/tmp/wholerec_server.log
mkdir -p "$(dirname "$OUT")"

cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT   # one trap, and only one: a second would silently replace this and leave the namespace

sudo -n ip netns del "$NS" 2>/dev/null
sudo -n ip netns add "$NS" || exit 1
sudo -n ip netns exec "$NS" ip link set lo up || exit 1
# The bench sets _tt_CONFIG.broadcast = "192.168.10.255" (the rig's wired test link), and core refuses to
# create a node when no local interface carries that directed broadcast - which lo does not. So the
# namespace gets a link that does. A veth pair with BOTH ends inside this one namespace, rather than a
# dummy: it is a real broadcast-capable link, and keeping both ends here is what makes the two processes
# same-host peers, which is the entire precondition 6e(a) is being asked about.
sudo -n ip link add wrec0 netns "$NS" type veth peer name wrec1 netns "$NS" || exit 1
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev wrec0 || exit 1
sudo -n ip netns exec "$NS" ip link set wrec0 up
sudo -n ip netns exec "$NS" ip link set wrec1 up

: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== whole-record refusal $(date -Is) dir=$DIR dur=$DUR ==="
say "    $(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && git rev-parse --short HEAD) $(sha256sum "$DIR/client" | cut -c1-16)"

# shellcheck disable=SC2024  # the redirect is this user's, which is what we want - the logs stay readable here
sudo -n ip netns exec "$NS" env BENCH_IFACE=lo "$DIR/server" -Q -d $((DUR + 20)) >"$SERVER_LOG" 2>&1 &
sleep 2
# shellcheck disable=SC2024
sudo -n ip netns exec "$NS" env BENCH_IFACE=lo "$DIR/client" -Q -d "$DUR" >"$CLIENT_LOG" 2>&1
wait 2>/dev/null

say "--- control: did the publisher's log reach us at all? ---"
if grep -q 'Node open' "$CLIENT_LOG"; then
    say "    yes - 'Node open' present, so an absent refusal line is the publisher's silence and not ours"
else
    say "    NO. Nothing was captured; every reading below is void. $CLIENT_LOG:"
    sed 's/^/      | /' "$CLIENT_LOG" | tail -20 | tee -a "$OUT"
    exit 1
fi

say "--- refusal causes, each said once by the publisher ---"
if grep -h 'Whole-record send refused' "$CLIENT_LOG" | sed 's/^/    /' | tee -a "$OUT" | grep -q .; then :; else
    say "    (none - the predicate did not refuse)"
fi

say "--- what it did instead ---"
grep -h '^RESULT' "$CLIENT_LOG" | sed 's/^/    /' | tee -a "$OUT" >/dev/null
awk '/^RESULT/ {
    for (i = 1; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
    if (f["sent"] > 0 && f["tx_shm"] != "") printf "    datagrams/sample %.2f  (tx_shm=%s sent=%s sample_path=%s)\n", f["tx_shm"] / f["sent"], f["tx_shm"], f["sent"], f["sample_path"]
}' "$CLIENT_LOG" | tee -a "$OUT"
say "=== done $(date -Is) ==="

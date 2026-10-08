#!/usr/bin/env bash
# S9 window A on the PC, in a private network namespace (no rig): service_window_a.c says what the window is and what
# each count means. Builds core with tt_MAX_BUFFER_LENGTH=65507, as rmw_tickle does - with the core default (1472) no
# service datagram exceeds a slot and the window cannot be reached at all.
#
# Arms: mixed (one client 3000 B, one 64 B: one path each), shm (both 64 B), udp (both 3000 B). REPS repetitions of
# each, CALLS calls per client per run.
#
# Pre-registered reading, implemented in the summary below, not only written here:
#   VOID   an arm that did not do what it is named for: a run that did not exit 0 or answered fewer than 99% of its
#          calls; mixed without both paths on both sides (oversized_to_udp > 0 and tx_shm > 0, client and server);
#          shm with any oversized datagram; udp with oversized datagrams under 99% of its answers on either side (the
#          first calls go over UDP as unattached, window B, before the attach: the smoke run showed 4-6 of 4000).
#   VOID   a control arm (shm, udp) with any cross inversion: one path is one FIFO, so an inversion there means the
#          instrument counts something other than the path split.
#   DEFECT any stream inversion in any arm: a client's own requests or answers out of order. One outstanding call per
#          client makes that impossible by construction; this is the outcome that would call for a product change.
#   REORDERS     mixed with cross inversions > 0: window A is reached - two clients of one context to one peer are
#                taken in another order than sent. Not a per-stream defect (different endpoints are not ordered with
#                respect to each other, in DDS either); the row says how often.
#   NOT REACHED  mixed with 0 cross inversions: not reached in that many exchanges, which is the claim and no more.
#
# Usage: service_window_a.sh            (REPS=3 CALLS=20000 OUT=... to override). Runs ~1-3 minutes.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
REPS=${REPS:-3}
CALLS=${CALLS:-20000}
OUT=${OUT:-$HOME/tickle_results/service_window_a_$(date +%Y%m%d_%H%M%S).txt}
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/service_window_a.XXXXXX")
NS=tickle_windowa_$$
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -rf "$BUILD"
}
trap cleanup EXIT
mkdir -p "$(dirname "$OUT")"
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

say "=== service_window_a $(date -Is) commit=$(git -C "$REPO" rev-parse --short HEAD) reps=$REPS calls=$CALLS ==="
if ! cc -O2 -std=gnu11 -Dtt_MAX_BUFFER_LENGTH=65507 -I"$REPO/include" -I"$REPO/src" -o "$BUILD/service_window_a" \
    "$REPO/examples/perf_hil/experiments/service_window_a.c" "$REPO/src/tickle.c" "$REPO/src/encoding.c" \
    "$REPO/src/log.c" "$REPO/src/hal_linux.c" -lm >"$BUILD/build.log" 2>&1; then
    say "FATAL build failed"
    tee -a "$OUT" <"$BUILD/build.log"
    exit 3
fi
sudo -n ip netns add "$NS" || { say "FATAL cannot create netns"; exit 3; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo

for rep in $(seq 1 "$REPS"); do
    for arm in mixed shm udp; do
        line=$(sudo -n ip netns exec "$NS" timeout 180 "$BUILD/service_window_a" "$arm" "$CALLS" 2>/dev/null | grep '^RESULT ')
        [ -n "$line" ] || line="RESULT arm=$arm exit=noresult"
        say "rep=$rep $line"
    done
done

python3 - "$OUT" "$CALLS" <<'PYEOF' | tee -a "$OUT"
import re, sys
calls_per_client = int(sys.argv[2])
rows = []
for line in open(sys.argv[1]):
    if " RESULT " not in line:
        continue
    fields = dict(re.findall(r"(\w+)=([\w.,-]+)", line))
    rows.append(fields)
def n(row, key):
    try:
        return int(row.get(key, "-1"))
    except ValueError:
        return -1
void, defect = [], []
totals = {}
for row in rows:
    arm = row.get("arm", "?")
    t = totals.setdefault(arm, {"runs": 0, "answered": 0, "req_cross": 0, "resp_cross": 0, "stream": 0,
                                "oversized": 0, "tx_shm": 0})
    t["runs"] += 1
    calls = n(row, "calls")  # every call made, a timed-out one included
    wanted = 2 * calls_per_client
    if row.get("exit") != "0" or calls <= 0 or n(row, "answered") < 0.99 * wanted:
        void.append(f"{arm} rep {row.get('rep', '?')}: exit={row.get('exit')} answered={n(row, 'answered')} of {wanted}")
        continue
    co, cs, so, ss = (n(row, k) for k in ("client_oversized_to_udp", "client_tx_shm",
                                            "server_oversized_to_udp", "server_tx_shm"))
    if arm == "mixed" and not (co > 0 and cs > 0 and so > 0 and ss > 0):
        void.append(f"mixed did not take both paths: client {co} oversized / {cs} shm, server {so} / {ss}")
    if arm == "shm" and (co > 0 or so > 0):
        void.append(f"shm arm sent oversized datagrams: client {co}, server {so}")
    if arm == "udp" and (co < 0.99 * n(row, "answered") or so < 0.99 * n(row, "answered")):
        void.append(f"udp arm under-oversized: client {co}, server {so} for {n(row, 'answered')} answers")
    cross = n(row, "req_cross") + n(row, "resp_cross")
    if arm in ("shm", "udp") and cross > 0:
        void.append(f"control {arm} shows {cross} cross inversions: the instrument counts something else")
    stream = n(row, "req_stream") + n(row, "resp_stream")
    if stream > 0:
        defect.append(f"{arm}: {stream} stream inversions")
    t["answered"] += n(row, "answered")
    t["req_cross"] += n(row, "req_cross")
    t["resp_cross"] += n(row, "resp_cross")
    t["stream"] += stream
    t["oversized"] += co
    t["tx_shm"] += cs
print()
for arm, t in totals.items():
    print(f"  {arm:5s} runs={t['runs']} exchanges={t['answered']} req_cross={t['req_cross']} "
          f"resp_cross={t['resp_cross']} stream={t['stream']} client_oversized={t['oversized']} client_tx_shm={t['tx_shm']}")
if void:
    print("VERDICT: VOID - " + "; ".join(void))
elif defect:
    print("VERDICT: DEFECT - " + "; ".join(defect))
else:
    m = totals.get("mixed", {})
    cross = m.get("req_cross", 0) + m.get("resp_cross", 0)
    if cross > 0:
        print(f"VERDICT: REORDERS - mixed: {m['req_cross']} requests and {m['resp_cross']} responses taken out of send "
              f"order across the two clients in {m['answered']} exchanges; 0 within any client; controls 0")
    else:
        print(f"VERDICT: NOT REACHED - mixed: 0 cross inversions in {m.get('answered', 0)} exchanges; controls 0")
PYEOF
say "=== done $(date -Is) ==="

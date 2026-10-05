#!/usr/bin/env bash
# Does a KEEP_ALL rmw_tickle publisher tell its subscriber that unacknowledged samples were evicted?
#
# Rig observation (2026-10-05, rmw_keepall_rig.sh): perf_test -c ROS2 --reliable -r 0, history KEEP_ALL (perf_test's
# default), publisher alone on one Pi, subscriber alone on the other, tc netem loss 5% on the publisher's egress,
# 20 s. Every rmw_tickle run lost ~5% of its samples and the subscriber's gap_evicted equalled perf_test's lost,
# while 0% loss lost nothing and rmw_cyclonedds_cpp lost nothing. gap_evicted rises only in
# advance_past_unavailable() (src/tickle.c), i.e. on a Heartbeat whose first_available_seq_no is above the reader's
# ack watermark - which a KEEP_ALL writer may only send for samples every matched reader has acknowledged.
#
# Shape here, off the rig: two private network namespaces joined by a veth pair (different addresses, so core's
# same-host shared-memory path never engages - note_same_host_peer() requires equal IPs), tc netem loss on the
# publisher's veth egress, the same perf_test binary and arguments. Nothing runs in this PC's default namespace.
# rmw_tickle and its typesupport are built from `git archive HEAD` into $W (never the repo's build/ or install/),
# twice: "plain" (Release, as on the rig) and "inst" (Release, -Dtt_RELIABLE_STATS, plus the debug prints that
# rmw_keepall_evict_repro.py adds to a COPY of src/tickle.c).
#
# HOW TO READ IT, written before running and enforced by the analysis at the bottom:
#   VOID run: the subscriber's or publisher's /proc/PID/maps does not show THIS build's librmw_tickle.so; the
#     subscriber printed no "delivery:" line; delivered == 0.
#   CONTROL: the KEEP_ALL 0%-loss arm must show gap_evicted == 0 and lost == 0. If it does not, the whole result is
#     VOID as a reproduction of the rig (the effect is not loss-driven here).
#   REPRODUCED: every valid KEEP_ALL 5%-loss run shows gap_evicted > 0 while the control holds.
#   NOT REPRODUCED: every valid KEEP_ALL 5%-loss run shows gap_evicted == 0 (the rig-only factor is then
#     something this shape lacks: CPU, real NIC, timing).
#   MECHANISM (inst build only), per lossy run:
#     evict_unacked > 0 -> the publisher's cache dropped a sample its slowest matched reader had not acknowledged:
#       a KEEP_ALL admission hole on the writer (keep_all_writable()/reliable_cache_admits() let a write through
#       that reliable_cache_reserve() then made room for by evicting).
#     evict_unacked == 0 and gap_evicted > 0 -> the writer never evicted unacknowledged data, yet told the reader
#       it had: the Heartbeat's first_available_seq_no / the "gone" answer is wrong, not the cache admission.
#
# Usage: rmw_keepall_evict_repro.sh            (builds, then runs ARMS)
#   env: W=/tmp/keepall_evict  SKIP_BUILD=1  REBUILD=inst  QUICK=1  BUILDS="plain inst"  DUR=20  REPS=2
set -o pipefail
REPO=/home/semih/tickle
W="${W:-/tmp/keepall_evict}"
OUT="${OUT:-$W/run_$(date +%Y%m%d-%H%M%S)}"
DUR="${DUR:-20}"; REPS="${REPS:-2}"; BUILDS="${BUILDS:-plain inst}"
NS_P=kae-pub; NS_S=kae-sub; VP=kaev0; VS=kaev1; NET=192.168.77; BCAST=$NET.255
mkdir -p "$OUT"
exec > >(tee -a "$OUT/summary.txt") 2>&1
say() { echo "[$(date +%T)] $*"; }

cleanup() {
    sudo -n ip netns del "$NS_P" 2>/dev/null
    sudo -n ip netns del "$NS_S" 2>/dev/null
}
trap cleanup EXIT

# ---- build --------------------------------------------------------------------------------------------------------
SHA=$(git -C "$REPO" rev-parse --short HEAD)
build_one() { # $1 = plain|inst
    local b=$1 src="$W/src_$1"
    rm -rf "${src:?}" "${W:?}/${b:?}"
    mkdir -p "$src" "$W/$b"
    git -C "$REPO" archive HEAD | tar -x -C "$src" || return 1
    local cflags=""
    if [ "$b" = inst ]; then
        python3 "$REPO/examples/perf_hil/experiments/rmw_keepall_evict_repro.py" "$src/src/tickle.c" "$src/src/hal_linux.c" || return 1
        cflags="-Dtt_RELIABLE_STATS"
    fi
    (
        set +u
        # shellcheck disable=SC1091
        . /opt/ros/lyrical/setup.bash
        # shellcheck disable=SC2030 # set for this build subshell only, on purpose
        export PYTHONPATH="$src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}"
        cd "$W/$b" && colcon build --base-paths "$src" --build-base "$W/$b/build" --install-base "$W/$b/install" \
            --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
            --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=$cflags"
    ) > "$W/build_$b.log" 2>&1 || { say "BUILD FAILED ($b), see $W/build_$b.log"; return 1; }
    [ -f "$W/$b/install/rmw_tickle/lib/librmw_tickle.so" ] || { say "BUILD produced no librmw_tickle.so ($b)"; return 1; }
    say "built $b from $SHA: $(stat -c '%y' "$W/$b/install/rmw_tickle/lib/librmw_tickle.so")"
}
# perf_test's message typesupport must come from this generator: ~/rmw_perf_ws's is older and this rmw_tickle refuses
# it ("callbacks struct reads as 120 bytes, this rmw_tickle's is 144"). Built once, over the plain build; the inst
# build differs only in tickle.c, which no generated code sees.
build_perf() {
    rm -rf "$W/perf"; mkdir -p "$W/perf"
    (
        set +u
        # shellcheck disable=SC1091
        . /opt/ros/lyrical/setup.bash
        # shellcheck disable=SC1091
        . "$W/plain/install/setup.bash"
        # shellcheck disable=SC2031 # this subshell sets its own, independent of the build one
        export PYTHONPATH="$W/src_plain/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}"
        cd "$W/perf" && colcon build --base-paths "$HOME/rmw_perf_ws/src/performance_test" --build-base "$W/perf/build" \
            --install-base "$W/perf/install" --packages-select performance_test \
            --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DPERFORMANCE_TEST_RT_ENABLED=ON \
            -DPERFORMANCE_TEST_CALLBACK_EXECUTOR_ENABLED=ON
    ) > "$W/build_perf.log" 2>&1 || { say "BUILD FAILED (perf_test), see $W/build_perf.log"; return 1; }
    [ -x "$W/perf/install/performance_test/lib/performance_test/perf_test" ] || { say "no perf_test built"; return 1; }
    say "built perf_test over plain"
}
if [ "${SKIP_BUILD:-0}" != 1 ]; then
    for b in $BUILDS; do build_one "$b" || exit 1; done
    build_perf || exit 1
fi
for b in ${REBUILD:-}; do build_one "$b" || exit 1; done   # e.g. SKIP_BUILD=1 REBUILD=inst after changing the .py

# ---- netns: two namespaces, one veth, netem on the publisher's side -------------------------------------------------
cleanup
if ! sudo -n ip netns add "$NS_P" || ! sudo -n ip netns add "$NS_S"; then say "FATAL cannot create netns"; exit 1; fi
sudo -n ip link add "$VP" netns "$NS_P" type veth peer name "$VS" netns "$NS_S" || { say "FATAL veth"; exit 1; }
for pair in "$NS_P $VP 1" "$NS_S $VS 2"; do
    read -r ns dev host <<<"$pair"
    sudo -n ip netns exec "$ns" ip link set lo up
    sudo -n ip netns exec "$ns" ip addr add "$NET.$host/24" broadcast "$BCAST" dev "$dev"
    sudo -n ip netns exec "$ns" ip link set "$dev" up
    sudo -n ip netns exec "$ns" ip route add default dev "$dev"
done
set_loss() { # $1 percent
    sudo -n ip netns exec "$NS_P" tc qdisc del dev "$VP" root 2>/dev/null
    if [ "$1" != 0 ]; then sudo -n ip netns exec "$NS_P" tc qdisc add dev "$VP" root netem loss "$1%" || return 1; fi
    sudo -n ip netns exec "$NS_P" tc qdisc show dev "$VP" | head -1
}

# The launcher runs inside a namespace (as root - ip netns exec needs it): sources the environment, starts perf_test,
# captures ITS OWN pid via $!, records which librmw_tickle.so that pid mapped, and waits for it.
LAUNCH="$OUT/launch.sh"
cat > "$LAUNCH" <<'EOF'
#!/bin/bash
b=$1; stem=$2; shift 2
set +u
. /opt/ros/lyrical/setup.bash
. "$W/perf/install/setup.bash"
. "$W/$b/install/local_setup.bash"
export AMENT_PREFIX_PATH="$W/$b/install/rmw_tickle:$AMENT_PREFIX_PATH"
export LD_LIBRARY_PATH="$W/$b/install/rmw_tickle/lib:$W/$b/install/rosidl_typesupport_tickle_c/lib:$W/$b/install/rosidl_typesupport_tickle_cpp/lib:${LD_LIBRARY_PATH:-}"
export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=$BCAST
cd "$OUT"
"$W/perf/install/performance_test/lib/performance_test/perf_test" "$@" > "$OUT/$stem.log" 2>&1 < /dev/null &
pid=$!
sleep 3
grep -o '/[^ ]*librmw_tickle[^ ]*\.so' "/proc/$pid/maps" 2>/dev/null | sort -u > "$OUT/$stem.maps"
wait "$pid"
echo "EXIT $?" >> "$OUT/$stem.log"
EOF
chmod +x "$LAUNCH"

run_one() { # build keep(ka|kl) topic loss rep
    local b=$1 keep=$2 topic=$3 loss=$4 rep=$5 stem="$1_$2_$3_l$4_r$5"
    local common=(-c ROS2 -t "$topic" --reliable) hist=()
    [ "$keep" = kl ] && hist=(--keep_last --history_depth 1000)
    local rosargs=(--ros-args --param start_type_description_service:=false)
    set_loss "$loss" > "$OUT/${stem}_tc.txt" || { say "tc failed"; exit 1; }
    sudo -n ip netns exec "$NS_S" env W="$W" OUT="$OUT" BCAST="$BCAST" HOME="$HOME" TICKLE_NODE_ID=102 \
        "$LAUNCH" "$b" "${stem}_sub" "${common[@]}" "${hist[@]}" -p 0 -s 1 --expected_num_pubs 1 \
        --max_runtime $((DUR + 8)) "${rosargs[@]}" &
    local sp=$!
    sleep 2
    sudo -n ip netns exec "$NS_P" env W="$W" OUT="$OUT" BCAST="$BCAST" HOME="$HOME" TICKLE_NODE_ID=101 \
        "$LAUNCH" "$b" "${stem}_pub" "${common[@]}" "${hist[@]}" -r 0 -p 1 -s 0 --expected_num_subs 1 \
        --max_runtime "$DUR" "${rosargs[@]}"
    wait "$sp"
    echo "RUN $stem want=$W/$b/install/rmw_tickle/lib/librmw_tickle.so tc=[$(cat "$OUT/${stem}_tc.txt")]" >> "$OUT/index.txt"
    say "  $stem done"
}

say "=== rmw_keepall_evict_repro $SHA, DUR=$DUR REPS=$REPS BUILDS=$BUILDS, out $OUT ==="
for b in $BUILDS; do
    run_one "$b" ka Array1k 0 1                       # control
    [ "${QUICK:-0}" = 1 ] && continue                 # discovery diagnosis only: the control run is enough
    for rep in $(seq 1 "$REPS"); do run_one "$b" ka Array1k 5 "$rep"; done
    run_one "$b" ka Array4k 5 1
    [ "$b" = plain ] && run_one "$b" kl Array1k 5 1   # KEEP_LAST: evictions expected, a positive control of the counter
done
set_loss 0 > /dev/null
say "=== runs done ==="

python3 - "$OUT" <<'PY'
import re, sys
from pathlib import Path
out = Path(sys.argv[1])

def perf_rows(text):
    lines = text.splitlines()
    if "---EXPERIMENT-START---" not in lines:
        return []
    i = lines.index("---EXPERIMENT-START---")
    hdr = [h.strip() for h in lines[i + 1].split(",") if h.strip()]
    rows = []
    for l in lines[i + 2:]:
        c = [x.strip() for x in l.split(",") if x.strip()]
        if len(c) >= len(hdr) and re.match(r"^[0-9.]+$", c[0]):
            try:
                rows.append({h: float(v) for h, v in zip(hdr, c)})
            except ValueError:
                pass
    return rows

def col(rows, name):
    return int(sum(r.get(name, 0) for r in rows))

results = []
for line in (out / "index.txt").read_text().splitlines():
    m = re.match(r"RUN (\S+) want=(\S+)", line)
    stem, want = m.group(1), m.group(2)
    pub = (out / f"{stem}_pub.log").read_text(errors="replace") if (out / f"{stem}_pub.log").exists() else ""
    sub = (out / f"{stem}_sub.log").read_text(errors="replace") if (out / f"{stem}_sub.log").exists() else ""
    void = []
    for role in ("pub", "sub"):
        maps = (out / f"{stem}_{role}.maps").read_text().split() if (out / f"{stem}_{role}.maps").exists() else []
        if want not in maps:
            void.append(f"{role} mapped {maps or 'nothing'}")
    d = re.findall(r"delivery: (.*)", sub)
    counters = dict(kv.split("=") for kv in d[-1].split() if "=" in kv) if d else {}
    if not d:
        void.append("no delivery line")
    elif int(counters.get("delivered", 0)) == 0:
        void.append("delivered=0")
    sent = col(perf_rows(pub), "sent")
    srows = perf_rows(sub)
    recv, lost = col(srows, "received"), col(srows, "lost")
    failed = "failed to publish" in pub
    dbg = {}
    for txt in (pub, sub):
        for e in re.findall(r"KEEPALL_DBG exit (.*)", txt)[-1:]:
            pass
    pdbg = re.findall(r"KEEPALL_DBG exit (.*)", pub)
    pd = dict(kv.split("=") for kv in pdbg[-1].split()) if pdbg else {}
    r = dict(stem=stem, void=void, sent=sent, recv=recv, lost=lost, failed=failed,
             gap_evicted=int(counters.get("gap_evicted", -1)), delivered=int(counters.get("delivered", -1)),
             evict_unacked=pd.get("evict_unacked"), publish_refused=pd.get("publish_refused"),
             evicted_by_count=pd.get("evicted_by_count"), evicted_by_bytes=pd.get("evicted_by_bytes"),
             eviction_heartbeats=pd.get("eviction_heartbeats"), null_evicted=pd.get("null_evicted"))
    results.append(r)
    print(f"{stem:28s} {'VOID(' + '; '.join(void) + ')' if void else 'valid'} sent={sent} recv={recv} lost={lost} "
          f"delivered={r['delivered']} gap_evicted={r['gap_evicted']} failed_publish={failed} "
          + (f"evict_unacked={r['evict_unacked']} evicted_by_count={r['evicted_by_count']} "
             f"evicted_by_bytes={r['evicted_by_bytes']} publish_refused={r['publish_refused']} "
             f"eviction_heartbeats={r['eviction_heartbeats']} null_evicted={r['null_evicted']}" if pd else ""))

print("--- verdict per build ---")
for b in sorted({r["stem"].split("_")[0] for r in results}):
    rs = [r for r in results if r["stem"].startswith(b + "_")]
    ctrl = [r for r in rs if "_ka_Array1k_l0_" in r["stem"]]
    lossy = [r for r in rs if "_ka_" in r["stem"] and "_l5_" in r["stem"] and not r["void"]]
    if not ctrl or ctrl[0]["void"]:
        print(f"{b}: VOID - control run missing or void"); continue
    if ctrl[0]["gap_evicted"] != 0 or ctrl[0]["lost"] != 0:
        print(f"{b}: VOID as a rig reproduction - control shows gap_evicted={ctrl[0]['gap_evicted']} lost={ctrl[0]['lost']}"); continue
    if not lossy:
        print(f"{b}: VOID - no valid lossy KEEP_ALL run"); continue
    if all(r["gap_evicted"] > 0 for r in lossy):
        print(f"{b}: REPRODUCED - every lossy KEEP_ALL run gap_evicted>0 ({[r['gap_evicted'] for r in lossy]}), control 0")
    elif all(r["gap_evicted"] == 0 for r in lossy):
        print(f"{b}: NOT REPRODUCED - lossy KEEP_ALL gap_evicted all 0")
    else:
        print(f"{b}: MIXED - lossy KEEP_ALL gap_evicted {[r['gap_evicted'] for r in lossy]}")
    for r in lossy:
        if r["evict_unacked"] is None:
            continue
        if int(r["evict_unacked"]) > 0:
            print(f"  {r['stem']}: MECHANISM writer cache evicted {r['evict_unacked']} unacknowledged samples")
        elif r["gap_evicted"] > 0:
            print(f"  {r['stem']}: MECHANISM writer evicted nothing unacknowledged, yet the reader skipped {r['gap_evicted']}")
PY

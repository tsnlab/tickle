#!/usr/bin/env bash
# Why did a reliable rmw_tickle subscriber see a RELIABLE KEEP_ALL publisher as BEST_EFFORT ("RxO mismatch ...
# offered reliable=0", rig 2026-10-05 11:29, 1 of 36 rmw_keepall_rig.sh runs) and drop every sample?
#
# HYPOTHESIS H1 (from reading the code). rmw_create_publisher() (rmw_tickle/src/rmw_publisher.c) calls
# tt_Node_create_publisher() under the context lock and unlocks; node_create_publisher_locked() (src/tickle.c)
# resets pub->reliable/keep_all/durable/deadline/lease to their defaults and arms announce_soon for
# tt_CONTEXT_TX_INTERVAL (1 ms) later. Only AFTER the unlock does setup_reliable_cache() set
# tickle_publisher.reliable/keep_all (and later deadline/lease) - with no lock held. If rmw_tickle's poll thread runs
# announce_soon inside that window, the announce carries the publisher with qos 0; the subscriber records qos 0 in its
# discovery table (upsert_discovered_entity()), and nothing re-announces the entity afterwards because setting a field
# does not touch last_modified. Every DATA is then dropped by subscriber_incompatible_with_writer().
#
# The window is normally microseconds (an allocation or two), so the natural rate is low. This harness widens it
# WITHOUT touching production code: an LD_PRELOAD shim (built here from the C below) interposes the PLT calls
# librmw_tickle.so makes to tt_Node_create_publisher() and tt_Context_unlock(), and sleeps DELAY_MS on the creating
# thread at one of two places:
#   after_unlock  - right after the tt_Context_unlock() that follows creating the /rxo_probe publisher, i.e. INSIDE the
#                   window (lock released, reliable not yet set). The poll thread is free to announce.
#   after_create  - right after rmw_create_publisher() returns (rcl's call into librmw_implementation, interposed):
#                   the same delay on the same thread, the poll thread just as free to announce, but every QoS field
#                   is final by then. The control for "a delay after creation causes it".
#   before_create - inside tt_Node_create_publisher() before the real call, with the context lock held. NOT a clean
#                   control (found in the smoke run, 2026-10-05 11:44): the poll thread is blocked for the delay and
#                   works off its backlog the moment the lock is released - i.e. inside the window. Reported, not scored.
#
# ARMS (each trial: subscriber first, publisher LEAD_S later, both rxo_mismatch_repro.py, 4 KiB samples, in one private netns):
#   N  natural       no shim, --reliable                     the unperturbed rate on this PC
#   T  treatment     after_unlock DELAY_MS, --reliable        H1 predicts a mismatch in (nearly) every trial
#   C1 control       after_create DELAY_MS, reliable          H1 predicts no mismatch (same delay, after the window)
#   C2 control       after_unlock DELAY_MS, best effort both  must never mismatch: subscriber requests nothing
#   C3 side arm      before_create DELAY_MS, reliable         reported only (see before_create above)
#
# READING RULES (implemented in the summary at the end, not only here):
#   A trial is VOID if: (a) an arm with a shim did not print "RXO_SHIM applied" in the publisher log (the arm did not
#     apply its treatment), (b) the publisher's /proc/PID/maps did not show this checkout's librmw_tickle.so, or
#     (c) the subscriber log has no core "Subscriber N delivery: ... rxo_drops=" end-of-run line (it never ran to the end). Void trials are counted, not scored.
#   A trial MISMATCHES if the subscriber log contains "RxO mismatch" (the core dropping DATA). rclcpp's own
#     "offering incompatible QoS" warning on the subscriber (the discovery-level view of the same entry) is reported
#     per trial as rclcpp_incompat but not scored. The publisher publishes even if it never matches (after 1 s).
#   The arm is VOID as a whole if fewer than half its trials are valid.
#   C2 > 0 mismatches            -> HARNESS VOID: a mismatch that is not about reliability; nothing can be read.
#   T  >= 80% and C1 <= 10%      -> H1 SUPPORTED: the announce-in-the-window race is a sufficient mechanism.
#   T  >= 80% and C1 >  10%      -> H1 NOT SPECIFIC: the delay itself, wherever it is, produces it.
#   T  <  80%                    -> H1 NOT SUPPORTED as the sole mechanism (a widened window does not reliably do it).
#   N is reported as a rate; any N > 0 is a natural reproduction on this PC, N = 0 says nothing by itself.
#
# Usage: [N_NAT=150] [N_ARM=25] [DELAY_MS=20] [LEAD_S=2] [PUB_S=3] rxo_mismatch_repro.sh [OUTDIR]
# Default OUTDIR /tmp/rxo_mismatch_repro; one dir per trial, a TRIAL line per trial in OUTDIR/trials.txt,
# the verdict at the end of stdout. Runs in a private netns only (CLAUDE.md: never in the default netns).
set -u
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=${1:-/tmp/rxo_mismatch_repro}
N_NAT=${N_NAT:-150}
N_ARM=${N_ARM:-25}
DELAY_MS=${DELAY_MS:-20}
LEAD_S=${LEAD_S:-2}
PUB_S=${PUB_S:-2}
ARMS=${ARMS:-"T C1 C2 C3 N"}
NS=tt-rxo-$$
# Not perf_test: this PC's performance_test interfaces were generated by an older rosidl_typesupport_tickle_c (their
# callbacks struct reads 120 bytes, the current rmw_tickle's 144) and it refuses to create the publisher. The probe
# is rclpy with std_msgs/UInt8MultiArray of 4096 bytes, KEEP_ALL depth 1000, from the acceptance interface workspace;
# it reaches the same rmw_create_publisher() and the same poll thread.
PY=$REPO/examples/perf_hil/experiments/rxo_mismatch_repro.py
IFACES=${IFACES:-$HOME/rmw_accept_ws/ifaces/install}
LIB=$REPO/install/rmw_tickle/lib/librmw_tickle.so
mkdir -p "$OUT"
: >"$OUT/trials.txt"
say() { echo "[$(date +%T)] $*"; }

cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
}
trap cleanup EXIT

# --- the shim -------------------------------------------------------------------------------------------------
cat >"$OUT/rxo_shim.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static __thread int armed;
/* librmw_tickle.so is dlopen()ed RTLD_LOCAL by rmw_implementation, so RTLD_NEXT cannot see it: look the real
   functions up in that library by its path (RXO_SHIM_LIB), which is already loaded when it calls us. */
static void* real_sym(const char* name) {
    void* h = dlopen(getenv("RXO_SHIM_LIB"), RTLD_NOLOAD | RTLD_LAZY);
    void* f = h != NULL ? dlsym(h, name) : NULL;
    if (f == NULL) { fprintf(stderr, "RXO_SHIM cannot find %s\n", name); abort(); }
    return f;
}
static int mode(void) { /* 0 off, 1 after_unlock, 2 before_create, 3 after_create */
    const char* m = getenv("RXO_SHIM_MODE");
    if (m == NULL) return 0;
    if (strcmp(m, "after_unlock") == 0) return 1;
    if (strcmp(m, "before_create") == 0) return 2;
    if (strcmp(m, "after_create") == 0) return 3;
    return 0;
}
static void delay(const char* where) {
    const char* d = getenv("RXO_SHIM_DELAY_MS");
    long ms = d != NULL ? atol(d) : 20;
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    fprintf(stderr, "RXO_SHIM applied %s %ld ms\n", where, ms);
}
int tt_Node_create_publisher(void* node, void* pub, void* topic, const char* name) {
    static int (*real)(void*, void*, void*, const char*);
    if (real == NULL) {
        real = (int (*)(void*, void*, void*, const char*))real_sym("tt_Node_create_publisher");
        Dl_info info;
        if (real != NULL && dladdr((void*)real, &info) != 0) fprintf(stderr, "RXO_SHIM real in %s\n", info.dli_fname);
    }
    int match = name != NULL && strstr(name, "rxo_probe") != NULL;
    if (match && mode() == 2) delay("before_create");
    int r = real(node, pub, topic, name);
    if (match && mode() == 1) armed = 1;
    return r;
}
/* Interposes rcl's call to rmw_create_publisher (librmw_implementation's forwarder) and calls rmw_tickle's own
   directly, which is all the forwarder does (RTLD_NEXT through the forwarder re-entered this shim and crashed). */
void* rmw_create_publisher(const void* node, const void* ts, const char* topic, const void* qos, const void* opts) {
    static void* (*real)(const void*, const void*, const char*, const void*, const void*);
    if (real == NULL) real = (void* (*)(const void*, const void*, const char*, const void*, const void*))real_sym("rmw_create_publisher");
    void* r = real(node, ts, topic, qos, opts);
    if (topic != NULL && strstr(topic, "rxo_probe") != NULL && mode() == 3) delay("after_create");
    return r;
}
void tt_Context_unlock(void* ctx) {
    static void (*real)(void*);
    if (real == NULL) real = (void (*)(void*))real_sym("tt_Context_unlock");
    real(ctx);
    if (armed) {
        armed = 0;
        delay("after_unlock");
    }
}
EOF
gcc -O1 -shared -fPIC -o "$OUT/rxo_shim.so" "$OUT/rxo_shim.c" -ldl || { echo "FATAL shim build"; exit 1; }

# --- the netns ------------------------------------------------------------------------------------------------
sudo -n ip netns add "$NS" || { echo "FATAL cannot create netns"; exit 1; }
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" route add default dev lo
sudo -n ip -n "$NS" link add bench0 type dummy
sudo -n ip -n "$NS" addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip -n "$NS" link set bench0 up

ENVSETUP="set +u; . /opt/ros/lyrical/setup.bash; . $IFACES/local_setup.bash; . $REPO/install/local_setup.bash
export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_DOMAIN_ID=7"

# in_ns <logfile> <command string>: runs as this user inside the netns, in the background; echoes nothing.
in_ns() {
    local log=$1
    shift
    # shellcheck disable=SC2024 # the redirect is meant to stay ours: the process runs as this user via setpriv
    sudo -n ip netns exec "$NS" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups \
        env HOME="$HOME" bash -c "$ENVSETUP
$*" >"$log" 2>&1 </dev/null &
}

trial() { # arm index
    local arm=$1 i=$2 dir="$OUT/$1_$2" mode=off qos=reliable pre=""
    case $arm in
    N) ;;
    T) mode=after_unlock ;;
    C1) mode=after_create ;;
    C2) mode=after_unlock qos=best_effort ;;
    C3) mode=before_create ;;
    esac
    [ "$mode" != off ] && pre="export LD_PRELOAD=$OUT/rxo_shim.so RXO_SHIM_LIB=$LIB RXO_SHIM_MODE=$mode RXO_SHIM_DELAY_MS=$DELAY_MS"
    mkdir -p "$dir"
    local subrt=$((LEAD_S + PUB_S + 1))
    in_ns "$dir/sub.log" "echo \$\$ > $dir/sub.pid
exec python3 $PY sub $qos $subrt"
    sleep "$LEAD_S"
    # The publisher writes its own PID first, so its maps are read from that PID and not found by a pattern.
    in_ns "$dir/pub.log" "$pre
echo \$\$ > $dir/pub.pid
( while [ -d /proc/\$\$ ]; do grep -o '/[^ ]*librmw_tickle[a-z_]*\.so' /proc/\$\$/maps >> $dir/pub.maps; sleep 0.1; done ) &
exec python3 $PY pub $qos $PUB_S"
    # Wait for both to end by themselves (each wrote its own PID), up to 5 s past the subscriber's deadline.
    local p
    for _ in $(seq 1 $(((subrt - LEAD_S + 5) * 10))); do
        local alive=0
        for p in "$dir/sub.pid" "$dir/pub.pid"; do
            p=$(cat "$p" 2>/dev/null) && [ -d "/proc/$p" ] && alive=1
        done
        [ $alive = 0 ] && break
        sleep 0.1
    done
    # Each probe ends by its own deadline; one that did not is interrupted by the PID it wrote itself, after checking
    # that PID is still a python3. Then wait for the netns to be empty before the next trial (/dev/shm is shared).
    for p in "$dir/sub.pid" "$dir/pub.pid"; do
        p=$(cat "$p" 2>/dev/null) || continue
        case "$(readlink "/proc/$p/exe" 2>/dev/null)" in */python3*) kill -INT "$p" && echo "killed $p" >>"$dir/killed" ;; esac
    done
    for _ in $(seq 1 20); do
        [ -z "$(sudo -n ip netns pids "$NS")" ] && break
        sleep 0.5
    done
    local left
    left=$(sudo -n ip netns pids "$NS" | wc -l)
    local applied=1 lib=0 ran=0 mm=0
    if [ "$mode" != off ]; then grep -q "RXO_SHIM applied" "$dir/pub.log" || applied=0; fi
    grep -qF "$LIB" "$dir/pub.maps" 2>/dev/null && lib=1
    # The subscriber's own end-of-run line, printed whatever happened: its absence means it never ran to the end.
    grep -q "Subscriber [0-9]* delivery:.*rxo_drops=" "$dir/sub.log" && ran=1
    grep -q "RxO mismatch" "$dir/sub.log" && mm=1
    local offered gaveup recv sent drops
    offered=$(grep -m1 -o "offered reliable=[0-9] durable=[0-9] manual=[0-9]" "$dir/sub.log" | tr ' ' ',')
    gaveup=$(grep -c "gave up" "$dir/pub.log")
    recv=$(grep -o "RXO_PY sub done received=[0-9]*" "$dir/sub.log" | grep -o "received=[0-9]*")
    sent=$(grep -o "RXO_PY pub done sent=[0-9]*" "$dir/pub.log" | grep -o "sent=[0-9]*")
    drops=$(grep -o "delivery: delivered=[0-9]*.* rxo_drops=[0-9]*" "$dir/sub.log" | grep -o "rxo_drops=[0-9]*")
    local incompat matched
    incompat=$(grep -c "offering incompatible QoS" "$dir/sub.log")
    matched=$(grep -o "matched=[0-9]*" "$dir/pub.log" | tail -n 1)
    echo "TRIAL arm=$arm i=$i applied=$applied lib=$lib ran=$ran mismatch=$mm offered=${offered:-none} pub_gave_up=$gaveup ${recv:-received=?} ${sent:-sent=?} ${drops:-rxo_drops=?} rclcpp_incompat=$incompat ${matched:-matched=?} left=$left" |
        tee -a "$OUT/trials.txt"
}

say "=== rxo_mismatch_repro: arms $ARMS, N_NAT=$N_NAT N_ARM=$N_ARM DELAY_MS=$DELAY_MS LEAD_S=$LEAD_S PUB_S=$PUB_S lib $LIB"
for arm in $ARMS; do
    n=$N_ARM
    [ "$arm" = N ] && n=$N_NAT
    for i in $(seq 1 "$n"); do trial "$arm" "$i"; done
    say "arm $arm done"
done

python3 - "$OUT/trials.txt" <<'EOF'
import re, sys
rows = [dict(kv.split('=', 1) for kv in l.split()[1:]) for l in open(sys.argv[1]) if l.startswith('TRIAL')]
res = {}
for arm in ('N', 'T', 'C1', 'C2', 'C3'):
    rs = [r for r in rows if r['arm'] == arm]
    valid = [r for r in rs if r['applied'] == '1' and r['lib'] == '1' and r['ran'] == '1']
    mm = sum(r['mismatch'] == '1' for r in valid)
    offered = sorted({r['offered'] for r in valid if r['mismatch'] == '1'})
    inc = sum(r.get('rclcpp_incompat', '0') != '0' for r in valid)
    res[arm] = (len(rs), len(valid), mm)
    print(f'ARM {arm}: trials={len(rs)} valid={len(valid)} void={len(rs)-len(valid)} mismatch={mm} '
          f'rate={(mm/len(valid) if valid else float("nan")):.3f} offered={offered} rclcpp_incompat_trials={inc}')
def ok(arm):
    t, v, _ = res.get(arm, (0, 0, 0))
    return t > 0 and v * 2 >= t
def rate(arm):
    _, v, m = res[arm]
    return m / v
if not (ok('T') and ok('C1') and ok('C2')):
    print('VERDICT: VOID - an arm has fewer than half its trials valid (or did not run)')
elif res['C2'][2] > 0:
    print('VERDICT: HARNESS VOID - the best-effort control mismatched')
elif rate('T') >= 0.8 and rate('C1') <= 0.1:
    print('VERDICT: H1 SUPPORTED - an announce inside the post-unlock window produces the mismatch; the same delay outside it does not')
elif rate('T') >= 0.8:
    print('VERDICT: H1 NOT SPECIFIC - the delay produces it wherever it is')
else:
    print('VERDICT: H1 NOT SUPPORTED as the sole mechanism')
if ok('N'):
    print(f'NATURAL: {res["N"][2]} of {res["N"][1]} valid unperturbed trials mismatched')
if ok('C3'):
    print(f'SIDE C3 (lock held {res["C3"][1] and "during the delay"}): {res["C3"][2]} of {res["C3"][1]} valid trials mismatched')
EOF
echo ALL_DONE

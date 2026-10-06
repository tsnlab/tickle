#!/usr/bin/env bash
# Is the p4 same-host latency gap (ROADMAP Now 1, RESULTS S10) the first touch of the reliable reorder ring, and not
# idling at all?
#
# WHAT WAS SEEN. p4_interval_rig.sh: TickLE's p4 - p3 RTT is +6 us at 0.5 ms ping spacing and +20 us at 5 ms and
# 20 ms; CycloneDDS's is 0-2 us. Read as "the extra time appears after idling". But that harness ran each interval
# for max(10 s, 2000 pings): about 18,000 round trips at 0.5 ms and about 2,000 at 5 ms and 20 ms. The interval and
# the number of round trips changed together.
#
# THE HYPOTHESIS (H-RING). A RELIABLE subscriber stores every fragment, in order or not, in its reorder slot
# seq_no % reorder_slots (accept_reliable_fragment -> reorder_store_fragment, DATAFRAG_PLAN 13), and only then copies
# the sample out (drain_fragmented_sample). The bench gives each side tt_RELIABLE_BITMAP_MAX_BITS = 4096 slots of
# tt_REORDER_SLOT_SIZE(2816) = 2840 B, an 11.6 MB static array (.bss; it is the +11 MB in p4's peak_rss_kb). A p4
# sample is two seq_nos, so every round trip writes two fresh slots per side: 5.7 KB never touched before, i.e. page
# faults (zero-fill) and DRAM misses, for the first 2048 samples - one lap of the ring. A p3 sample is a DATA,
# delivered in order straight from the receive buffer, and never touches the ring. So a run of 2,000 round trips is
# entirely first lap, and a run of 18,000 is 11% first lap: the "interval" effect is the run length.
#
# ARMS: size {p3, p4} x {0.5 ms first lap, 5 ms short (< 1 lap), 5 ms long (> 3 laps)} x prefault {none, pre}.
#   The 0.5 ms arm cannot be run long: at c1ebf43e the client stops pinging after ~124 pings at any interval below
#   ~4 ms, because each ping schedules a 500 ms ping_timeout that is never cancelled and the scheduler holds
#   tt_MAX_SCHEDULER_LENGTH = 128 entries; tt_Context_schedule()'s refusal of the next ping is ignored. (Found by
#   v1 of this harness, 2026-10-06 13:35, whose 0.5 ms "long" arm therefore ran 124 pings like its "short" one.)
#   124 pings is inside the first lap, which is what the 0.5 ms arm is for: short idle, fresh ring.
#   pre = LD_PRELOAD prefault_rw.so on both processes: MADV_POPULATE_WRITE on every private writable mapping before
#   main(), so the ring's pages are present (but still not in any cache) before the first ping.
#   p3 is the CONTROL: it never writes the ring, so neither length nor prefault can move it.
#
# HOW TO READ IT, written before running and enforced in the analysis below. gap = median over reps of the p4 arm's
# rtt_p50 minus the p3 arm's rtt_p50 (same interval, length, prefault). rtt_avg gaps are printed beside them.
#   TREATMENT: every pre arm must print PREFAULT with failed=0 and populated_bytes >= the bench's .bss (p4 11e6,
#     p3 5e6), on client and server, else that arm is VOID. Every arm's RESULT must carry the size's sample_bytes
#     (1424 / 2800) and the client binary's sha256 must be the one built for that size, else VOID.
#   CONTROL: p3's p50 must move by <= 3 us between short and long and between none and pre, at each interval.
#     If it moves more, the verdict is printed but marked CONTROL MOVED and is not to be read.
#   REALIZED: an arm's round trips must put it where it claims on the ring: 0.5 ms 100..2048, 5 ms short
#     1500..2048 (inside lap one), 5 ms long >= 6000 (past lap three); otherwise VOID.
#   H-RING SUPPORTED if all four hold:
#     (1) untreated 5 ms short p4 takes >= 0.5 more page faults per round trip per process than p3;
#     (2) prefault shrinks the 5 ms short gap by >= 8 us;
#     (3) at 5 ms, the short run's gap exceeds the long run's by >= 8 us (run length decides at a fixed idle);
#     (4) at 0.5 ms on the first lap the untreated gap is >= 12 us (short idle does not avoid it; the old 0.5 ms
#         reading of +6 us came from an 18,000-round-trip run, mostly past lap one).
#   H-IDLE (the old reading) if prefault moves the 5 ms short gap by <= 3 us AND the 5 ms short and long gaps are
#     within 3 us AND the 0.5 ms gap is <= 8 us: the cost follows idling, whatever the ring's pages are.
#   PARTIAL otherwise, with the numbers. gap(5 ms long pre) is printed as the steady-state p4 - p3 cost that
#     neither faults nor this hypothesis explain.
#
# Usage: SHA=<pushed sha> [REPS=3] [HOST=10.1.1.214] p4_reorder_firsttouch.sh
# Launch detached; results in ~/rig_results_safe/.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${SHA:?set SHA to a pushed commit}
REPS=${REPS:-3}
HOST=${HOST:-10.1.1.214}
OUT=${OUT:-$HOME/rig_results_safe/p4_reorder_firsttouch_${SHA}_$(date +%Y%m%d-%H%M%S).txt}
K=$HOME/.ssh/tickle_ci_ed25519
D=/home/ci/p4ft   # binaries and logs on the Pi
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
say() { echo "$*" | tee -a "$OUT"; }
: >"$OUT"

# One EXIT trap: stop a server this script started, by the PID captured at launch and its executable, never a pattern.
srv_wrap=""
stop_server() {
    [ -z "$srv_wrap" ] && return 0
    # srv_wrap is the PID captured at launch (the rusage wrapper, which forwards SIGINT to the server and then prints
    # the server's RUSAGE). Signalled only while its executable is still a python interpreter, so a recycled PID
    # cannot be hit.
    sh_ "case \"\$(readlink /proc/$srv_wrap/exe 2>/dev/null)\" in */python3*) kill -INT $srv_wrap;; esac
for i in 1 2 3 4 5 6 7 8; do [ -d /proc/$srv_wrap ] || break; sleep 1; done
case \"\$(readlink /proc/$srv_wrap/exe 2>/dev/null)\" in */python3*) kill -TERM $srv_wrap;; esac; true" </dev/null >/dev/null 2>&1
    srv_wrap=""
}
trap stop_server EXIT

say "=== p4 reorder first-touch, sha=$SHA host=$HOST reps=$REPS $(date -Is) ==="
# Build both sizes at SHA on the Pi, plus the preload and the rusage wrapper (uncommitted harness files, copied).
out=$(sh_ "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
mkdir -p $D/p3 $D/p4
cd examples/perf_hil/tickle
for sz in p3 p4; do ./build.sh reliable_latency \$sz > $D/build_\$sz.log 2>&1 || { echo BUILD_FAILED \$sz; tail -5 $D/build_\$sz.log; exit 0; }
  cp reliable_latency_\$sz/client reliable_latency_\$sz/server $D/\$sz/; done
echo SHA256 p3 \$(sha256sum $D/p3/client | cut -c1-16) p4 \$(sha256sum $D/p4/client | cut -c1-16)
echo PAGESIZE \$(getconf PAGESIZE) KERNEL \$(uname -r) GOV \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
echo CPUIDLE \$(cat /sys/devices/system/cpu/cpu0/cpuidle/state*/name 2>/dev/null | tr '\n' ' ')
size $D/p3/client $D/p4/client | tail -2 | awk '{print \"BSS\", \$6, \$3}'" </dev/null 2>&1)
say "$out"
case "$out" in *BUILD_FAILED*|*error:*) say "FATAL build failed"; exit 1;; esac
sha_p3=$(printf '%s\n' "$out" | awk '/^SHA256/{print $3}')
sha_p4=$(printf '%s\n' "$out" | awk '/^SHA256/{print $5}')
[ -n "$sha_p3" ] && [ -n "$sha_p4" ] && [ "$sha_p3" != "$sha_p4" ] || { say "FATAL no distinct p3/p4 binaries"; exit 1; }
sh_ "cat > $D/prefault_rw.c" <"$REPO/examples/perf_hil/experiments/prefault_rw.c" || { say "FATAL copy"; exit 1; }
sh_ "cat > $D/rusage_run.py" <"$REPO/examples/perf_hil/experiments/rusage_run.py" || { say "FATAL copy"; exit 1; }
out=$(sh_ "cc -O2 -shared -fPIC -o $D/prefault_rw.so $D/prefault_rw.c && echo PRELOAD_OK" </dev/null 2>&1)
say "$out"
case "$out" in *PRELOAD_OK*) ;; *) say "FATAL preload build"; exit 1;; esac

run_arm() { # run_arm <rep> <size> <interval> <len:short|long> <dur> <pf:none|pre>
    local rep=$1 sz=$2 iv=$3 len=$4 dur=$5 pf=$6 pre="" csha clog slog tag
    [ "$pf" = pre ] && pre="LD_PRELOAD=$D/prefault_rw.so"
    tag="rep=$rep size=$sz iv=$iv len=$len pf=$pf"
    csha=$(sh_ "sha256sum $D/$sz/client | cut -c1-16" </dev/null)
    srv_wrap=$(sh_ "cd $D/$sz && rm -f $D/srv.pid
(setsid sh -c 'echo \$\$ > $D/srv.pid; exec python3 $D/rusage_run.py env BENCH_IFACE=lo $pre ./server -Q -d $((dur + 40))' > $D/srv.log 2>&1 < /dev/null &); sleep 2; cat $D/srv.pid" </dev/null)
    # -W 0 -C 0: this harness measures the FIRST lap on purpose, so it keeps the client's pre-2026-10-06 behaviour
    # (no warm-up, no cool-down) - the default warm-up would put every arm past lap one (BenchWindow.h).
    clog=$(sh_ "cd $D/$sz && python3 $D/rusage_run.py env BENCH_IFACE=lo $pre ./client -Q -d $dur -i $iv -W 0 -C 0 > $D/cli.log 2>&1; cat $D/cli.log" </dev/null)
    stop_server
    slog=$(sh_ "cat $D/srv.log" </dev/null)
    say "ARM $tag client_sha=$csha"
    printf '%s\n' "$clog" | grep -E '^(RESULT|RUSAGE|PREFAULT)' | sed "s/^/  $tag client /" | tee -a "$OUT" >/dev/null
    printf '%s\n' "$slog" | grep -E '^(RUSAGE|PREFAULT)' | sed "s/^/  $tag server /" | tee -a "$OUT" >/dev/null
    printf '%s\n' "$clog" | grep -q '^RESULT' || { say "  $tag client printed no RESULT; its last lines:"; printf '%s\n' "$clog" | tail -4 | sed 's/^/     | /' | tee -a "$OUT"; }
}

# Arms in one fixed list; even reps run it backwards so no arm always runs first or last.
ARMS=()
for pf in none pre; do for sz in p3 p4; do
    ARMS+=("$sz 0.0005 short 1 $pf" "$sz 0.005 short 10 $pf" "$sz 0.005 long 60 $pf")
done; done
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep/$REPS $(date -Is) ---"
    if [ $((rep % 2)) -eq 0 ]; then order=$(seq $((${#ARMS[@]} - 1)) -1 0); else order=$(seq 0 $((${#ARMS[@]} - 1))); fi
    for i in $order; do
        # shellcheck disable=SC2086
        set -- ${ARMS[$i]}
        run_arm "$rep" "$1" "$2" "$3" "$4" "$5"
    done
done
say "=== runs done $(date -Is) ==="

python3 - "$OUT" "$sha_p3" "$sha_p4" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st, collections
path, sha = sys.argv[1], {'p3': sys.argv[2], 'p4': sys.argv[3]}
want_bytes = {'p3': '1424', 'p4': '2800'}
min_pop = {'p3': 5e6, 'p4': 11e6}
arms = collections.defaultdict(dict)  # (rep, sz, iv, len, pf) -> fields
key_re = r'rep=(\d+) size=(\S+) iv=(\S+) len=(\S+) pf=(\S+)'
for l in open(path, errors='replace'):
    m = re.match(r'ARM ' + key_re + r' client_sha=(\S+)', l)
    if m:
        arms[m.groups()[:5]]['sha'] = m.group(6); continue
    m = re.match(r'\s+' + key_re + r' (client|server) (RESULT|RUSAGE|PREFAULT):? (.*)', l)
    if not m:
        continue
    k, who, kind = m.groups()[:5], m.group(6), m.group(7)
    f = dict(kv.split('=', 1) for kv in m.group(8).split() if '=' in kv)
    arms[k][f'{who}_{kind}'] = f
rows, void = collections.defaultdict(list), []
for k, a in sorted(arms.items()):
    rep, sz, iv, ln, pf = k
    r = a.get('client_RESULT')
    if r is None or int(r.get('recv', 0)) == 0:
        void.append(f'{k}: no client RESULT with round trips'); continue
    lo, hi = {('0.0005', 'short'): (100, 2048), ('0.005', 'short'): (1500, 2048),
              ('0.005', 'long'): (6000, 10**9)}.get((iv, ln), (1, 0))  # (1, 0): not an arm of this design
    if not lo <= int(r['recv']) <= hi:
        void.append(f'{k}: not realized - {r["recv"]} round trips, the arm needs {lo}..{hi}'); continue
    if a.get('sha') != sha[sz] or r.get('sample_bytes') != want_bytes[sz]:
        void.append(f'{k}: identity - sha {a.get("sha")} sample_bytes {r.get("sample_bytes")}'); continue
    faults = {}
    ok = True
    for who in ('client', 'server'):
        ru = a.get(f'{who}_RUSAGE')
        if ru is None:
            void.append(f'{k}: no {who} RUSAGE'); ok = False; break
        base = 0
        if pf == 'pre':
            p = a.get(f'{who}_PREFAULT')
            if p is None or p.get('failed') != '0' or float(p.get('populated_bytes', 0)) < min_pop[sz]:
                void.append(f'{k}: {who} prefault not applied ({p})'); ok = False; break
            base = int(p['minflt_after'])
        faults[who] = (int(ru['minflt']) - base) / int(r['recv'])
    if not ok:
        continue
    rows[(sz, iv, ln, pf)].append(dict(p50=float(r['rtt_p50_ms']) * 1000, avg=float(r['rtt_avg_ms']) * 1000,
                                       n=int(r['recv']), mhz=float(r['cpu_mhz_mean']), cf=faults['client'],
                                       sf=faults['server']))
for v in void:
    print('VOID', v)
med = lambda k, f: st.median(x[f] for x in rows[k]) if len(rows.get(k, [])) >= 2 else None
print(f'{"arm":32} {"n":>3} {"rts":>6} {"p50us":>7} {"avgus":>7} {"MHz":>7} {"flt/rt cli":>10} {"srv":>6}')
for k in sorted(rows):
    print(f'{" ".join(k):32} {len(rows[k]):3} {med(k,"n") or 0:6.0f} {med(k,"p50") or 0:7.1f} {med(k,"avg") or 0:7.1f} '
          f'{med(k,"mhz") or 0:7.1f} {med(k,"cf") or 0:10.3f} {med(k,"sf") or 0:6.3f}')
gap, gapa = {}, {}
CONDS = [('0.0005', 'short'), ('0.005', 'short'), ('0.005', 'long')]
for iv, ln in CONDS:
        for pf in ('none', 'pre'):
            a, b = (('p3', iv, ln, pf), ('p4', iv, ln, pf))
            if med(a, 'p50') is not None and med(b, 'p50') is not None:
                gap[(iv, ln, pf)] = med(b, 'p50') - med(a, 'p50')
                gapa[(iv, ln, pf)] = med(b, 'avg') - med(a, 'avg')
                print(f'gap iv={iv:<6} {ln:5} {pf:4}: p50 {gap[(iv, ln, pf)]:+6.1f} us  avg {gapa[(iv, ln, pf)]:+6.1f} us  '
                      f'extra faults/rt p4-p3 client {med(b,"cf")-med(a,"cf"):+.3f} server {med(b,"sf")-med(a,"sf"):+.3f}')
need = [(iv, ln, pf) for iv, ln in CONDS for pf in ('none', 'pre')]
if any(n not in gap for n in need):
    print('NO VERDICT: a gap is missing (fewer than 2 usable reps somewhere)'); sys.exit(0)
moved = []
for iv, ln in CONDS:
    ref = med(('p3', '0.005', 'short', 'none'), 'p50') if iv == '0.005' else med(('p3', iv, ln, 'none'), 'p50')
    for pf in ('none', 'pre'):
        d = med(('p3', iv, ln, pf), 'p50') - ref
        if abs(d) > 3:
            moved.append(f'p3 iv={iv} {ln}/{pf} moved {d:+.1f} us')
ctl = 'CONTROL MOVED (' + '; '.join(moved) + ') - do not read the verdict' if moved else 'control held'
s3, s4 = ('p3', '0.005', 'short', 'none'), ('p4', '0.005', 'short', 'none')
c1 = min(med(s4, 'cf') - med(s3, 'cf'), med(s4, 'sf') - med(s3, 'sf')) >= 0.5
c2 = gap[('0.005', 'short', 'none')] - gap[('0.005', 'short', 'pre')] >= 8
c3 = gap[('0.005', 'short', 'none')] - gap[('0.005', 'long', 'none')] >= 8
c4 = gap[('0.0005', 'short', 'none')] >= 12
print(f'criteria: (1) faults {c1}  (2) prefault shrinks 5ms gap >= 8 us {c2}  (3) 5ms short-long >= 8 us {c3}  '
      f'(4) 0.5ms first-lap gap >= 12 us {c4}; {ctl}')
print(f'steady state, pages present and ring past lap one: gap(5ms long pre) = {gap[("0.005", "long", "pre")]:+.1f} us')
if c1 and c2 and c3 and c4:
    print('H-RING SUPPORTED: the p4 gap is first touch of the reorder ring, set by run length, not by idling')
elif abs(gap[('0.005', 'short', 'none')] - gap[('0.005', 'short', 'pre')]) <= 3 and \
        abs(gap[('0.005', 'short', 'none')] - gap[('0.005', 'long', 'none')]) <= 3 and gap[('0.0005', 'short', 'none')] <= 8:
    print('H-IDLE: prefault and run length do not move the gap; it follows idling')
else:
    print('PARTIAL: neither reading holds whole; see the numbers above')
PYEOF
echo "raw: $OUT"

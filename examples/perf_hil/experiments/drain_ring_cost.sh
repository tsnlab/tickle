#!/usr/bin/env bash
# drain_ring_cost.sh - what drain_rx()'s ring-turn look costs, and whether it still keeps a socket that never empties
# from starving the ring, on THIS PC in one private network namespace. A PC figure for steering a change before its
# rig A/B, never for publication.
#
# WHY (2026-10-08). 56565720 made drain_rx() look at the own ring once per pass (an acquire load of a line the writers
# keep writing), and its rig A/B read WORSE on best_effort_throughput p4 (receive rate -0.5%, client CPU per sample
# +0.5%). The follow-up looks only every tt_RX_CLOCK_REFRESH datagrams of one drain. This measures the arms side by
# side, and runs the starvation shape itself so the cheaper look is shown to still do its job.
#
# Usage: drain_ring_cost.sh NAME=REF [NAME=REF ...]      e.g. main=bf12bca2 every=a71e07eb long=cf5bebc2
#   The FIRST arm is the base the others are compared against.
# Env: REPS (5) DUR (5 s measured, +2 s warm-up and +2 s cool-down) PIN ("2 4": server and client CPUs, "" unpinned)
#      CELLS ("best_effort_throughput:p4 best_effort_throughput:p3 reliable_throughput:p3")
#      REPRO_REPS (5) REPRO_D (6 s of publishing) REPRO_LATE (1 s: how long after the publisher the subscriber starts)
#      MODE (all | cost | repro)  OUT (results file; default ~/drain_results/, outside any session directory)
#
# Each REF is exported with git archive and built with examples/perf_hil/tickle/build.sh under a private HOME, so no
# shared core prefix ($HOME/tickle_local_install*) and no checkout's build objects are touched.
#
# READING RULES, enforced in the analysis below:
#   COST. A run is VOID when its client or server RESULT is missing or its window is not ok. Per cell and arm: the
#     mean of server win_recv_mbps (BEST_EFFORT) or client win_send_mbps (RELIABLE), and of client and server
#     cpu_s_per_Msample, over the non-void reps; each arm against the base as delta % and Welch t. |t| > 2 at n >= 5 is
#     flagged MOVED; fewer than 3 non-void reps in either arm is NO VERDICT. Runs are interleaved (rep-major, arms
#     rotated) so a drift of the PC lands on every arm alike.
#   TREATMENT. Every traffic line of an arm built with the rule carries rx_drain_ring_turns=; the base's carries none.
#     An arm that does not is VOID (it measured the wrong build).
#   REPRO. The publisher (ring_starvation_repro.c: max rate from a thread that is not the polling one, before any
#     subscriber) starts REPRO_LATE s before the best_effort_throughput p3 server. Per rep: tx_shm_share. The repro
#     DECIDES only if the base arm, which has no rule, starves (share 0.000) in at least one rep - otherwise this PC
#     did not reproduce the shape and nothing is concluded about the others. Then an arm with the rule PASSES when
#     no rep starves, and FAILS on any rep with share 0.000.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
REPS=${REPS:-5}
DUR=${DUR:-5}
PIN=${PIN-2 4}
CELLS=${CELLS:-"best_effort_throughput:p4 best_effort_throughput:p3 reliable_throughput:p3"}
REPRO_REPS=${REPRO_REPS:-5}
REPRO_D=${REPRO_D:-6}
REPRO_LATE=${REPRO_LATE:-1}
REPRO_THREADS=${REPRO_THREADS:-1}
REPRO_SHAPE=${REPRO_SHAPE:-p3}
# REPRO_PIN=<cpu>: the whole publisher process, polling thread and publishing threads alike, on one CPU, so the
# polling thread reads slower than the publishers refill its socket - the rig Pi's shape. Empty: unpinned.
REPRO_PIN=${REPRO_PIN:-}
RPIN=()
[ -n "$REPRO_PIN" ] && RPIN=(taskset -c "$REPRO_PIN")
case " $CELLS " in *" best_effort_throughput:$REPRO_SHAPE "*) ;; *)
    echo "REPRO_SHAPE=$REPRO_SHAPE needs best_effort_throughput:$REPRO_SHAPE in CELLS (its server is the subscriber)" >&2
    exit 2 ;;
esac
MODE=${MODE:-all}
OUT=${OUT:-$HOME/drain_results/drain_ring_cost_$(date +%Y%m%d-%H%M%S).txt}
[ $# -ge 1 ] || { echo "usage: $0 NAME=REF [NAME=REF ...]" >&2; exit 2; }
mkdir -p "$(dirname "$OUT")"
: >"$OUT"
WORK=$(mktemp -d /tmp/drain_ring_cost.XXXXXX)
NS=tickle_drain_$$
say() { echo "$*" | tee -a "$OUT"; }
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    say "=== drain_ring_cost ended $(date -Is) (work: $WORK) ==="
}
trap cleanup EXIT
say "=== drain_ring_cost $(date -Is) mode=$MODE reps=$REPS dur=$DUR pin='$PIN' cells='$CELLS' repro_reps=$REPRO_REPS repro_d=$REPRO_D repro_late=$REPRO_LATE repro_threads=$REPRO_THREADS repro_shape=$REPRO_SHAPE repro_pin='$REPRO_PIN' ==="

# ---- build every arm ----------------------------------------------------------------------------------------------
ARMS=()
for spec in "$@"; do
    name=${spec%%=*}
    ref=${spec#*=}
    sha=$(git -C "$REPO" rev-parse --verify -q "$ref^{commit}") || { say "FATAL $name=$ref does not resolve"; exit 2; }
    src=$WORK/src_$name
    mkdir -p "$src" "$WORK/home_$name" "$WORK/arm_$name"
    git -C "$REPO" archive "$sha" | tar -x -C "$src" || { say "FATAL cannot export $sha"; exit 2; }
    for cell in $CELLS; do
        scen=${cell%%:*}
        size=${cell#*:}
        if ! HOME=$WORK/home_$name bash "$src/examples/perf_hil/tickle/build.sh" "$scen" "$size" \
            >>"$WORK/build_$name.log" 2>&1; then
            say "FATAL build $name $cell failed: $WORK/build_$name.log"
            exit 1
        fi
        mkdir -p "$WORK/arm_$name/$cell"
        cp "$src/examples/perf_hil/tickle/${scen}_$size/client" "$src/examples/perf_hil/tickle/${scen}_$size/server" \
            "$WORK/arm_$name/$cell/"
    done
    # The repro against this arm's prefix for REPRO_SHAPE (release, the build.sh default), with that shape's defines.
    prefix=$WORK/home_$name/tickle_local_install_o2
    shape_def=""
    if [ "$REPRO_SHAPE" = p4 ]; then
        prefix=$WORK/home_$name/tickle_local_install_sample4096_o2
        shape_def=-Dtt_MAX_SAMPLE_LENGTH=4096
    fi
    turns=""
    grep -q 'rx_drain_ring_turns;' "$src/include/tickle/tickle.h" && turns=-DREPRO_RING_TURNS
    # shellcheck disable=SC2046,SC2086
    if ! gcc -O2 $turns $shape_def -I"$src/examples/perf_hil/tickle/common/$REPRO_SHAPE" \
        $(PKG_CONFIG_PATH=$prefix/lib/pkgconfig pkg-config --cflags tickle) \
        -o "$WORK/arm_$name/repro" "$X/ring_starvation_repro.c" "$src/examples/perf_hil/tickle/common/$REPRO_SHAPE/Bench.c" \
        $(PKG_CONFIG_PATH=$prefix/lib/pkgconfig pkg-config --libs tickle) -lm -lpthread >>"$WORK/build_$name.log" 2>&1; then
        say "FATAL repro build $name failed: $WORK/build_$name.log"
        exit 1
    fi
    ARMS+=("$name")
    say "arm $name = $sha rule=$([ -n "$turns" ] && echo yes || echo no) repro $(sha256sum "$WORK/arm_$name/repro" | cut -c1-16)"
done

sudo -n ip netns add "$NS" || { say "FATAL cannot create netns"; exit 1; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
# The benches' compiled-in broadcast address is the rig link's; a dummy link carries it inside the namespace.
sudo -n ip netns exec "$NS" ip link add bench0 type dummy
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip netns exec "$NS" ip link set bench0 up
SPIN=()
CPIN=()
if [ -n "$PIN" ]; then
    read -r s c <<<"$PIN"
    SPIN=(taskset -c "$s")
    CPIN=(taskset -c "$c")
fi

# Ends the server whose PID its own shell wrote before exec'ing it, after checking /proc/PID/exe - never by pattern.
stop_server() { # pidfile binary
    local spid
    spid=$(cat "$1" 2>/dev/null)
    if [ -n "$spid" ] && [ "$(sudo -n ip netns exec "$NS" readlink "/proc/$spid/exe")" = "$(readlink -f "$2")" ]; then
        sudo -n ip netns exec "$NS" kill -INT "$spid"
    else
        say "WARN server pid '$spid' is not $2 - left to its own cap"
    fi
}

# ---- cost: interleaved reps -----------------------------------------------------------------------------------------
if [ "$MODE" = all ] || [ "$MODE" = cost ]; then
    WIN=(--warmup-s 2 --cooldown-s 2)
    n=${#ARMS[@]}
    for rep in $(seq 1 "$REPS"); do
        for cell in $CELLS; do
            for k in $(seq 0 $((n - 1))); do
                name=${ARMS[$(((k + rep - 1) % n))]} # rotated, so no arm always runs first
                d=$WORK/run_${name}_${cell}_$rep
                mkdir -p "$d"
                a=$WORK/arm_$name/$cell
                # shellcheck disable=SC2024,SC2016
                sudo -n ip netns exec "$NS" "${SPIN[@]}" sh -c 'echo $$ > "$0"; exec "$@"' "$d/srv.pid" \
                    "$a/server" -Q -d $((DUR + 30)) "${WIN[@]}" >"$d/srv.log" 2>&1 &
                srv=$!
                sleep 1
                # shellcheck disable=SC2024
                sudo -n ip netns exec "$NS" "${CPIN[@]}" "$a/client" -Q -d "$DUR" "${WIN[@]}" >"$d/cli.log" 2>&1
                sleep 0.5
                stop_server "$d/srv.pid" "$a/server"
                wait "$srv"
                {
                    echo "RUN arm=$name cell=$cell rep=$rep"
                    grep -h '^RESULT' "$d/cli.log" | sed 's/^/CLI /'
                    grep -h '^RESULT' "$d/srv.log" | sed 's/^/SRV /'
                    grep -h 'traffic: ' "$d/cli.log" | sed 's/^/CLITRAFFIC /'
                    grep -h 'traffic: ' "$d/srv.log" | sed 's/^/SRVTRAFFIC /'
                } >>"$OUT"
            done
        done
        say "cost rep $rep done $(date +%T)"
    done
fi

# ---- repro: the publisher before its subscriber ----------------------------------------------------------------------
if [ "$MODE" = all ] || [ "$MODE" = repro ]; then
    for rep in $(seq 1 "$REPRO_REPS"); do
        for name in "${ARMS[@]}"; do
            d=$WORK/repro_${name}_$rep
            mkdir -p "$d"
            a=$WORK/arm_$name
            # shellcheck disable=SC2024
            sudo -n ip netns exec "$NS" "${RPIN[@]}" "$a/repro" -d "$REPRO_D" -t "$REPRO_THREADS" >"$d/pub.log" 2>&1 &
            pub=$!
            sleep "$REPRO_LATE"
            # shellcheck disable=SC2024,SC2016
            sudo -n ip netns exec "$NS" sh -c 'echo $$ > "$0"; exec "$@"' "$d/srv.pid" \
                "$a/best_effort_throughput:$REPRO_SHAPE/server" -Q -d $((REPRO_D + 30)) >"$d/srv.log" 2>&1 &
            srv=$!
            wait "$pub"
            sleep 0.5
            stop_server "$d/srv.pid" "$a/best_effort_throughput:$REPRO_SHAPE/server"
            wait "$srv"
            {
                echo "REPRORUN arm=$name rep=$rep"
                grep -h '^REPRO:' "$d/pub.log" || echo "REPRO: missing"
            } >>"$OUT"
        done
    done
fi

(python3 - "$OUT" "${ARMS[@]}" <<'PYEOF'
import sys, math, statistics as st
path, arms = sys.argv[1], sys.argv[2:]
base = arms[0]
def kv(line):
    return dict(t.split('=', 1) for t in line.split() if '=' in t)
runs, repros, cur = [], [], None
for l in open(path):
    l = l.rstrip('\n')
    if l.startswith('RUN '):
        cur = kv(l); cur.update(cli=None, srv=None, ctr=None, str=None); runs.append(cur)
    elif l.startswith('REPRORUN '):
        cur = None; repros.append(kv(l))
    elif l.startswith('REPRO:') and repros:
        repros[-1].update(kv(l[6:])); repros[-1]['seen'] = 'sent' in repros[-1]
    elif cur is not None:
        for tag, key in (('CLI ', 'cli'), ('SRV ', 'srv'), ('CLITRAFFIC ', 'ctr'), ('SRVTRAFFIC ', 'str')):
            if l.startswith(tag):
                cur[key] = kv(l[len(tag):].split('traffic:', 1)[-1] if 'TRAFFIC' in tag else l[len(tag):])
print('--- analysis ---')
cells = []
for r in runs:
    if r['cell'] not in cells: cells.append(r['cell'])
treat_void = set()
for r in runs:
    has_rule = None
    for t in ('ctr', 'str'):
        if r[t] is not None:
            present = 'rx_drain_ring_turns' in r[t]
            has_rule = present if has_rule is None else (has_rule and present)
    r['turns'] = sum(int(r[t].get('rx_drain_ring_turns', 0)) for t in ('ctr', 'str') if r[t] is not None)
    want = r['arm'] != base
    if has_rule is None or has_rule != want:
        treat_void.add(r['arm'])
        print(f"TREATMENT VOID arm={r['arm']} cell={r['cell']} rep={r['rep']}: rx_drain_ring_turns present={has_rule}, want {want}")
def metrics(r):
    c, s = r['cli'], r['srv']
    if not c or not s: return None, 'RESULT missing'
    if c.get('window') != 'ok' or s.get('window') != 'ok': return None, 'window not ok'
    m = {'cli_cpu': float(c['cpu_s_per_Msample']), 'srv_cpu': float(s['cpu_s_per_Msample'])}
    if r['cell'].startswith('best_effort'):
        m['rate'] = float(s['win_recv_mbps']); m['rate_name'] = 'server.win_recv_mbps'
    else:
        m['rate'] = float(c['win_send_mbps']); m['rate_name'] = 'client.win_send_mbps'
    return m, None
for cell in cells:
    data = {}
    for arm in arms:
        rows = []
        for r in runs:
            if r['arm'] != arm or r['cell'] != cell: continue
            m, why = metrics(r)
            if m is None:
                print(f"VOID arm={arm} cell={cell} rep={r['rep']}: {why}"); continue
            m['turns'] = r['turns']; rows.append(m)
        data[arm] = rows
    print(f"cell {cell}")
    for arm in arms:
        rows = data[arm]
        if not rows: print(f"  {arm}: no runs"); continue
        rn = rows[0]['rate_name']
        print(f"  {arm:6s} n={len(rows)} {rn}={st.mean(x['rate'] for x in rows):.1f} "
              f"client.cpu_s_per_Msample={st.mean(x['cli_cpu'] for x in rows):.4f} "
              f"server.cpu_s_per_Msample={st.mean(x['srv_cpu'] for x in rows):.4f} "
              f"rx_drain_ring_turns(sum both, per run)={[x['turns'] for x in rows]}")
    b = data[base]
    for arm in arms[1:]:
        o = data[arm]
        for key, label in (('rate', 'rate'), ('cli_cpu', 'client cpu/sample'), ('srv_cpu', 'server cpu/sample')):
            if len(o) < 3 or len(b) < 3:
                print(f"  {arm}-{base} {label}: NO VERDICT (n {len(o)}/{len(b)})"); continue
            mb, mo = st.mean(x[key] for x in b), st.mean(x[key] for x in o)
            se = math.sqrt(st.variance([x[key] for x in b]) / len(b) + st.variance([x[key] for x in o]) / len(o))
            t = (mo - mb) / se if se > 0 else float('inf')
            flag = 'MOVED' if abs(t) > 2 and min(len(o), len(b)) >= 5 else 'held'
            print(f"  {arm}-{base} {label}: {100 * (mo - mb) / mb:+.2f}% t={t:+.2f} {flag}")
if repros:
    print('repro (tx_shm_share per rep):')
    shares = {}
    for arm in arms:
        rs = [x for x in repros if x['arm'] == arm]
        shares[arm] = [float(x['tx_shm_share']) if x.get('seen') else None for x in rs]
        print(f"  {arm:6s} " + ' '.join('missing' if v is None else f'{v:.3f}' for v in shares[arm]) +
              '  turns=' + ','.join(x.get('rx_drain_ring_turns', '?') for x in rs) +
              '  self_rx=' + ','.join(x.get('rx_self_sent', '?') for x in rs))
    if not any(v == 0.0 for v in shares[base] if v is not None):
        print(f"REPRO NO VERDICT: the base arm {base} never starved on this PC - the shape was not reproduced")
    else:
        for arm in arms[1:]:
            vals = shares[arm]
            bad = [v for v in vals if v is None or v == 0.0]
            print(f"REPRO {arm}: {'FAIL' if bad else 'PASS'} ({len(vals) - len(bad)}/{len(vals)} reps on the segment)")
PYEOF
) >"$WORK/analysis.txt" 2>&1
cat "$WORK/analysis.txt" >>"$OUT"
cat "$WORK/analysis.txt"
say "results: $OUT"

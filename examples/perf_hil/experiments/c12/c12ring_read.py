#!/usr/bin/env python3
"""C12 ring-follows-data rig A/B reader (ab/c12-ring-follows-data vs main), rules written 2026-10-29 15:3x BEFORE the run.

Input: the two kh2_rig_ab.sh arm outputs, P (main) and B (fix): kh2ab_<arm>_<sha8>_<ts>.txt (+ .pcaps .sysstamp .logs).
Measured (rmw_tickle@libc *_sst stems, reps 1-3; vendors rmw_cyclonedds_cpp / rmw_fastrtps_cpp the same):
  ping tap -> receive return over all echoes (ping_path.py "via all"), echoes received by io_uring_enter,
  pong tap -> ring return (prev_call.py "tap->recv all (asleep)"), and the production logs' ring_moves.
Rules, in order:
  1 VOID       any ping stem with < 90 echoes matched, or a missing stem.
  2 VOID       treatment absent: a B tickle rep with < 95 echoes received by io_uring_enter, or a B tickle ping log
               (all variants) with ring_moves outside 1..4. (P with > 5 ring receives in a rep is printed as a warning.)
  3 NOT COMPARABLE  a vendor's median-of-rep-p50 ping return moves by > 0.5 us between P and B. (0.3 in the design;
               widened 15:4x before the run: the 10-24 KH2B pair, both arms without the fix, moved CycloneDDS -0.41 us.)
  4 REGRESSION pong tickle p50 worse in B than P by > 0.5 us in every rep, or a B tickle pong log with ring_moves > 2.
  5 PASS       every B tickle ping rep p50 below every P rep, and median(B) - median(P) <= -1.0 us.
    NARROWED   every B rep below every P rep, shift between -1.0 and -0.3 us.
    NO EFFECT  otherwise (the falsifier: the treatment acted and the return did not move).
"""
import re
import subprocess
import sys
from pathlib import Path
from statistics import median

HERE = Path(__file__).resolve().parent
REPS = (1, 2, 3)
TICKLE = "rmw_tickle@libc_bench_best_effort_rep{}_block_sst"
VENDORS = ("rmw_cyclonedds_cpp", "rmw_fastrtps_cpp")


def run(script, out, stems):
    r = subprocess.run([sys.executable, str(HERE / script), str(out), *stems], capture_output=True, text=True,
                       timeout=900)
    return r.stdout


def blocks(text):
    cur, res = None, {}
    for ln in text.splitlines():
        m = re.match(r"== (\S+)", ln)
        if m:
            cur = m.group(1)
            res[cur] = []
        elif cur:
            res[cur].append(ln)
    return res


def p50(lines, label):
    for ln in lines:
        if label in ln:
            m = re.search(r"n=\s*(\d+).*?p50\s+([\d.]+)", ln)
            if m:
                return int(m.group(1)), float(m.group(2))
    return 0, None


def ring_recv(lines):
    for ln in lines:
        m = re.search(r"'io_uring_enter': (\d+)", ln)
        if "by {" in ln:
            return int(m.group(1)) if m else 0
    return 0


def log_moves(out, side):
    d = Path(str(out) + ".logs")
    res = {}
    for f in sorted(d.glob(f"rmw_tickle@*_block*_{side}.log")):
        m = re.findall(r"ring_moves=(\d+)", f.read_text(errors="replace"))
        res[f.name] = int(m[-1]) if m else None
    return res


def arm(out):
    stems = [TICKLE.format(r) for r in REPS] + [f"{v}_bench_best_effort_rep{r}_block_sst" for v in VENDORS for r in REPS]
    ping = blocks(run("ping_path.py", out, stems))
    pong = blocks(run("prev_call.py", out, [TICKLE.format(r) for r in REPS]))
    a = {"ping": {}, "ring": {}, "pong": {}, "vendor": {v: [] for v in VENDORS}, "n_short": [],
         "moves_ping": log_moves(out, "ping"), "moves_pong": log_moves(out, "pong")}
    for s in stems:
        n, v = p50(ping.get(s, []), "via all")
        if s not in ping or n < 90 or v is None:
            a["n_short"].append(f"{s} n={n}")
            continue
        if s.startswith("rmw_tickle"):
            a["ping"][s] = v
            a["ring"][s] = ring_recv(ping[s])
        else:
            a["vendor"][next(x for x in VENDORS if s.startswith(x))].append(v)
    for s in [TICKLE.format(r) for r in REPS]:
        _, v = p50(pong.get(s, []), "tap->recv all (asleep)")
        a["pong"][s] = v
    return a


def main():
    if len(sys.argv) != 3:
        print("usage: c12ring_read.py <P kh2ab_P_*.txt> <B kh2ab_B_*.txt>")
        return 64
    P, B = arm(Path(sys.argv[1])), arm(Path(sys.argv[2]))
    for name, a in (("P", P), ("B", B)):
        print(f"{name}: ping tickle p50 {a['ping']}  ring receives {a['ring']}")
        print(f"{name}: pong tickle p50 {a['pong']}")
        print(f"{name}: vendors ping p50 {a['vendor']}")
        print(f"{name}: ring_moves ping {a['moves_ping']}  pong {a['moves_pong']}")
    if P["n_short"] or B["n_short"]:
        print(f"VERDICT: VOID (short or missing stems: P {P['n_short']} B {B['n_short']})")
        return 2
    if any(n > 5 for n in P["ring"].values()):
        print("warning: P received > 5 echoes by the ring in a rep (main was expected not to)")
    bad = [s for s, n in B["ring"].items() if n < 95]
    badm = [f for f, m in B["moves_ping"].items() if m is None or not 1 <= m <= 4]
    if bad or badm or not B["moves_ping"]:
        print(f"VERDICT: VOID (treatment absent: ring receives < 95 in {bad}; ping ring_moves outside 1..4 in {badm})")
        return 2
    for v in VENDORS:
        d = median(B["vendor"][v]) - median(P["vendor"][v])
        print(f"control {v}: median ping p50 B - P = {d:+.2f} us")
        if abs(d) > 0.5:
            print(f"VERDICT: NOT COMPARABLE ({v} moved {d:+.2f} us > 0.5)")
            return 2
    pong_d = [B["pong"][s] - P["pong"][s] for s in P["pong"] if P["pong"][s] is not None and B["pong"].get(s) is not None]
    badpong = [f for f, m in B["moves_pong"].items() if m is not None and m > 2]
    print(f"pong B - P per rep: {[round(x, 2) for x in pong_d]}")
    if (pong_d and len(pong_d) == len(REPS) and all(x > 0.5 for x in pong_d)) or badpong:
        print(f"VERDICT: REGRESSION (pong worse > 0.5 us every rep, or pong ring_moves > 2 in {badpong})")
        return 1
    pb, pp = list(B["ping"].values()), list(P["ping"].values())
    shift = median(pb) - median(pp)
    below = max(pb) < min(pp)
    print(f"ping tickle median p50: P {median(pp):.2f}  B {median(pb):.2f}  shift {shift:+.2f} us; every B below every P: {below}")
    if below and shift <= -1.0:
        print("VERDICT: PASS")
    elif below and shift <= -0.3:
        print("VERDICT: NARROWED")
    else:
        print("VERDICT: NO EFFECT")
    return 0


if __name__ == "__main__":
    sys.exit(main())

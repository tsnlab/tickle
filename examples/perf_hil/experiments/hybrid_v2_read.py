#!/usr/bin/env python3
"""hybrid_v2_read.py - the rig A/B of ab/hybrid-v2 against its parent, read by rules written 2026-10-12 before any run
(~/rig_queue_hybrid_v2.sh). B is A plus two changes to how a large sample is sent:

  (1) a send call of runs carries LARGE_BATCH (64) units, not 64 runs, so the ranged encode stays a call ahead of the
      wire instead of encoding the whole sample before the first run (PC: -59 us at 1 MB, -288..-312 us at 4 MB);
  (2) a send blocked by a full socket buffer resumes on the socket's writability (EPOLLOUT at half the buffer), the
      50 us -> 1 ms timer only a fallback (rig: 20-30 timer retries per MB, send_waits).
  and a fix that changes no 0% cell: the end-of-sample HEARTBEAT carries the match point while one is owed.

Input: rmw_keepall_rig.sh invocations at 0% loss (OUT.runs/), arms tickle@head (B), tickle@pre (A), fastdds,
cyclonedds, read through hybrid_rig_ab.py's read_invocation() with HYBRID_PRE=1 (its VOID rules, treatment included:
head and pre at 0% must reach n = 45 - A has the hybrid transport too) and largemsg_rig_summary.py's run records (clock-corrected latency median, CPU ms per MB each side).

RULES (enforced below; a cell with fewer than 3 usable runs of head or pre is NO VERDICT, and so is the criterion)
  FLOOR      per cell and metric, the larger of the two vendors' spread (max - min over their runs): drift the change
             cannot touch. d = mean(head) - mean(pre), SE Welch.
             BETTER iff d < -2 SE and -d > floor; WORSE iff d > 2 SE and d > floor; else HELD.
  TREATMENT  every head publisher log's "large:" line carries send_writable=; in a cell where head waited for room
             (send_waits > 0) send_writable must be > 0 in every head run, or the cell's (2) is VOID ("could not look"
             is not "no wait"). pre's line must carry no send_writable= (A is the parent), or the reading is VOID.
  (1) LATENCY  in each of the four s1 cells (Array1m 30 Hz and Array4m 15 Hz, RELIABLE and BEST_EFFORT), the latency
             median: -d >= 0.1 ms at 1 MB and >= 0.3 ms at 4 MB, with d < -2 SE (the PC's gain was one whole-sample
             copy; the Pi's copy is ~4x the PC's). AND no CPU WORSE: pub and sub ms/MB not WORSE in any s1 cell.
             PASS iff every s1 cell meets it; FAIL if any misses; FALSIFIED (the encode was not on the latency path on
             the Pi) if both 4 MB cells are HELD on latency.
  (2) SEND     send_waits per MB down - head's mean below pre's by more than 2 SE - in every cell where pre waited
             (pre send_waits > 0), AND pub ms/MB BETTER in at least 2 of those cells and WORSE in none. FAIL
             otherwise. NOT EXERCISED when pre never waited in any cell.
  PRINTED, never deciding: p99 lower bound and sub peak RSS head vs pre; head vs fastdds and cyclonedds by the range
             rule (hybrid_rig_ab.py --zero prints the same per invocation); I4s is read by largemsg_rig_summary.py
             (delivery per offered tick) and i4s_drain_summary.py, not here.

Exit 0 when every criterion has a verdict (PASS or FAIL alike), 2 when any is NO VERDICT or VOID, 1 on missing input.
Usage: hybrid_v2_read.py OUT [OUT ...]   (each OUT.runs/ an invocation; the cell is read from its runs)
"""
import math
import os
import re
import statistics
import sys
from pathlib import Path

os.environ["HYBRID_PRE"] = "1"  # A (c7262c89) has the hybrid transport: pre is read by head's AUTO rule
sys.path.insert(0, str(Path(__file__).resolve().parent))
from hybrid_rig_ab import read_invocation, stem_of, welch  # noqa: E402

MB = {"Array1m": 1048576 / 1e6, "Array4m": 4194304 / 1e6}
LAT_MIN_GAIN_MS = {"Array1m": 0.1, "Array4m": 0.3}
VENDORS = ("fastdds", "cyclonedds")
LARGE_RE = re.compile(r"large: published=(\d+) .*?send_waits=(\d+)(.*)$", re.M)
WRITABLE_RE = re.compile(r"send_writable=(\d+)")


def fin(xs):
    return [x for x in xs if isinstance(x, (int, float)) and not math.isnan(x)]


def send_of(path, topic):
    """(send_waits per MB, send_writable or None when the line has none) from a publisher log, or None unread."""
    if not path.exists():
        return None
    lines = LARGE_RE.findall(path.read_text(errors="replace"))
    lines = [x for x in lines if int(x[0]) > 0]
    if not lines:
        return None
    published, waits, rest = max(lines, key=lambda x: int(x[0]))
    m = WRITABLE_RE.search(rest)
    return int(waits) / (int(published) * MB[topic]), (int(m.group(1)) if m else None), int(waits)


def floor_of(arms, metric):
    spreads = []
    for v in VENDORS:
        xs = fin(r[metric] for r in arms.get(v, []))
        if len(xs) >= 2:
            spreads.append(max(xs) - min(xs))
    return max(spreads) if spreads else None


def verdict(head, pre, floor):
    if len(head) < 3 or len(pre) < 3 or floor is None:
        return None, float("nan"), float("nan")
    d, se = welch(head, pre)
    if d < -2 * se and -d > floor:
        return "BETTER", d, se
    if d > 2 * se and d > floor:
        return "WORSE", d, se
    return "HELD", d, se


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cells = {}
    status = 0
    for out in sys.argv[1:]:
        got = read_invocation(out, quiet=True)
        if isinstance(got, int):
            print(f"MISSING/VOID {out}: read_invocation returned {got}")
            status = max(status, 2 if got == 2 else 1)
            continue
        meta, usable, _lose = got
        reliable = "--reliable" in meta.get("qos_args", "")
        for (topic, loss), arms in usable.items():
            if loss != 0 or topic not in MB:
                continue
            rate = meta.get("pub_rate")
            cells[(topic, reliable, rate)] = (Path(f"{out}.runs"), arms)
    if not cells:
        print("NO VERDICT: no usable 0% cell")
        return 2
    lat_ok, lat_any_missing, cpu_worse, held_4m = True, False, False, 0
    send_cells, send_down, send_better, send_worse, send_void = 0, True, 0, False, False
    for (topic, reliable, rate), (runs, arms) in sorted(cells.items()):
        name = f"{topic} {rate} Hz {'RELIABLE' if reliable else 'BEST_EFFORT'}"
        head, pre = arms.get("tickle@head", []), arms.get("tickle@pre", [])
        s1 = (topic == "Array1m" and str(rate) == "30") or (topic == "Array4m" and str(rate) == "15")
        print(f"=== {name}: head {len(head)} runs, pre {len(pre)} runs ({runs}) ===")
        for metric in ("lat_med", "p99_lb", "cpu_pub", "cpu_sub", "rss_sub"):
            v, d, se = verdict(fin(r.get(metric) for r in head), fin(r.get(metric) for r in pre),
                               floor_of(arms, metric))
            hm = fin(r.get(metric) for r in head)
            pm = fin(r.get(metric) for r in pre)
            print(f"  {metric:8s} head {statistics.mean(hm) if hm else float('nan'):10.3f} "
                  f"pre {statistics.mean(pm) if pm else float('nan'):10.3f} d {d:+9.3f} 2SE {2 * se:7.3f} "
                  f"floor {floor_of(arms, metric) or float('nan'):7.3f} -> {v or 'NO VERDICT'}")
            if s1 and metric in ("cpu_pub", "cpu_sub") and v == "WORSE":
                cpu_worse = True
            if s1 and metric == "lat_med":
                if v is None:
                    lat_any_missing = True
                else:
                    gain = -d
                    meets = gain >= LAT_MIN_GAIN_MS[topic] and d < -2 * se
                    print(f"  (1) latency gain {gain:.3f} ms, needed >= {LAT_MIN_GAIN_MS[topic]} ms beyond 2 SE: "
                          f"{'MET' if meets else 'MISSED'}")
                    lat_ok = lat_ok and meets
                    if topic == "Array4m" and v == "HELD":
                        held_4m += 1
        # (2) send waits and publisher CPU
        hs = [send_of(runs / f"{stem_of(r)}_pub.log", topic) for r in head]
        ps = [send_of(runs / f"{stem_of(r)}_pub.log", topic) for r in pre]
        if any(x is None for x in hs + ps):
            print("  (2) VOID: a publisher log without its 'large:' line")
            send_void = True
            continue
        if any(x[1] is not None for x in ps):
            print("  (2) VOID overall: pre's line carries send_writable= - A is not the parent")
            send_void = True
            continue
        if any(x[1] is None for x in hs):
            print("  (2) VOID: a head publisher line without send_writable=")
            send_void = True
            continue
        hw, pw = [x[0] for x in hs], [x[0] for x in ps]
        print(f"  send_waits/MB head {', '.join(f'{x:.1f}' for x in hw)} (send_writable "
              f"{', '.join(str(x[1]) for x in hs)}) pre {', '.join(f'{x:.1f}' for x in pw)}")
        if any(x[2] > 0 and x[1] == 0 for x in hs):
            print("  (2) VOID: head waited for room and no wait ended on writability (the treatment did not apply)")
            send_void = True
            continue
        if not pw or max(pw) == 0:
            print("  (2) not exercised here: pre never waited for room")
            continue
        send_cells += 1
        if len(hw) < 3 or len(pw) < 3:
            print("  (2) NO VERDICT: fewer than 3 runs")
            send_void = True
            continue
        d, se = welch(hw, pw)
        down = d < -2 * se
        send_down = send_down and down
        v, dc, sec = verdict(fin(r.get("cpu_pub") for r in head), fin(r.get("cpu_pub") for r in pre),
                             floor_of(arms, "cpu_pub"))
        send_better += 1 if v == "BETTER" else 0
        send_worse = send_worse or v == "WORSE"
        print(f"  (2) send_waits/MB d {d:+.2f} (2SE {2 * se:.2f}) {'DOWN' if down else 'NOT DOWN'}; pub ms/MB {v}")
    print("=== criteria ===")
    if lat_any_missing:
        print("(1) LATENCY: NO VERDICT - an s1 cell without 3 usable runs of head and pre")
        status = max(status, 2)
    elif held_4m == 2:
        print("(1) LATENCY: FALSIFIED - both 4 MB cells HELD: the encode was not on the Pi's latency path")
    else:
        ok = lat_ok and not cpu_worse
        print(f"(1) LATENCY: {'PASS' if ok else 'FAIL'} - every s1 cell's gain {'met' if lat_ok else 'NOT met'}, "
              f"CPU WORSE in an s1 cell: {'yes' if cpu_worse else 'no'}")
    if send_void:
        print("(2) SEND: VOID / NO VERDICT - see the cells above")
        status = max(status, 2)
    elif send_cells == 0:
        print("(2) SEND: NOT EXERCISED - pre never waited for room")
    else:
        ok = send_down and send_better >= 2 and not send_worse
        print(f"(2) SEND: {'PASS' if ok else 'FAIL'} - send_waits down in every waiting cell: "
              f"{'yes' if send_down else 'no'}; pub ms/MB BETTER in {send_better} of {send_cells}, WORSE in "
              f"{'one or more' if send_worse else 'none'}")
    return status


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""C12 pong residual: is the pong tap -> receive return shift between P and B noise, the well-known wake, or b9439f3d?

Reads existing kh2_rig_ab.sh arm outputs only (pcaps + pong sysstamp); runs nothing on the rig.

Per ping at the pong tap (rmw_tickle@libc *_sst stems; vendors by the recv* carrying the key, as prev_call.py):
  v        tap -> return of the call that received it (the ring's io_uring_enter for TickLE), us
  prev     the reply thread's last wake before the tap: the last call on that thread that returned before the
           receiving wait was entered; kind = "ping" if it was within 5 ms after the previous ping's receive, else
           "wk" (a datagram read on the well-known socket - the peer's announcement, 0x6b0e03..) or "tick" (the
           node's own periodic send and its loopback), and gap_ms = tap - that return.
Printed per rep: p50 of all samples, p50 of samples whose previous wake was the previous ping (gap 95..105 ms:
nothing else woke the thread in between), and per kind the count, gap range and p50.

Post-hoc diagnostic (2026-10-30), not a pre-registered reader: it decides nothing by itself. Its use on the
10-24 and 10-29 KH2 arms is in ~/claude_reports/C12_PONG_RESIDUAL_2026-10-30.md.

Usage: pong_residual.py <kh2ab_*.txt> [<kh2ab_*.txt> ...]
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import rmw_pcap_split as S  # noqa: E402

REPS = (1, 2, 3)
STEMS = ("rmw_tickle@libc_bench_best_effort_rep{}_block_sst", "rmw_cyclonedds_cpp_bench_best_effort_rep{}_block_sst",
         "rmw_fastrtps_cpp_bench_best_effort_rep{}_block_sst")


def p50(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else float("nan")


def offsets(text):
    offs = {}
    for m in re.finditer(r"clock_offset_ns stem=(\S+) before: client=(-?\d+) server=(-?\d+) "
                         r"after: client=(-?\d+) server=(-?\d+)", text):
        c0, _, c1, _ = (int(x) for x in m.groups()[1:])
        offs[m.group(1)] = (c0 + c1) // 2
    return offs


def samples(out, stem, off):
    """[(tap, v_us, receive record)] for every ping that reached the pong tap and was received asleep."""
    pc = out.with_suffix(out.suffix + ".pcaps")
    sst = out.with_suffix(out.suffix + ".sysstamp") / f"{stem}_pong"
    if not sst.is_dir():
        return None, []
    sent, cli = S.ping_keys(pc / f"{stem}_ping.pcap", off)
    arr = S.first_by_key(pc / f"{stem}_pong.pcap", sent, cli)
    recs = S.sysstamp_records(sst)
    res = []
    for k, tap in sorted(arr.items(), key=lambda kv: kv[1]):
        if stem.startswith("rmw_tickle"):
            pss = S.first_call(recs, k.hex(), "send", tap)
            pr = S.ring_receive(recs, tap, pss[2], pss[0]) if pss else None
        else:
            pr = S.first_call(recs, k.hex(), "recv", tap)
        if not pr or pr[2] >= tap:
            continue
        res.append((tap, (pr[3] - tap) / 1000, pr))
    return recs, res


def classify(recs, res):
    """Adds (kind, gap_ms) per sample: what last woke the reply thread before the wait that received this ping."""
    out = []
    for i, (tap, v, pr) in enumerate(res):
        prior = [r for r in recs if r[0] == pr[0] and r[3] <= pr[2]]
        last = prior[-1] if prior else None
        kind, gap = "none", float("nan")
        if last:
            gap = (tap - last[3]) / 1e6
            pp = res[i - 1][2] if i else None
            if pp and pp[0] == pr[0] and 0 <= last[3] - pp[3] <= 5_000_000:
                kind = "ping"
            else:
                # the wake that started the burst `last` belongs to: read what the thread did after it
                burst = [r for r in prior if r[3] >= last[3] - 5_000_000]
                heads = [r[5][:6] for r in burst if r[1].startswith("recv") and r[4] > 0]
                kind = "wk" if any(h == "6b0e03" for h in heads) else "tick"
        out.append((tap, v, kind, gap))
    return out


def main():
    for arg in sys.argv[1:]:
        out = Path(arg)
        offs = offsets(out.read_text())
        print(f"--- {out.name}")
        for tmpl in STEMS:
            for rep in REPS:
                stem = tmpl.format(rep)
                if stem not in offs:
                    print(f"  {stem}: NO OFFSET")
                    continue
                recs, res = samples(out, stem, offs[stem])
                if not res:
                    print(f"  {stem}: NO SAMPLE")
                    continue
                rows = classify(recs, res)
                allv = [r[1] for r in rows]
                clean = [r[1] for r in rows if r[2] == "ping" and 95 <= r[3] <= 105]
                line = f"  {stem:52s} n={len(allv):3d} p50 {p50(allv):6.2f} | prev-ping only n={len(clean):3d} " \
                       f"p50 {p50(clean):6.2f}"
                for kind in ("wk", "tick", "none"):
                    ks = [r for r in rows if r[2] == kind]
                    if ks:
                        g = [r[3] for r in ks]
                        kv = p50([r[1] for r in ks])
                        line += f" | {kind} n={len(ks)} gap {min(g):.0f}-{max(g):.0f} ms p50 {kv:.2f}"
                print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())

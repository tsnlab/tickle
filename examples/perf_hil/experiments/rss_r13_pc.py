#!/usr/bin/env python3
"""rss_r13_pc.py <out dir> - read rss_r13_pc.sh's runs: per build and arm, n, median sub ru_maxrss (R13), median idle sub
VmHWM (before the publisher's launch), median of the sampler's largest sub VmHWM, and the same for pub. VOID: no
received sample or no ru_maxrss row."""
import collections
import pathlib
import re
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import rmw_samehost_summary as S  # noqa: E402


def one(d):
    m = S.read_meta(d)
    sub, pub = S.perf_rows(d / "sub.log")[0], S.perf_rows(d / "pub.log")[0]
    if not sub or not pub or sum(r["received"] for r in sub) == 0:
        return None
    rows = S.sampler(d)
    t_pub = m["launch"].get("pub")
    idle = [r["sub"][1] for r in rows if "sub" in r and t_pub and r["t"] < t_pub]
    peak = {k: max((r[k][1] for r in rows if k in r), default=0) for k in ("sub", "pub")}
    # The topic's subscriber is the one that delivered most; its reorder_held_peak says how many reorder slots the run
    # touched (each slot is resident once written), which on the PC is either ~0 or ~490 and moves R13 by ~1 MB.
    held = 0
    best = -1
    for line in (d / "sub.log").read_text(errors="replace").splitlines():
        mm = re.search(r"delivery: delivered=(\d+).*reorder_held_peak=(\d+)", line)
        if mm and int(mm.group(1)) > best:
            best, held = int(mm.group(1)), int(mm.group(2))
    return dict(sub=sub[-1]["ru_maxrss"], pub=pub[-1]["ru_maxrss"], idle=idle[-1] if idle else 0,
                sub_peak=peak["sub"], pub_peak=peak["pub"], n=sum(r["received"] for r in sub), held=held)


def main():
    out = pathlib.Path(next(a for a in sys.argv[1:] if not a.startswith("--")))
    res, void = collections.defaultdict(list), []
    for d in sorted(p for p in out.iterdir() if p.is_dir()):
        mm = re.match(r"(.+?)(?:_(tickle|cyclonedds|fastdds))?_r(\d+)$", d.name)  # rss_r13_smaps.sh: no arm
        if not mm:
            continue
        r = one(d)
        (void.append(d.name) if r is None else res[(mm.group(1), mm.group(2) or "tickle")].append(r))
    print(f"{'build':12s} {'arm':10s} {'n':>2s} {'R13 max(pub,sub)':>16s} {'sub ru_max':>10s} {'pub ru_max':>10s} "
          f"{'sub idle':>8s} {'sub peak':>8s} {'pub peak':>8s} {'recv/run':>10s}")
    if "--by-held" in sys.argv:  # split each build's runs by whether the reorder buffer was used (held peak >= 64)
        split = collections.defaultdict(list)
        for (b, a), rs in res.items():
            for r in rs:
                split[(b, a + ("/held" if r["held"] >= 64 else "/none"))].append(r)
        res = split
    for (b, a), rs in sorted(res.items()):
        med = lambda k: statistics.median(r[k] for r in rs)  # noqa: E731
        r13 = statistics.median(max(r["sub"], r["pub"]) for r in rs)
        print(f"{b:12s} {a:10s} {len(rs):2d} {r13:16.0f} {med('sub'):10.0f} {med('pub'):10.0f} {med('idle'):8.0f} "
              f"{med('sub_peak'):8.0f} {med('pub_peak'):8.0f} {med('n'):10.0f}   sub runs {[int(r['sub']) for r in rs]} held {[r['held'] for r in rs]}")
    if void:
        print("VOID:", " ".join(void))


if __name__ == "__main__":
    main()

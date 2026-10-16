#!/usr/bin/env python3
"""Reads pmtu_bias_sweep.sh output files: per cell (ppm, bound, auto), pooled over seeds, the reader's p_mtu
(events / frames) against the link's own count of the same MTU units (lost / sent), their ratio, and the same for the
runs (q(n)'s loss). The pre-registered rule (pmtu_bias_patch.py): an arm that counts every sample passes when every
cell's p_mtu ratio is within 0.5% of 1; a cell outside it falsifies the inference. A cell with no runs, or fewer than
20 link losses pooled, says nothing and is printed VOID.

  pmtu_bias_read.py <label>=<file> [<label>=<file> ...]
"""
import re
import sys

KEY = re.compile(r"(\w+)=(\S+)")


def ratio(a, b):
    num_a, den_a = a
    num_b, den_b = b
    if den_a == 0 or den_b == 0 or num_b == 0:
        return float("nan")
    return (num_a / den_a) / (num_b / den_b)


exit_code = 0
for arg in sys.argv[1:]:
    label, path = arg.split("=", 1)
    lines = open(path).read().splitlines()
    done = bool(lines) and lines[-1] == "PMTU_SWEEP_DONE"
    fails = [l for l in lines if l.startswith("PMTU_FAIL")]
    cells = {}
    for line in lines:
        if not line.startswith("PMTU_R"):
            continue
        f = dict(KEY.findall(line))
        cell = (int(f["ppm"]), int(f["bound"]), int(f["auto"]))
        c = cells.setdefault(cell, {"runs": 0, "r": [0, 0], "l": [0, 0], "rr": [0, 0], "rl": [0, 0], "missed": 0,
                                    "collapses": 0, "n": set()})
        c["runs"] += 1
        for key, name in (("r", "reader"), ("l", "link"), ("rr", "runs_reader"), ("rl", "runs_link")):
            num, den = f[name].split("/")
            c[key][0] += int(num)
            c[key][1] += int(den)
        c["missed"] += int(f["missed"])
        c["collapses"] += int(f["collapses"])
        c["n"].add(int(f["n"]))
    print(f"== {label}: {path} ({'complete' if done else 'INCOMPLETE'}, {len(fails)} failed runs)")
    print("ppm    layout          seeds  p_mtu reader     link             ratio   q reader/link  missed  collapses")
    for cell in sorted(cells):
        c = cells[cell]
        ppm, bound, auto = cell
        layout = "AUTO cap on" if auto else (f"IP n<={bound}" if bound else "IP n=11")
        pr = c["r"][0] / c["r"][1] if c["r"][1] else float("nan")
        pl = c["l"][0] / c["l"][1] if c["l"][1] else float("nan")
        rat = ratio(c["r"], c["l"])
        qrat = ratio(c["rr"], c["rl"])
        verdict = "VOID" if c["l"][0] < 20 else ("OK" if abs(rat - 1.0) <= 0.005 else "BIASED")
        print(f"{ppm:<6} {layout:<15} {c['runs']:>5}  {pr:.5f} ({c['r'][0]:>5}) {pl:.5f} ({c['l'][0]:>5})  "
              f"{rat:.4f}  {qrat:.4f}         {c['missed']:>6}  {c['collapses']:>5}  n={sorted(c['n'])} {verdict}")
    if not done or fails:
        exit_code = 1
sys.exit(exit_code)

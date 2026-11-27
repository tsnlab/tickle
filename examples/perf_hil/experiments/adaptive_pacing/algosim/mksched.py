#!/usr/bin/env python3
"""Writes the workload schedules (sched/NAME.txt) the algosim model reads.
P*/RS/M*/S*/H*: the varying-bandwidth, rate-step, static-link and high-rate workloads of DESIGN section 27 (the first
four families are the model's patterns of DESIGN section 26). N1-N4: converted from the 5G-like schedules of
examples/perf_hil/experiments/wifitc/results/5g_schedules/N*.json (branch ab/wifi-adaptive-s1).
Format: header lines `name`, `dur_ms`, `limit_pkts`, `ge p_ppm r_ppm bad_ppm good_ppm` (optional), then `t_ms rate_kbit delay_ms loss_ppm`."""
import json, os, re, subprocess, sys

OUT = sys.argv[1]
NJSON = sys.argv[2] if len(sys.argv) > 2 else None
os.makedirs(OUT, exist_ok=True)

PAT = {
 "P5a": ("0:30 10:10 20:30 30:10 40:30 50:10 60:30 70:10 80:30 90:10 100:30 110:10 120:30 130:10 140:30 150:10 160:30 170:10", 180, 5, 10000),
 "P5b": ("0:30 30:10 60:30 90:10 120:30 150:10", 180, 5, 10000),
 "P7": ("0:20 5:19 10:6 15:24 20:26 25:7 30:14 35:15 40:25 45:12 50:25 55:25 60:14 65:8 70:18 75:16 80:12 85:22 90:30 95:9 100:22 105:25 110:23 115:17 120:17 125:27 130:16 135:16 140:11 145:20 150:7 155:23 160:28 165:12 170:27 175:29", 180, 5, 10000),
 "P6": ("0:30 60:4 65:30", 150, 5, 10000),
 "P3": ("0:30 5:29 10:28 15:27 20:27 25:26 30:25 35:24 40:23 45:22 50:21 55:20 60:20 65:19 70:18 75:17 80:16 85:15 90:14 95:14 100:13 105:12 110:11 115:10 120:9 125:8 130:7 135:7 140:6 145:5 150:4", 180, 5, 10000),
 "P4": ("0:4 5:5 10:6 15:7 20:7 25:8 30:9 35:10 40:11 45:12 50:13 55:14 60:14 65:15 70:16 75:17 80:18 85:19 90:20 95:20 100:21 105:22 110:23 115:24 120:25 125:26 130:27 135:27 140:28 145:29 150:30", 180, 5, 10000),
 "RS": ("0:30 40:4 80:30", 120, 5, 10000),
 "RSm": ("0:30 41.3:4 81.7:30", 120, 5, 10000),  # the same step, not aligned with the 4 s sample grid
 "RSd": ("0:30 40.02:4 80.6:30", 120, 5, 10000),  # the step down lands 20 ms into a transfer (a 128 KiB sample takes 35 ms at 30 Mbit/s, 1 MiB 280 ms)
 "M1": ("0:30", 100, 5, 10000),
 "M2": ("0:30", 100, 20, 30000),
 "S1": ("0:4", 100, 5, 10000),
 "S2": ("0:4", 100, 20, 30000),
 "H100": ("0:100", 60, 10, 1000),
 "H200": ("0:200", 60, 8, 1000),
}
for name, (s, dur, d, loss) in PAT.items():
    with open(f"{OUT}/{name}.txt", "w") as f:
        f.write(f"name {name}\ndur_ms {dur*1000}\nlimit_pkts 100\n")
        for p in s.split():
            t, r = p.split(":")
            f.write(f"{int(round(float(t)*1000))} {int(r)*1000} {d} {loss}\n")

def loss_ppm(s):
    m = re.match(r"([\d.]+)%$", s)
    return int(round(float(m.group(1)) * 10000)) if m else None

if NJSON:
    for n in (1, 2, 3, 4):
        d = json.load(open(f"{NJSON}/N{n}.json"))
        ge = None
        with open(f"{OUT}/N{n}.txt", "w") as f:
            f.write(f"name N{n}\ndur_ms {int(d['duration_s'])*1000}\nlimit_pkts {d['base']['limit_pkts']}\n")
            if d["base"]["loss"].startswith("gemodel"):
                # netem gemodel p r 1-h 1-k: good->bad 1%, bad->good 15%, loss in bad 60%, loss in good 0.5%
                f.write("ge 10000 150000 600000 5000\n")
            for s in d["steps"]:
                lp = loss_ppm(s["loss"]) if not s["loss"].startswith("gemodel") else 0
                f.write(f"{s['t_ms']} {s['rate_kbit']} {s['delay_ms']} {lp}\n")

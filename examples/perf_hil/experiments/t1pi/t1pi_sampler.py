#!/usr/bin/env python3
"""Read-only 1 Hz sampler of the T1-Pi client Pi (run as root, debugfs): minstrel rows with attempts, aqm counters of both hwsim phys,
wmediumd utime+stime ticks, per-core /proc/stat. One JSON line per second in OUT; stops when STOPFILE exists. SIMULATION ONLY: it reads only
the hwsim phys' debugfs entries (netdev hwsta / hwap), never wlan0's.
Usage: t1pi_sampler.py OUT STOPFILE [WMPID_FILE]"""
import glob, json, os, sys, time

out, stop = sys.argv[1], sys.argv[2]
wmf = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser("~ci/t1pi_run/run/wm.pid")
D = "/sys/kernel/debug/ieee80211"


def rc_rows(path):
    rows = []
    try:
        lines = open(path).read().splitlines()
    except OSError:
        return rows
    for l in lines:
        t = l.split()
        if len(t) < 14 or not (t[-1].isdigit() and t[-2].isdigit()):
            continue
        try:
            att, suc, sa, ss = int(t[-3]), int(t[-4]), int(t[-1]), int(t[-2])
            name, prob = t[-11], float(t[-6])
        except (ValueError, IndexError):
            continue
        flags = "".join(t[3:-11])
        if sa > 0 or any(c in flags for c in "ABCDP"):
            rows.append([name, flags, prob, ss, sa])
    return rows


def aqm_row(path):
    try:
        L = open(path).read().splitlines()
    except OSError:
        return None
    s = {"backlog_pkts": 0, "drops": 0, "overlimit": 0, "tx_pkts": 0}
    for l in L[1:]:
        t = l.split()
        if len(t) >= 10:
            s["backlog_pkts"] += int(t[2]); s["drops"] += int(t[4]); s["overlimit"] += int(t[6]); s["tx_pkts"] += int(t[9])
    return s


def ticks(pid):
    try:
        f = open("/proc/%s/stat" % pid).read().rsplit(")", 1)[1].split()
        return int(f[11]) + int(f[12])
    except (OSError, IndexError, ValueError):
        return None


def cpu():
    r = []
    for l in open("/proc/stat"):
        if l.startswith("cpu") and l[3].isdigit():
            v = [int(x) for x in l.split()[1:]]
            r.append([sum(v) - v[3] - v[4], sum(v)])  # busy (not idle/iowait), total
    return r


fo = open(out, "w")
nxt = time.time()
while not os.path.exists(stop):
    try:
        pid = open(wmf).read().strip()
    except OSError:
        pid = ""
    rec = {"t": time.time(), "wm_ticks": ticks(pid) if pid else None, "cpu": cpu()}
    for nd, key in (("hwsta", "sta"), ("hwap", "ap")):
        rows = []
        for p in glob.glob("%s/*/netdev:%s/stations/*/rc_stats" % (D, nd)):
            rows += rc_rows(p)
        rec[key] = rows
        q = glob.glob("%s/*/netdev:%s/aqm" % (D, nd))
        rec[key + "_aqm"] = aqm_row(q[0]) if q else None
    for p in glob.glob("%s/*/aqm" % D):
        try:
            rec.setdefault("phy_aqm", []).append(dict(l.split()[1:3] for l in open(p).read().splitlines()[1:] if len(l.split()) >= 3))
        except OSError:
            pass
    fo.write(json.dumps(rec) + "\n"); fo.flush()
    nxt += 1.0
    time.sleep(max(0.0, nxt - time.time()))
fo.close()

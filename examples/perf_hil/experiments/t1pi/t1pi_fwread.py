#!/usr/bin/env python3
"""Reads a t1pi_fwcmp run directory (t1pi_fw.sh) and prints the comparison table with the registered verdicts.
Copy of the PC's t1fw_read.py (T1_FRAMEWORK_CMP_PLAN.md, the same rules) with two Pi changes: rel_large one-way latencies cross two hosts
whose CLOCK_MONOTONIC differ, so the offset measured before and after the run (t1pi_clock.py, NAME.clockoff / NAME.clockoff2) is
subtracted; the echo-side one-way figures of the small cells (cross-host, not scored) are not computed.

Rules (FRAMEWORK_CMP_PLAN.md section 6): per cell, arm and metric TickLE against each scored vendor (cyclonedds, fastdds) over the
runs of each framework (copies A and A2 pooled): WIN when every TickLE run strictly beats every vendor run, LOSE when every vendor
run strictly beats every TickLE run, else TIE. VOID for a framework-metric when |median(A2) - median(A)| > 10% of median(A); a pair
with a VOID side is VOID. zenohpico is printed, never scored. Latency with no scored sample counts as infinity (worst).
Usage: t1pi_fwread.py <run dir> [--md]
"""
import os, re, sys, statistics

FWS = ["tickle", "cyclonedds", "fastdds", "zenohpico"]
SCORED_VENDORS = ["cyclonedds", "fastdds"]
INF = float("inf")


def kv(line):
    d = {}
    for tok in line.replace("RESULT:", "RESULT").split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    return d


def num(d, k, default=0.0):
    try:
        return float(d[k])
    except (KeyError, ValueError):
        return default


def result(path, role=None):
    if not os.path.exists(path):
        return None
    for line in open(path, errors="replace"):
        if line.startswith("RESULT"):
            d = kv(line)
            if role is None or d.get("role") == role:
                return d
    return None


def clock_offset_ms(base):
    """Mean of the offsets measured before and after the run, milliseconds (server clock - client clock); 0 if absent."""
    vals = []
    for suffix in (".clockoff", ".clockoff2"):
        try:
            m = re.search(r"offset_us=(-?[0-9.]+)", open(base + suffix).read())
        except OSError:
            m = None
        if m:
            vals.append(float(m.group(1)))
    return (sum(vals) / len(vals)) / 1000.0 if vals else 0.0


def unwrap_us(x):
    """The large nodes print (receiver clock - sender clock) as an unsigned 64-bit ns count; with the server clock behind the client's it
    wraps to about 2^64/1000 us. A value above 2^63/1000 us is a negative number."""
    return x - 18446744073709551.616 if x > 9223372036854775.0 else x


def run_metrics(base, cell):
    """base = path without .near/.far. Returns dict of metrics, or None if no RESULT (lost)."""
    near, far = result(base + ".near"), result(base + ".far")
    if near is None:
        return None
    m = {"matched": near.get("matched", "1"), "error": near.get("error", "")}
    if cell == "rel_large":
        pub = num(near, "published")
        deliv = num(far, "delivered", num(far, "scored")) if far else 0.0
        m["delivery"] = 100.0 * deliv / pub if pub > 0 else 0.0
        m["n"] = f"{int(deliv)}/{int(pub)}"
        if far and deliv > 0:
            off = clock_offset_ms(base)
            m["p50"] = unwrap_us(num(far, "latency_median_us", num(far, "p50_us"))) / 1000.0 - off
            m["p99"] = unwrap_us(num(far, "latency_p99_us", num(far, "p99_us"))) / 1000.0 - off
        else:
            m["p50"] = m["p99"] = INF
        return m
    pub, wu, sc = num(near, "published"), num(near, "warmup"), num(near, "scored")
    m["delivery"] = 100.0 * sc / (pub - wu) if pub - wu > 0 else 0.0
    m["n"] = f"{int(sc)}/{int(pub - wu) if pub > wu else 0}"
    for p in ("p50", "p90", "p99"):
        m[p] = num(near, p + "_us") / 1000.0 if sc > 0 else INF
    return m


def metrics_for(cell):
    return ["delivery", "p50", "p99"] if cell == "rel_large" else ["delivery", "p50", "p90", "p99"]


def better(metric, a, b):
    return a > b if metric == "delivery" else a < b


def med(xs):
    return statistics.median(xs) if xs else None


def fmt(metric, v):
    if v is None:
        return "n/a"
    if v == INF:
        return "none"
    return f"{v:.1f}" if metric == "delivery" else (f"{v:.2f}" if v < 100 else f"{v:.0f}")


def noise_void(metric, a_vals, a2_vals):
    if not a_vals or not a2_vals:
        return True
    ma, mb = med(a_vals), med(a2_vals)
    if ma == INF and mb == INF:
        return False
    if ma == INF or mb == INF:
        return True
    return abs(mb - ma) > 0.10 * abs(ma) if ma != 0 else mb != 0


def verdict(metric, t, v):
    if all(better(metric, x, y) for x in t for y in v):
        return "WIN"
    if all(better(metric, y, x) for x in t for y in v):
        return "LOSE"
    return "TIE"


def main():
    root = sys.argv[1]
    md = "--md" in sys.argv
    data = {}  # (cell, arm) -> fw -> copy -> [metrics]
    lost = []
    for cell in sorted(os.listdir(root)):
        cd = os.path.join(root, cell)
        if not os.path.isdir(cd) or cell == "pilot" or cell.startswith("pilot"):
            continue
        for arm in sorted(os.listdir(cd)):
            ad = os.path.join(cd, arm)
            for f in sorted(os.listdir(ad)):
                mt = re.match(r"r(\d+)_(\w+?)_(A2?)\.done$", f)
                if not mt:
                    continue
                rnd, fw, copy = mt.group(1), mt.group(2), mt.group(3)
                m = run_metrics(os.path.join(ad, f[:-5]), cell)
                if m is None:
                    lost.append(f"{cell}/{arm}/{f[:-5]}")
                    continue
                m["run"] = f"{cell}/{arm}/{f[:-5]}"
                data.setdefault((cell, arm), {}).setdefault(fw, {}).setdefault(copy, []).append(m)
    counts = {"WIN": 0, "TIE": 0, "LOSE": 0, "VOID": 0}
    print("# T1-Pi framework comparison, read by t1pi_fwread.py (simulation only; relative, not Wi-Fi performance; one simulated hop, rig Pis)\n")
    for (cell, arm), by_fw in sorted(data.items()):
        mets = metrics_for(cell)
        print(f"\n## {cell} / {arm}\n")
        print("| metric | " + " | ".join(FWS) + " | vs CycloneDDS | vs Fast DDS |")
        print("|---|" + "---:|" * len(FWS) + "---|---|")
        for metric in mets:
            cols, pooled, void = [], {}, {}
            for fw in FWS:
                a = [x[metric] for x in by_fw.get(fw, {}).get("A", [])]
                a2 = [x[metric] for x in by_fw.get(fw, {}).get("A2", [])]
                pooled[fw] = a + a2
                void[fw] = noise_void(metric, a, a2)
                allv = a + a2
                cell_txt = f"{fmt(metric, med(allv))} [{fmt(metric, min(allv)) if allv else 'n/a'}..{fmt(metric, max(allv)) if allv else 'n/a'}]"
                if void[fw]:
                    cell_txt += " V"
                cols.append(cell_txt)
            vs = []
            for vend in SCORED_VENDORS:
                if not pooled["tickle"] or not pooled[vend]:
                    vs.append("n/a")
                elif void["tickle"] or void[vend]:
                    vs.append("VOID")
                    counts["VOID"] += 1
                else:
                    r = verdict(metric, pooled["tickle"], pooled[vend])
                    vs.append(r)
                    counts[r] += 1
            unit = "%" if metric == "delivery" else " ms"
            print(f"| {metric}{unit} | " + " | ".join(cols) + f" | {vs[0]} | {vs[1]} |")
        # per-run evidence line
        for fw in FWS:
            runs = [x for c in ("A", "A2") for x in by_fw.get(fw, {}).get(c, [])]
            print(f"\n- {fw} scored/offered: " + ", ".join(x["n"] + ("" if x["matched"] == "1" else " (no match)") + (f" ({x['error']})" if x["error"] else "") for x in runs))
    print("\n## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)\n")
    print(", ".join(f"{k} {v}" for k, v in counts.items()))
    print("\nColumns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).")
    if lost:
        print("\nRUNS LOST (no RESULT after 3 attempts): " + ", ".join(lost))


if __name__ == "__main__":
    main()

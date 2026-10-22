#!/usr/bin/env python3
"""bundle10_rss reader: per build x scenario x role x table, sizeof against resident bytes, and the bundle-10 rule.

Usage: read.py OUT_DIR   (the directory run.sh wrote: OUT/<build>/<scenario>_r<rep>_<A|B>.{log,rt})

THE RULES, written before the first full run (2026-10-22) and enforced below, not only stated here:
  - A run is VOID when its .rt holds no RT line for the hosted object (the instrument could not look: RT_NONE, a
    crash before the first sample, a missing file), or when its receiving side counted nothing (CORE_HOST/RSS_NODE
    recv=0 on a sub, server or client role). A VOID run is listed and contributes nothing. "Could not look" is never
    read as "nothing resident".
  - Resident bytes of a table = its peak over the run (core: after DUR s of traffic; rmw: the highest of the 200 ms
    samples and the one taken just before the context is freed). The value across reps is the largest; the spread is
    shown as min..max.
  - Realistic scenarios are every scenario but idle.
  - A table is WORTH MOVING to caller storage when, on Linux (core) or rmw:
      (a) its resident bytes exceed 64 KiB in any realistic scenario and role, or
      (b) it is FULLY RESIDENT WHILE MOSTLY UNUSED in some realistic scenario: resident >= 90% of its sizeof and its
          nonzero bytes < 25% of its sizeof (nonzero is counted on resident pages only, a lower bound on use), with
          sizeof >= 4 KiB (below a page it shares its pages and cannot be "fully resident" on its own).
    On FreeRTOS (freertos_static.py) everything linked is resident, so the static rule is size > 4 KiB.
  - Tables already in caller storage (rmw's tt_Discovery entities, a Server/Client cache attached with
    *_set_storage()) are reported and marked so; meeting the rule there argues for a smaller default or example,
    not for an API change.
"""
import collections
import glob
import os
import re
import sys

KIB = 1024
RULE_RESIDENT = 64 * KIB
FULL_SHARE = 0.90
UNUSED_SHARE = 0.25
MIN_TABLE = 4 * KIB
SHOWN = 4 * KIB  # tables at least this large (sizeof or resident) get a row; the rest are summed as "(other fields)"
M2 = {  # INITOPTIONS_PLAN section 2, "M 2nd": the bundle-10 candidates, by the field name each build gives them
    "frag_slots": "FRAG_REASSEMBLY_SLOTS x MAX_SAMPLE",
    "scheduler": "SCHEDULER",
    "endpoints": "ENDPOINT",
    "endpoint_index": "ENDPOINT_INDEX",
    "entities": "DISCOVERED_ENTITIES (already caller storage)",
    "cache_buf": "SERVER/CLIENT_CACHE (set_storage() exists)",
}
RECEIVER_ROLES = {"sub", "server", "client"}
LINE = re.compile(r"(\w+)=(\S+)")


def parse_rt(path):
    """Fields of the last reading in an .rt file: {(obj_type, field): (size, peak_resident, peak_nonzero)}, hwm_kb."""
    fields = {}
    hwm = None
    last_label = None
    with open(path, encoding="ascii", errors="replace") as handle:
        lines = handle.read().splitlines()
    labels = [dict(LINE.findall(l)).get("label") for l in lines if l.startswith("RT ")]
    if labels:
        last_label = labels[-1]
    for line in lines:
        kv = dict(LINE.findall(line))
        if line.startswith("RT_PROC ") and kv.get("label") == last_label:
            hwm = int(kv["hwm_kb"])
        elif line.startswith("RT ") and kv.get("label") == last_label:
            obj = kv["obj"].split("#")[0]
            fields[(obj, kv["field"])] = (int(kv["size"]), int(kv["peak_resident"]), int(kv["peak_nonzero"]))
    return fields, hwm


def parse_log(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("CORE_HOST ") or line.startswith("RSS_NODE "):
                return dict(LINE.findall(line))
    return None


def table_name(obj, field):
    """The table a field belongs to; small fields fold into "(other fields)" later."""
    if obj == "rmw_tickle_context_impl_t":
        if field.startswith("tickle_context."):
            return "ctx." + field[len("tickle_context."):]
        if field.startswith("discovery."):
            return "disc." + field[len("discovery."):]
        return "impl." + field
    short = {"tt_Context": "ctx", "tt_Server": "server", "tt_Client": "client", "tt_Publisher": "pub",
             "tt_Subscriber": "sub"}.get(obj, obj)
    return short + "." + field


def m2_label(table):
    leaf = table.split(".")[-1]
    return M2.get(leaf, "")


def main():
    out = sys.argv[1]
    # rows[(build, scenario, side)] = list of runs: {table: (size, resident, nonzero)}, hwm
    runs = collections.defaultdict(list)
    roles = {}
    voids = []
    drops = {}
    for rt in sorted(glob.glob(os.path.join(out, "*", "*_r*_[AB].rt"))):
        build = os.path.basename(os.path.dirname(rt))
        m = re.match(r"(.+)_r(\d+)_([AB])\.rt$", os.path.basename(rt))
        scenario, rep, side = m.group(1), m.group(2), m.group(3)
        log = parse_log(rt[:-3] + ".log")
        fields, hwm = parse_rt(rt)
        role = log.get("role", "?") if log else "?"
        key = (build, scenario, side)
        if not fields or hwm is None:
            voids.append("%s %s r%s %s: no RT reading (instrument could not look)" % (build, scenario, rep, side))
            continue
        if log is None:
            voids.append("%s %s r%s %s: no CORE_HOST/RSS_NODE line (did not finish)" % (build, scenario, rep, side))
            continue
        netem = rt[:-5] + "_netem.txt"
        if scenario.endswith("_loss") or scenario == "reliable_loss":
            dropped = 0
            if os.path.exists(netem):
                with open(netem, encoding="ascii", errors="replace") as handle:
                    dropped = sum(int(d) for d in re.findall(r"dropped (\d+)", handle.read()))
            if dropped == 0:
                voids.append("%s %s r%s %s: a loss scenario whose netem dropped nothing (or was not recorded)" % (
                    build, scenario, rep, side))
                continue
            drops[(build, scenario)] = drops.get((build, scenario), []) + [dropped]
        if role in RECEIVER_ROLES and int(log.get("recv", "0")) == 0:
            voids.append("%s %s r%s %s (%s): received nothing" % (build, scenario, rep, side, role))
            continue
        tables = collections.defaultdict(lambda: [0, 0, 0])
        for (obj, field), (size, res, nz) in fields.items():
            if obj in ("pub_storage", "sub_storage"):
                name = obj
            else:
                name = table_name(obj, field)
            if size < SHOWN and res < SHOWN and obj not in ("pub_storage", "sub_storage") and not m2_label(name):
                name = name.split(".")[0] + ".(other fields)"
            t = tables[name]
            t[0] += size
            t[1] += res
            t[2] += nz
        runs[key].append((tables, hwm))
        roles[key] = role

    print("# bundle10_rss: resident bytes per table\n")
    print("Resident = bytes of the table on pages written in this process (pagemap present+exclusive), peak over the "
          "run, largest over reps (min..max). % RSS = resident / the process's VmHWM.\n")
    worth = collections.defaultdict(list)
    for build in sorted({k[0] for k in runs}):
        print("## %s\n" % build)
        print("| scenario | side/role | n | VmHWM kB | table | sizeof B | resident B (min..max) | % of sizeof | "
              "nonzero B | % RSS | M2 |")
        print("|---|---|---:|---:|---|---:|---:|---:|---:|---:|---|")
        for key in sorted(k for k in runs if k[0] == build):
            _, scenario, side = key
            rs = runs[key]
            hwm = max(h for _, h in rs)
            names = sorted(set().union(*[set(t) for t, _ in rs]), key=lambda n: -rs[0][0].get(n, [0, 0, 0])[0])
            for name in names:
                size = max(t[name][0] for t, _ in rs if name in t)
                ress = [t[name][1] for t, _ in rs if name in t]
                nz = max(t[name][2] for t, _ in rs if name in t)
                res = max(ress)
                share = 100.0 * res / size if size else 0.0
                rss_share = 100.0 * res / (hwm * KIB) if hwm else 0.0
                print("| %s | %s/%s | %d | %d | %s | %d | %d (%d..%d) | %.0f%% | %d | %.1f%% | %s |" % (
                    scenario, side, roles[key], len(rs), hwm, name, size, res, min(ress), max(ress), share, nz,
                    rss_share, m2_label(name)))
                if scenario == "idle" or name.endswith("(other fields)"):
                    continue
                if res > RULE_RESIDENT:
                    worth[(build, name)].append("(a) %d B resident in %s/%s" % (res, scenario, roles[key]))
                if size >= MIN_TABLE and res >= FULL_SHARE * size and nz < UNUSED_SHARE * size:
                    worth[(build, name)].append("(b) %.0f%% resident, %d B nonzero of %d in %s/%s" % (
                        share, nz, size, scenario, roles[key]))
        print()
    print("## Context totals (the object the tables live in)\n")
    print("| build | scenario | side/role | object | sizeof B | resident B (largest rep) | % of sizeof | VmHWM kB | "
          "% RSS |")
    print("|---|---|---|---|---:|---:|---:|---:|---:|")
    for key in sorted(runs):
        build, scenario, side = key
        prefixes = ("ctx.", "disc.", "impl.") if build == "rmw" else ("ctx.",)
        label = "rmw_tickle_context_impl_t (tt_Context + tt_Discovery)" if build == "rmw" else "tt_Context"
        best = None
        for tables, hwm in runs[key]:
            size = sum(v[0] for n, v in tables.items() if n.startswith(prefixes))
            res = sum(v[1] for n, v in tables.items() if n.startswith(prefixes))
            if best is None or res > best[1]:
                best = (size, res, hwm)
        print("| %s | %s | %s/%s | %s | %d | %d | %.0f%% | %d | %.1f%% |" % (
            build, scenario, side, roles[key], label, best[0], best[1], 100.0 * best[1] / best[0], best[2],
            100.0 * best[1] / (best[2] * KIB)))
    print()
    print("## Rule (pre-registered): tables that are worth moving\n")
    if not worth:
        print("none")
    for (build, name), why in sorted(worth.items()):
        reasons = sorted(set(w.split(" in ")[0].split(" ")[0] for w in why))
        print("- %s %s %s: %s; e.g. %s" % (build, name, ("[" + m2_label(name) + "]") if m2_label(name) else "",
                                            "+".join(reasons), "; ".join(why[:3])))
    print("\n## Loss treatment (netem drops per run and side, both veth ends)\n")
    for (build, scenario), d in sorted(drops.items()):
        print("- %s %s: %s" % (build, scenario, " ".join(str(x) for x in d)))
    print("\n## VOID runs\n")
    print("\n".join("- " + v for v in voids) if voids else "none")


if __name__ == "__main__":
    main()

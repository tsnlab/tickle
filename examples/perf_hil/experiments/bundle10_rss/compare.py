#!/usr/bin/env python3
"""compare.py BEFORE_REPORT AFTER_REPORT - read.py's two reports side by side (before_after.sh).

Per build and table: the most resident bytes in any scenario and role (each already the largest of its reps), before
and after; then each scenario's whole context. A table either report lists only under "(other fields)" is absent from
the other's rows and shown as "-". Prints markdown.
"""
import re
import sys

ROW = re.compile(r"^\| (\S+) \| (\S+) \| (\d+) \| (\d+) \| (\S+) \| (\d+) \| (\d+) \(")
TOTAL = re.compile(r"^\| (core|rmw) \| (\S+) \| (\S+) \| [^|]+ \| (\d+) \| (\d+) \|")


def read(path):
    tables, totals, build = {}, {}, None
    for line in open(path):
        if line.startswith("## "):
            word = line[3:].strip()
            build = word if word in ("core", "rmw") else build
            continue
        m = ROW.match(line)
        if m and build:
            scenario, role, table, size, resident = m.group(1), m.group(2), m.group(5), int(m.group(6)), int(m.group(7))
            key = (build, table)
            if scenario != "idle":
                old = tables.get(key, (size, 0))
                tables[key] = (size, max(old[1], resident))
            continue
        m = TOTAL.match(line)
        if m:
            totals[(m.group(1), m.group(2), m.group(3))] = (int(m.group(4)), int(m.group(5)))
    return tables, totals


def kb(n):
    return "-" if n is None else "%.1f" % (n / 1024)


before_t, before_c = read(sys.argv[1])
after_t, after_c = read(sys.argv[2])
print("## Tables: most resident in any realistic scenario and role (KiB), before -> after\n")
print("| build | table | sizeof KiB | before | after | saved |")
print("|---|---|---:|---:|---:|---:|")
for key in sorted(set(before_t) | set(after_t), key=lambda k: (k[0], -max(before_t.get(k, (0, 0))[1],
                                                                          after_t.get(k, (0, 0))[1]))):
    size = (before_t.get(key) or after_t.get(key))[0]
    b = before_t.get(key, (size, None))[1]
    a = after_t.get(key, (size, None))[1]
    saved = "" if a is None or b is None else kb(b - a)
    print("| %s | %s | %s | %s | %s | %s |" % (key[0], key[1], kb(size), kb(b), kb(a), saved))
print("\n## The whole context, resident (KiB), before -> after\n")
print("| build | scenario | role | sizeof KiB | before | after | saved |")
print("|---|---|---|---:|---:|---:|---:|")
for key in sorted(set(before_c) | set(after_c)):
    b = before_c.get(key)
    a = after_c.get(key)
    size = (b or a)[0]
    print("| %s | %s | %s | %s | %s | %s | %s |" % (key + (kb(size), kb(b and b[1]), kb(a and a[1]),
                                                      kb(b[1] - a[1]) if a and b else "")))

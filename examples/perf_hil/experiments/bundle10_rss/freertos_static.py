#!/usr/bin/env python3
"""bundle10_rss FreeRTOS reader: static RAM per table in each RV32 example image (freertos_static.sh's OUT).

Usage: freertos_static.py OUT

On an MCU every linked byte of .bss/.data is resident, so a table's cost is its sizeof times the objects the image
holds. THE RULE, written before the first reading (2026-10-22): a table is worth moving to caller storage when it is
larger than 4 KiB in an image. Rows: the image's .bss+.data totals, every .bss/.data symbol of at least 1 KiB, and the
fields of each TickLE object the image defines (by the symbol's type, from the layout) of at least 512 B.
"""
import os
import re
import sys

RULE = 4 * 1024
FIELD_MIN = 512
ROLES = ("publisher", "subscriber", "server", "client")
OBJECT_SYMBOLS = {"node": "tt_Context", "server": "tt_Server", "client": "tt_Client", "pub": "tt_Publisher",
                  "sub": "tt_Subscriber"}


def main():
    out = sys.argv[1]
    print("# FreeRTOS RV32 static RAM per table\n")
    if os.path.exists(os.path.join(out, "head.txt")):
        print("Built from %s\n" % open(os.path.join(out, "head.txt"), encoding="ascii").read().strip())
    worth = {}
    for role in ROLES:
        sizes = {}
        with open(os.path.join(out, role + ".size"), encoding="ascii") as handle:
            for line in handle:
                m = re.match(r"^(\.\S+)\s+(\d+)\s+", line)
                if m:
                    sizes[m.group(1)] = int(m.group(2))
        ram = sum(v for k, v in sizes.items() if k in (".bss", ".sbss", ".data", ".sdata"))
        layout = {}
        types = {}
        with open(os.path.join(out, role + ".layout"), encoding="ascii") as handle:
            for line in handle:
                p = line.split()
                if p[0] == "TYPE":
                    types[p[1]] = int(p[2])
                elif p[0] == "FIELD":
                    layout.setdefault(p[1], []).append((p[2], int(p[4])))
        syms = []
        with open(os.path.join(out, role + ".syms"), encoding="ascii") as handle:
            for line in handle:
                size, kind, name = line.split()
                if not name.startswith("_stack_"):  # linker-script markers, not objects
                    syms.append((int(size), kind, name))
        print("## %s image: .bss+.data %d B\n" % (role, ram))
        print("| symbol / table | static B | rule (> 4 KiB) |")
        print("|---|---:|---|")
        for size, kind, name in sorted(syms, reverse=True):
            print("| %s (%s) | %d | %s |" % (name, kind, size, "MEETS" if size > RULE else ""))
            typ = OBJECT_SYMBOLS.get(name)
            if typ and typ in types and types[typ] == size:
                for field, fsize in sorted(layout.get(typ, []), key=lambda x: -x[1]):
                    if fsize >= FIELD_MIN:
                        meets = fsize > RULE
                        print("| &nbsp;&nbsp;%s.%s | %d | %s |" % (name, field, fsize, "MEETS" if meets else ""))
                        if meets:
                            worth.setdefault("%s.%s" % (typ, field), []).append((role, fsize))
        print()
    print("## Rule: TickLE object fields above 4 KiB static\n")
    for key, rows in sorted(worth.items()):
        print("- %s: %s" % (key, ", ".join("%s %d B" % r for r in rows)))


if __name__ == "__main__":
    main()

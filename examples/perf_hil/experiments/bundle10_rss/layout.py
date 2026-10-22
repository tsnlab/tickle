# bundle10_rss: the field layout rss_tables.c reads, taken from a build's own debug info by gdb, so the measured
# offsets and sizes are the measured build's and no list of fields is kept by hand.
#
#   RT_TYPES="struct tt_Context;struct tt_Server" RT_OUT=layout.txt gdb -batch -nx -x layout.py <binary or .so>
#
# For each type in RT_TYPES: "TYPE <name> <sizeof>", then one "FIELD <name> <path> <offset> <size>" per member. A
# member whose type is a struct named in RT_EXPAND (default "struct tt_Context;struct tt_Discovery") is replaced by its
# own members, path "outer.inner", offsets from the outer object - so rmw_tickle's context implementation, which holds
# the core context and the discovery table, is read table by table. Arrays and other structs are leaves.
import os

import gdb  # pylint: disable=import-error

TYPES = [t for t in os.environ["RT_TYPES"].split(";") if t]
EXPAND = set(t for t in os.environ.get("RT_EXPAND", "struct tt_Context;struct tt_Discovery").split(";") if t)
OUT = os.environ["RT_OUT"]


def tag(name):
    return name.replace("struct ", "").replace(" ", "_")


def walk(typ, prefix, base, out, name):
    for field in typ.fields():
        if field.bitpos is None or not field.name:
            continue
        ftype = field.type.strip_typedefs()
        off = base + field.bitpos // 8
        path = prefix + field.name
        if ftype.code == gdb.TYPE_CODE_STRUCT and str(ftype) in EXPAND:
            walk(ftype, path + ".", off, out, name)
        else:
            out.append("FIELD %s %s %d %d" % (name, path, off, ftype.sizeof))


lines = []
for t in TYPES:
    typ = gdb.lookup_type(t).strip_typedefs()
    lines.append("TYPE %s %d" % (tag(t), typ.sizeof))
    walk(typ, "", 0, lines, tag(t))
with open(OUT, "w", encoding="ascii") as handle:
    handle.write("\n".join(lines) + "\n")
print("layout: %d lines to %s" % (len(lines), OUT))

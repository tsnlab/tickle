#!/usr/bin/env python3
"""Measure the real C sizeof of every generated TickLE struct, which is what decides whether
rmw_tickle can create a publisher, subscription, client or service for a type at all.

Why a compiled probe and not the generator's own numbers (2026-09-24): rmw hands the core
callbacks->tickle_struct_size, i.e. sizeof(the TickLE C struct with every buffer at capacity),
and tt_Node_create_*() rejects anything above tt_MAX_BUFFER_LENGTH (valid_msg_size(), tickle.c),
because the receive path decodes into a stack buffer of that size. layout.max_wire_size() is the
*wire* bound and differs from sizeof both ways - a plain string is 2 B on the wire but a char*
(8 B on LP64) in the struct, plus C padding - and nothing in the generator models the C layout of
a variable struct. p2_inventory.py's first classification used max_wire_size and was therefore
answering the wrong question; this replaces it as the pass criterion.

For each interface in the (annotated) package tree it renders TickLE's own header exactly as the
generator does (render_topic / render_service, so the header's own #pragma pack applies), compiles
a one-line program printing sizeof of each top-level struct, and runs it. Numbers are for the
host's ABI, printed in the header line; char* is 8 B on x86_64 and aarch64 (the rpis) alike.

Usage: p2_sizeof_probe.py PKG_TREE [OUT_DIR]
  PKG_TREE as produced by p2_apply_capacities.py (one <pkg>/ directory per package).
Output (stdout), one row per struct: label, sizeof, CREATE_OK|CREATE_FAILS, max_wire_size.
"""
import pathlib
import platform
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools" / "typesupport"))
sys.path.insert(0, str(REPO / "rmw_tickle" / "rosidl_typesupport_tickle_c"))
sys.path.insert(0, str(HERE))

from tickle_typesupport import _rosidl_parser as rosidl  # noqa: E402
from rosidl_typesupport_tickle_c import ros2_resolve  # noqa: E402
from tickle_typesupport import adapt, layout, model, render  # noqa: E402
from p2_inventory import packages  # noqa: E402

LIMIT = model.TT_MAX_BUFFER_LENGTH


def main(argv):
    tree = pathlib.Path(argv[1]).resolve()
    out = pathlib.Path(argv[2] if len(argv) > 2 else tempfile.mkdtemp(prefix="p2_sizeof_"))
    pkgs = packages([tree])
    typesupport_packages = set(pkgs)
    include_dirs = [str(tree)]
    print(f"# sizeof probe, {platform.machine()}, limit {LIMIT} B (tt_MAX_BUFFER_LENGTH)")

    for pkg in sorted(pkgs):
        (out / pkg).mkdir(parents=True, exist_ok=True)
    jobs = []
    for pkg in sorted(pkgs):
        pdir = pkgs[pkg]
        for sub in ("msg", "srv"):
            d = pdir / sub
            if not d.is_dir():
                continue
            for f in sorted(d.glob(f"*.{sub}")):
                name = f.stem
                label = f"{pkg}/{sub}/{name}"
                resolver = ros2_resolve.Ros2Resolver(pkg, str(pdir / "msg"), include_dirs, typesupport_packages)
                try:
                    text = f.read_text(encoding="utf-8")
                    if sub == "msg":
                        ir = adapt.adapt_message(name, rosidl.parse_message_string(pkg, name, text), resolver)
                        header, _ = render.render_topic(ir)
                        hname = f"{name}.h"
                        structs = [(label, f"{name}Data", layout.max_wire_size(ir.data))]
                    else:
                        ir = adapt.adapt_service(f"{name}_srv", rosidl.parse_service_string(pkg, name, text), resolver)
                        header, _ = render.render_service(ir)
                        hname = f"{name}_srv.h"
                        structs = [(f"{label}_Request", f"{name}_srvRequest", layout.max_wire_size(ir.request)),
                                   (f"{label}_Response", f"{name}_srvResponse", layout.max_wire_size(ir.response))]
                except Exception as e:  # noqa: BLE001 - every rejection is data here
                    print(f"{label}\t-\tNOT_GENERATED\t{type(e).__name__}: {e}")
                    continue
                (out / pkg / hname).write_text(header, encoding="utf-8")
                deps = sorted({p for (p, _m) in resolver.resolved_structs} - {pkg})
                jobs.append((pkg, hname, structs, deps))

    for pkg, hname, structs, deps in jobs:
        prog = "#include <stdio.h>\n#include \"%s\"\nint main(void){\n%s  return 0;\n}\n" % (
            hname, "".join(f'  printf("%zu\\n", sizeof(struct {s}));\n' for _l, s, _w in structs))
        src = out / pkg / f"probe_{hname[:-2]}.c"
        exe = out / pkg / f"probe_{hname[:-2]}"
        src.write_text(prog, encoding="utf-8")
        incs = ["-I", str(REPO / "include"), "-I", str(out / pkg)]
        for dep in deps:
            incs += ["-I", str(out / dep)]
        cc = subprocess.run(["cc", "-std=c11", "-w", *incs, str(src), "-o", str(exe)], capture_output=True, text=True)
        if cc.returncode != 0:
            err = (cc.stderr.strip().splitlines() or ["?"])[0]
            for label, _s, _w in structs:
                print(f"{label}\t-\tCOMPILE_FAILED\t{err}")
            continue
        sizes = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.split()
        for (label, _s, wire), size in zip(structs, sizes):
            verdict = "CREATE_OK" if int(size) <= LIMIT else "CREATE_FAILS"
            print(f"{label}\t{size}\t{verdict}\tmax_wire_size {wire}")


if __name__ == "__main__":
    main(sys.argv)

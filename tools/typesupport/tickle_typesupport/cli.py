# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Generate TickLE codecs from .msg/.srv interface files (M1: scalars, strings, constants,
defaults - see tools/typesupport/PLAN.md for scope by milestone), or just dump what the parser
found (--dump-ir, no codegen).
"""

import argparse
import os
import sys

from . import _rosidl_parser as rosidl
from . import adapt, postprocess, render


def _type_str(field_type):
    """A compact, wire-relevant summary of a rosidl Type: base type, package (if any),
    and array-ness - deliberately not just str(field_type), so this survives the parser's own
    string formatting changing."""
    base = field_type.type
    if field_type.pkg_name:
        base = f"{field_type.pkg_name}/{base}"
    if not field_type.is_array:
        return base
    if field_type.array_size is None:
        return f"{base}[]"
    bound = "<=" if field_type.is_upper_bound else ""
    return f"{base}[{bound}{field_type.array_size}]"


def _dump_message(spec, indent=""):
    for constant in spec.constants:
        print(f"{indent}const {constant.type} {constant.name} = {constant.value!r}")
    for field in spec.fields:
        default = f" = {field.default_value!r}" if field.default_value is not None else ""
        print(f"{indent}{_type_str(field.type):<24} {field.name}{default}")


def _guess_package_and_name(path):
    """rosidl's parser wants (package_name, interface_name) - neither is recoverable from a bare
    file path with certainty, but the parent directory (or its parent, for a ROS 2-style
    pkg/msg/Foo.msg layout) is the closest stand-in this stub needs for a readable dump."""
    stem = os.path.splitext(os.path.basename(path))[0]
    parent = os.path.basename(os.path.dirname(os.path.abspath(path)))
    if parent in ("msg", "srv", "action"):
        parent = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(path))))
    return parent or "unknown", stem


def dump_interface(path):
    package, name = _guess_package_and_name(path)
    with open(path, encoding="utf-8") as f:
        text = f.read()

    if path.endswith(".msg"):
        spec = rosidl.parse_message_string(package, name, text)
        print(f"# {path}  (message {package}/{name})")
        _dump_message(spec)
    elif path.endswith(".srv"):
        spec = rosidl.parse_service_string(package, name, text)
        print(f"# {path}  (service {package}/{name})")
        print("  request:")
        _dump_message(spec.request, indent="    ")
        print("  response:")
        _dump_message(spec.response, indent="    ")
    else:
        raise SystemExit(f"{path}: expected a .msg or .srv file")


def generate_interface(path, outdir, *, name_override=None, style_dir=None):
    """Parses one .msg/.srv, generates its <Name>.h/.c, clang-formats them, and writes them into
    outdir. Returns the two file paths written."""
    package, guessed_name = _guess_package_and_name(path)
    name = name_override or guessed_name
    text = open(path, encoding="utf-8").read()
    source_label = os.path.basename(path)

    if path.endswith(".msg"):
        spec = rosidl.parse_message_string(package, guessed_name, text)
        ir = adapt.adapt_message(name, spec)
        header, source = render.render_topic(ir)
    elif path.endswith(".srv"):
        spec = rosidl.parse_service_string(package, guessed_name, text)
        ir = adapt.adapt_service(name, spec)
        header, source = render.render_service(ir)
    else:
        raise SystemExit(f"{path}: expected a .msg or .srv file")

    os.makedirs(outdir, exist_ok=True)
    header_path = os.path.join(outdir, f"{name}.h")
    source_path = os.path.join(outdir, f"{name}.c")
    fmt_dir = style_dir or outdir

    header_text = postprocess.clang_format(
        postprocess.add_banner(header, source_label), style_dir=fmt_dir, filename=f"{name}.h"
    )
    source_text = postprocess.clang_format(
        postprocess.add_banner(source, source_label), style_dir=fmt_dir, filename=f"{name}.c"
    )
    with open(header_path, "w", encoding="utf-8") as f:
        f.write(header_text)
    with open(source_path, "w", encoding="utf-8") as f:
        f.write(source_text)
    return header_path, source_path


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="tickle-typesupport",
        description="Generate TickLE codecs from .msg/.srv interface files.",
    )
    parser.add_argument("inputs", nargs="+", metavar="FILE.msg|FILE.srv")
    parser.add_argument("-O", "--outdir", help="write <Name>.c/.h here (default: alongside the input)")
    parser.add_argument("--name", help="override the generated interface name (only valid with one input)")
    parser.add_argument(
        "--style-dir",
        help="directory clang-format's -style=file search starts from (default: --outdir)",
    )
    parser.add_argument(
        "--dump-ir",
        action="store_true",
        help="parse and print the interface's fields/constants instead of generating code",
    )
    args = parser.parse_args(argv)

    if args.name and len(args.inputs) != 1:
        parser.error("--name only makes sense with a single input file")

    for path in args.inputs:
        if args.dump_ir:
            dump_interface(path)
            continue
        outdir = args.outdir or os.path.dirname(os.path.abspath(path))
        header_path, source_path = generate_interface(path, outdir, name_override=args.name, style_dir=args.style_dir)
        print(f"{path} -> {header_path}, {source_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

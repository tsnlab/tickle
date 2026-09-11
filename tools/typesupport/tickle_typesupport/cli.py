# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""M0 stub: parse a .msg/.srv with the vendored ROS 2 grammar parser and dump what it found.

Nothing here does CDR-4 layout or code generation yet (that's adapt.py / resolve.py / layout.py
/ the empy templates, coming in M1+) - this exists to prove the parsing stage end-to-end and
give M1 a real starting point instead of an empty file.
"""

import argparse
import os
import sys

from . import _rosidl_parser as rosidl


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


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="tickle-typesupport",
        description="Generate TickLE codecs from .msg/.srv interface files "
        "(M0: --dump-ir only; code generation lands in M1+, see tools/typesupport/PLAN.md).",
    )
    parser.add_argument("inputs", nargs="+", metavar="FILE.msg|FILE.srv")
    parser.add_argument(
        "--dump-ir",
        action="store_true",
        required=True,
        help="parse and print the interface's fields/constants; the only mode this milestone supports",
    )
    args = parser.parse_args(argv)

    for path in args.inputs:
        dump_interface(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())

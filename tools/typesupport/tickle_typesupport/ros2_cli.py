# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Entry point `python3 -m tickle_typesupport.ros2_cli`, invoked once per .msg by rosidl_
typesupport_tickle_c's own CMake extension (rmw_tickle/rosidl_typesupport_tickle_c/cmake/
rosidl_typesupport_tickle_c_generate_interfaces.cmake, registered into `rosidl_generate_
interfaces()`'s `rosidl_generate_idl_interfaces` extension point - rmw_tickle/PLAN.md's Milestone
1(c)) during a *real* ROS 2 `colcon build` - NOT the `tickle-typesupport` console script (cli.py),
which targets tools/typesupport's own codegen pipeline for TickLE's own examples/ and has no
notion of ROS 2 at all.

Writes, into --outdir, for one <package>/<subfolder>/<name>.msg:
  <name>.h / <name>.c                                           - TickLE's own codec (cli.py's
                                                                    generate_interface() output,
                                                                    unchanged - just invoked
                                                                    programmatically here)
  <package>__<subfolder>__<name>__rosidl_typesupport_tickle_c.h/.c - the ROS 2 <-> TickLE struct
                                                                    converter (ros2_adapter.
                                                                    render_adapter())
  <package>__<subfolder>__<name>__type_support.c                 - the rosidl_message_type_
                                                                    support_t wrapper that makes
                                                                    the above actually reachable
                                                                    from a real rmw_create_
                                                                    publisher() call (ros2_adapter.
                                                                    render_type_support() - see its
                                                                    own docstring for why this step
                                                                    is mandatory, not optional)

Messages only for now, matching the CMake extension's own current scope (rmw_tickle/PLAN.md's
Milestone 1(b)/(c) first cut) - .srv support follows once this is proven end-to-end in a real ROS 2
CI build. Nested message fields (std_msgs/Header et al.) are resolved the same way cli.py's own
-I/tickle_typesupport.builtins path works for TickLE's own codec, but this CLI does not yet also
emit a nested dependency's *own* converter/type-support-wrapper files - a real .msg using a nested
type would fail to link (undoing that gap is follow-on work, not yet needed by any test this
package's own CMakeLists.txt builds).
"""

import argparse
import os

from . import _rosidl_parser as rosidl
from . import adapt, cli, postprocess, render, ros2_adapter


def generate(package, subfolder, name, input_path, outdir, *, style_dir=None):
    """Returns the list of file paths written - same "top-level interface first" convention as
    cli.generate_interface()."""
    if subfolder != "msg":
        raise SystemExit(f"{input_path}: rosidl_typesupport_tickle_c only supports .msg so far, not .{subfolder}")

    os.makedirs(outdir, exist_ok=True)
    fmt_dir = style_dir or outdir
    source_label = os.path.basename(input_path)
    ros_name = f"{package}__{subfolder}__{name}"

    text = open(input_path, encoding="utf-8").read()
    spec = rosidl.parse_message_string(package, name, text)
    ir = adapt.adapt_message(name, spec, None)
    struct = ir.data

    tickle_header = f"{name}.h"
    header, source = render.render_topic(ir)
    written = list(cli._write_generated(name, header, source, source_label, outdir, fmt_dir))

    adapter_header_name = f"{ros_name}__rosidl_typesupport_tickle_c.h"
    adapter_source_name = f"{ros_name}__rosidl_typesupport_tickle_c.c"
    adapter_header, adapter_source = ros2_adapter.render_adapter(struct, ros_name, tickle_header)
    for filename, text_out in ((adapter_header_name, adapter_header), (adapter_source_name, adapter_source)):
        path = os.path.join(outdir, filename)
        formatted = postprocess.clang_format(
            postprocess.add_banner(text_out, source_label), style_dir=fmt_dir, filename=filename
        )
        with open(path, "w", encoding="utf-8") as f:
            f.write(formatted)
        written.append(path)

    type_support_name = f"{ros_name}__type_support.c"
    type_support_source = ros2_adapter.render_type_support(struct, ros_name, tickle_header, adapter_header_name)
    type_support_path = os.path.join(outdir, type_support_name)
    formatted = postprocess.clang_format(
        postprocess.add_banner(type_support_source, source_label), style_dir=fmt_dir, filename=type_support_name
    )
    with open(type_support_path, "w", encoding="utf-8") as f:
        f.write(formatted)
    written.append(type_support_path)

    return written


def main(argv=None):
    parser = argparse.ArgumentParser(prog="tickle-typesupport-ros2")
    parser.add_argument("--package", required=True)
    parser.add_argument("--subfolder", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--input", required=True, metavar="FILE.msg")
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--style-dir")
    args = parser.parse_args(argv)

    written = generate(
        args.package, args.subfolder, args.name, args.input, args.outdir, style_dir=args.style_dir
    )
    print(f"{args.input} -> {', '.join(written)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

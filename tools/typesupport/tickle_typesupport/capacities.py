# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Capacities for a package's unbounded fields, from a file beside the package rather than
annotations inside its .msg files.

Why a file at all: the interface packages TickLE has to serve (std_msgs, geometry_msgs, ...) are
upstream sources nobody wants to fork. An unbounded `T[]` or plain `string` needs a capacity before
TickLE can give it a fixed C buffer, and the `# @capacity N` annotation (adapt.py) would mean
editing every such .msg. A capacity file carries the same numbers without touching them, and a
user can shadow the one TickLE ships.

Format, one row per field, tab or space separated, `#` starts a comment:

    <pkg>/<msg|srv>/<Type>   <field>   <N>   [free text...]

For a service, <Type> is the service name when the field name is unique across its request and
response, or `<Name>_Request` / `<Name>_Response` to say which. Columns after N are ignored, so
rows copied from rmw_tickle/tools/p2_capacities_proposal.tsv (which carries a rule and a note)
work unchanged.

A capacity is part of the wire layout of every type that nests the field - a package nesting
std_msgs/MultiArrayLayout has to see exactly the capacity std_msgs was built with, or the two
disagree on the layout. So the file a package was generated with is installed beside it
(INSTALLED_NAME, under share/<pkg>/), and resolve.Ros2Resolver reads it from there when it adapts a
type from that package for someone else.

Every row must land. A row naming a type the package does not have, a field the type does not
have, or a field that is not unbounded is a CapacityError - never a silent no-op, and never a
decline: a typo in this file must fail the build, not quietly turn a type into "unsupported".
"""

import pathlib

INSTALLED_NAME = "tickle_capacities.tsv"


class CapacityError(ValueError):
    """A capacity file row that cannot apply. Deliberately not an adapt.UnsupportedFieldError, so
    ros2_cli's decline path (which catches that) never swallows it."""


def parse(text, package, source="<capacities>"):
    """{(subfolder, type_name): {field: capacity}} for `package`, where type_name is a message
    name, a service name, or `<Service>_Request`/`_Response`. Rows for another package are an
    error: a per-package file that names a different package was put in the wrong place."""
    table = {}
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cols = line.split()
        where = f"{source}:{lineno}"
        if len(cols) < 3:
            raise CapacityError(f"{where}: expected '<pkg>/<msg|srv>/<Type> <field> <N>', got {raw!r}")
        iface, field, count = cols[0], cols[1], cols[2]
        parts = iface.split("/")
        if len(parts) != 3 or parts[1] not in ("msg", "srv"):
            raise CapacityError(f"{where}: '{iface}' is not <pkg>/<msg|srv>/<Type>")
        pkg, subfolder, type_name = parts
        if pkg != package:
            raise CapacityError(f"{where}: row is for package '{pkg}', but this file is for '{package}'")
        try:
            capacity = int(count)
        except ValueError:
            raise CapacityError(f"{where}: capacity '{count}' is not a whole number") from None
        if capacity < 1:
            raise CapacityError(f"{where}: capacity must be at least 1, got {capacity}")
        fields = table.setdefault((subfolder, type_name), {})
        if field in fields:
            raise CapacityError(f"{where}: {iface}.{field} is listed twice")
        fields[field] = capacity
    return table


def load(path, package):
    path = pathlib.Path(path)
    return parse(path.read_text(encoding="utf-8"), package, str(path))


def find_installed(package, include_dirs):
    """The capacities `package` was generated with, from the first share root on the search path
    that has them - the same roots, in the same order, resolve.py finds the package's .msg files
    in. {} when it has none (it had no unbounded fields to size, or it uses annotations)."""
    for include_dir in include_dirs:
        candidate = pathlib.Path(include_dir) / package / INSTALLED_NAME
        if candidate.is_file():
            return load(candidate, package)
    return {}


def check_types_exist(table, package_root):
    """Every row's type has a source file in the package being generated. Checked on every
    generator run, because the generator runs once per interface: a row for a type that does not
    exist would otherwise never be read by any run, and so never reported."""
    root = pathlib.Path(package_root)
    for subfolder, type_name in table:
        base = type_name
        if subfolder == "srv":
            for suffix in ("_Request", "_Response"):
                if base.endswith(suffix) and not (root / "srv" / f"{base}.srv").is_file():
                    base = base[: -len(suffix)]
                    break
        if not (root / subfolder / f"{base}.{subfolder}").is_file():
            raise CapacityError(f"capacity row names {subfolder}/{type_name}, which is not in {root}")


def for_message(table, name):
    return dict(table.get(("msg", name), {}))


def for_service(table, name, request_fields, response_fields):
    """(request capacities, response capacities). A row under the bare service name goes to
    whichever side has the field, and must not be ambiguous."""
    request = dict(table.get(("srv", f"{name}_Request"), {}))
    response = dict(table.get(("srv", f"{name}_Response"), {}))
    for field, capacity in table.get(("srv", name), {}).items():
        in_request, in_response = field in request_fields, field in response_fields
        if in_request and in_response:
            raise CapacityError(
                f"srv/{name}.{field} exists in both the request and the response - write the row as "
                f"srv/{name}_Request or srv/{name}_Response"
            )
        if not in_request and not in_response:
            raise CapacityError(f"srv/{name} has no field '{field}' in its request or its response")
        (request if in_request else response)[field] = capacity
    return request, response

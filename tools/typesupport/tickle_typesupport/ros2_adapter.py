# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""rosidl_typesupport_tickle_c's own generation step (Milestone 1, rmw_tickle/PLAN.md) - NOT part
of the normal `tickle-typesupport` codegen path (cli.generate_interface()) at all. Where that path
generates TickLE's own bespoke struct + codec (<Name>.c/.h, used directly by examples/), this one
instead generates a *converter* between a real ROS 2 interface package's own `rosidl_generator_c`
struct (e.g. `struct std_msgs__msg__String`) and TickLE's already-generated, already-tested
struct/codec for that same message - so the actual CDR-4 wire logic in emit.py is reused
completely unchanged; only the "copy fields between two different C structs" part is new here.

Field type mapping this assumes (rosidl_runtime_c's own public, stable headers - not the rosidl/
ament_cmake *build-system* internals, which this module has nothing to do with):
  - a scalar field: the same C type on both sides (bool/uint8_t/int32_t/float/double/...) -
    direct assignment.
  - string (unbounded): `struct rosidl_runtime_c__String { char* data; size_t size; size_t
    capacity; }` (rosidl_runtime_c/string.h) - `.data` is always NUL-terminated at `.size`, so
    ros->tickle aliases it directly (zero copy, matching TickLE's own char* semantics);
    tickle->ros allocates via `rosidl_runtime_c__String__assign()`.
  - string (bounded, `string<=N` or `@capacity`): same rosidl_runtime_c__String on the ROS 2 side
    (rosidl has no fixed-capacity string type of its own - the bound is enforced here, not by its
    struct layout) - ros->tickle bounds-checks then memcpy()s into TickLE's fixed char[N+1]
    buffer; tickle->ros still just calls rosidl_runtime_c__String__assign().
  - fixed array `T[N]`: a plain C array both sides - direct memcpy(), sizes always match.
  - variable/bounded array `T[]`/`T[<=N]`: `struct rosidl_runtime_c__<T>__Sequence { T* data;
    size_t size; size_t capacity; }` (rosidl_runtime_c/primitives_sequence.h, one per primitive
    type) - ros->tickle bounds-checks then memcpy()s into TickLE's fixed buffer; tickle->ros
    allocates via `rosidl_runtime_c__<T>__Sequence__init()`. NOTE: the exact Sequence type name
    for the legacy `byte`/`char` IDL types specifically hasn't been verified against a real ROS 2
    install (no ROS 2 available in this tool's own dev/test environment - see PLAN.md) - every
    other primitive type's mapping (bool and every fixed-width int/float type) is the stable,
    long-documented rosidl_runtime_c convention.
  - nested message: recurses into that nested type's own <Ros2Name>__to_tickle/__from_tickle,
    named the same way (see ros2_nested_struct_name()).

render_adapter() above produces the converter alone - fully offline-verifiable (tests/test_ros2_
adapter.py), but on its own unreachable from a real `rmw_create_publisher()` call: the `rosidl_
message_type_support_t*` handle rcl hands an rmw implementation is always the one `rosidl_
typesupport_c` builds for that specific message, listing only whichever typesupport identifiers
were registered (via `ament_index_register_resource("rosidl_typesupport_c")`, discovered through
`get_used_typesupports()`) at the *interface package's own* `rosidl_generate_interfaces()` time -
a converter this module generates but that PLAN.md's Milestone 1(c) registration never rode into
that table on is dead code from rmw_tickle's point of view, correct or not. render_type_support()
below is the piece that actually rides along: wraps this same struct's converter plus TickLE's own
codec into a `rosidl_message_type_support_t` reachable through that exact standard dispatch chain
(see its own docstring) - see rmw_tickle/rosidl_typesupport_tickle_c/ for the CMake-side extension-
point registration this depends on.
"""

import re

# A nested field's own ROS 2 struct name ("msg" is the only subfolder that can appear nested -
# ROS 2 doesn't nest .srv types). Reads the nested WireStruct's own ros_pkg_name/ros_type_name
# (resolve.py sets these on every struct it ever resolves as a nested field, regardless of which
# resolver or c_name convention produced it) rather than deriving it from c_name itself - c_name
# alone stopped being enough once resolve.Ros2Resolver's own "<Name>Data" convention (reusing an
# already-independently-generated ROS 2 sibling message) started coexisting with resolve.
# Resolver's original "pkg__Name" one (model.WireStruct.header_name's own doc comment has the
# full story).
def ros2_nested_struct_name(nested_struct):
    return f"{nested_struct.ros_pkg_name}__msg__{nested_struct.ros_type_name}"


def ros2_struct_name(ros_pkg, ros_subfolder, ros_type_name):
    return f"{ros_pkg}__{ros_subfolder}__{ros_type_name}"


# rosidl_generator_c's own generated *header path* uses snake_case for the type-name segment
# (package and "msg"/"srv" are already snake_case) even though the C struct/type name itself
# keeps the original mixed case - e.g. std_msgs/UInt64.msg is `struct std_msgs__msg__UInt64` but
# `#include "std_msgs/msg/u_int64.h"`. Standard camelCase/PascalCase -> snake_case regex (splits
# before an uppercase-started word, and between a lowercase/digit and a following uppercase) -
# verified against that exact UInt64 -> u_int64 case, which is the one real ROS 2 example this
# tool's own dev environment (no ROS 2 install - see this module's own docstring) could check by
# hand rather than against a live rosidl.
def _camel_to_snake(name):
    step1 = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    step2 = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", step1)
    return step2.lower()


def ros2_header_path(ros_name):
    """`pkg__msg__Type` -> `pkg/msg/type.h` (the header rosidl_generator_c itself would emit).
    A .srv's `pkg__srv__Type_Request`/`pkg__srv__Type_Response` both resolve to the *same*
    `pkg/srv/type.h` - rosidl_generator_c generates one header per .srv (mirroring TickLE's own
    render_service() putting both structs in one file), not one per request/response - the
    `_Request`/`_Response` suffix is a *struct*-naming convention only, stripped here before
    snake-casing so it doesn't leak into the file name too (e.g. "set_bool__request.h", wrong)."""
    pkg, subfolder, type_name = ros_name.split("__")
    for suffix in ("_Request", "_Response"):
        if type_name.endswith(suffix):
            type_name = type_name[: -len(suffix)]
            break
    return f"{pkg}/{subfolder}/{_camel_to_snake(type_name)}.h"


def _to_tickle_field_lines(f):
    if f.kind == "scalar":
        return [f"tickle->{f.name} = ros->{f.name};"]
    if f.kind == "string" and f.capacity is None:
        return [f"tickle->{f.name} = ros->{f.name}.data;"]
    if f.kind == "string":
        return [
            f"if (ros->{f.name}.size > {f.capacity}) {{ return false; }}",
            f"memcpy(tickle->{f.name}, ros->{f.name}.data, ros->{f.name}.size);",
            f"tickle->{f.name}[ros->{f.name}.size] = '\\0';",
        ]
    if f.kind == "array" and f.array_mode == "fixed" and f.array_element_kind == "string":
        # A fixed ROS 2 string array is a plain in-place `struct rosidl_runtime_c__String[N]`
        # (no Sequence wrapper, matching TickLE's own no-count-prefix "Fixed arrays" wire rule) -
        # each element aliases the same way a plain unbounded string field's own `.data` already
        # does, just once per element. Not memcpy-able the way a scalar fixed array is: a
        # rosidl_runtime_c__String owns its own allocation, copying the struct's raw bytes would
        # alias it, not copy it.
        return [
            f"for (size_t i = 0; i < {f.array_size}; i++) {{",
            f"    tickle->{f.name}[i] = ros->{f.name}[i].data;",
            "}",
        ]
    if f.kind == "array" and f.array_mode == "fixed":
        return [f"memcpy(tickle->{f.name}, ros->{f.name}, sizeof(tickle->{f.name}));"]
    if f.kind == "array" and f.array_element_kind == "string":
        # A variable/bounded ROS 2 string array is `struct rosidl_runtime_c__String__Sequence`
        # (`.data`/`.size`/`.capacity`, rosidl_runtime_c/string_functions.h) - not one of the
        # primitive `rosidl_runtime_c__<T>__Sequence` types (needs_rosidl_string() already covers
        # the header this needs, same one a plain string field pulls in).
        return [
            f"if (ros->{f.name}.size > {f.capacity}) {{ return false; }}",
            f"for (size_t i = 0; i < ros->{f.name}.size; i++) {{",
            f"    tickle->{f.name}[i] = ros->{f.name}.data[i].data;",
            "}",
            f"tickle->{f.name}_count = (uint16_t)ros->{f.name}.size;",
        ]
    if f.kind == "array":
        return [
            f"if (ros->{f.name}.size > {f.capacity}) {{ return false; }}",
            f"memcpy(tickle->{f.name}, ros->{f.name}.data, ros->{f.name}.size * sizeof(*tickle->{f.name}));",
            f"tickle->{f.name}_count = (uint16_t)ros->{f.name}.size;",
        ]
    if f.kind == "nested":
        nested_ros_name = ros2_nested_struct_name(f.nested)
        return [
            f"if (!{nested_ros_name}__to_tickle(&ros->{f.name}, &tickle->{f.name})) {{ return false; }}",
        ]
    raise NotImplementedError(f.kind)


def _from_tickle_field_lines(f):
    if f.kind == "scalar":
        return [f"ros->{f.name} = tickle->{f.name};"]
    if f.kind == "string":
        # Bounded or not, the ROS 2 side is always a real owned rosidl_runtime_c__String -
        # __assign() allocates+copies, matching rosidl's own ownership contract (a decoded
        # message it hands to application code must own its own memory, unlike TickLE's own
        # decode(), which aliases the rx buffer - see DESIGN.md's "Strings" rule).
        return [f"if (!rosidl_runtime_c__String__assign(&ros->{f.name}, tickle->{f.name})) {{ return false; }}"]
    if f.kind == "array" and f.array_mode == "fixed" and f.array_element_kind == "string":
        return [
            f"for (size_t i = 0; i < {f.array_size}; i++) {{",
            f"    if (!rosidl_runtime_c__String__assign(&ros->{f.name}[i], tickle->{f.name}[i])) {{ return false; }}",
            "}",
        ]
    if f.kind == "array" and f.array_mode == "fixed":
        return [f"memcpy(ros->{f.name}, tickle->{f.name}, sizeof(ros->{f.name}));"]
    if f.kind == "array" and f.array_element_kind == "string":
        count_var = f"tickle->{f.name}_count"
        return [
            f"if (!rosidl_runtime_c__String__Sequence__init(&ros->{f.name}, {count_var})) {{ return false; }}",
            f"for (size_t i = 0; i < {count_var}; i++) {{",
            f"    if (!rosidl_runtime_c__String__assign(&ros->{f.name}.data[i], tickle->{f.name}[i])) {{ return false; }}",
            "}",
        ]
    if f.kind == "array":
        count_var = f"tickle->{f.name}_count"
        return [
            f"if (!rosidl_runtime_c__{f.scalar_type}__Sequence__init(&ros->{f.name}, {count_var})) {{ return false; }}",
            f"memcpy(ros->{f.name}.data, tickle->{f.name}, (size_t){count_var} * sizeof(*tickle->{f.name}));",
        ]
    if f.kind == "nested":
        nested_ros_name = ros2_nested_struct_name(f.nested)
        return [
            f"if (!{nested_ros_name}__from_tickle(&tickle->{f.name}, &ros->{f.name})) {{ return false; }}",
        ]
    raise NotImplementedError(f.kind)


def emit_to_tickle(struct, ros_name):
    """`<ros_name>__to_tickle(const struct <ros_name>*, struct <struct.c_name>*) -> bool` - false
    on a bounds check failure (a variable array/bounded string longer than TickLE's resolved
    capacity), matching the existing codec's own `-2` capacity-rejection contract one level up.
    Not `static` - the typesupport wrapper this same package generates (Milestone 1's next piece)
    calls this from a different translation unit."""
    lines = [f"bool {ros_name}__to_tickle(const struct {ros_name}* ros, struct {struct.c_name}* tickle) {{"]
    for f in struct.fields:
        lines += [f"    {line}" for line in _to_tickle_field_lines(f)]
    lines += ["    return true;", "}"]
    return lines


def emit_from_tickle(struct, ros_name):
    """The decode-side mirror of emit_to_tickle() - false only if a rosidl_runtime_c allocation
    fails (bad_alloc-equivalent), since a value already inside TickLE's own resolved-capacity
    buffers can never overflow the ROS 2 side (rosidl_runtime_c__*__Sequence/String are all
    dynamically sized). Also not `static`, for the same reason as emit_to_tickle()."""
    lines = [f"bool {ros_name}__from_tickle(const struct {struct.c_name}* tickle, struct {ros_name}* ros) {{"]
    for f in struct.fields:
        lines += [f"    {line}" for line in _from_tickle_field_lines(f)]
    lines += ["    return true;", "}"]
    return lines


def nested_ros_includes(struct):
    """The ROS 2-generated header for each of this struct's own *directly* nested fields - same
    "only one level, each nested header pulls in what it itself needs" reasoning as render.py's
    own _nested_includes()."""
    names = sorted({ros2_nested_struct_name(f.nested) for f in struct.fields if f.kind == "nested"})
    return [ros2_header_path(name) for name in names]


def nested_adapter_includes(struct):
    """The *other* message's own already-generated `<name>__rosidl_typesupport_tickle_c.h` for
    each of this struct's own directly nested fields - declares the `__to_tickle`/`__from_tickle`
    functions emit_to_tickle()/emit_from_tickle() call for a `kind == "nested"` field. Unlike
    nested_ros_includes() (whose struct-definition purpose the top-level ROS header this same file
    already includes normally satisfies transitively, since a real ROS 2 message header always
    #includes its own nested fields' headers), there is no other file that would otherwise pull
    this one in - it must be included directly."""
    names = sorted({ros2_nested_struct_name(f.nested) for f in struct.fields if f.kind == "nested"})
    return [f"{name}__rosidl_typesupport_tickle_c.h" for name in names]


def needs_rosidl_string(struct):
    # rosidl_runtime_c/string_functions.h declares both rosidl_runtime_c__String__* AND
    # rosidl_runtime_c__String__Sequence__* (String, unlike a primitive scalar type, gets one
    # self-contained header covering both shapes) - a string array field (either array_mode)
    # needs the exact same header a plain string field does, no separate one.
    return any(
        f.kind == "string" or (f.kind == "array" and f.array_element_kind == "string") for f in struct.fields
    )


def needs_rosidl_sequence(struct):
    return any(f.kind == "array" and f.array_mode == "variable" for f in struct.fields)


def sequence_element_types(struct):
    """Distinct rosidl_runtime_c *primitive* Sequence element types this struct's own (not nested
    structs') variable arrays need - each is its own header, rosidl_runtime_c/<type>__functions.h.
    Excludes a string array's own variable arrays: rosidl_runtime_c__String__Sequence isn't one of
    the primitive Sequence types this covers - needs_rosidl_string() above already brings in the
    one header (string_functions.h) it actually needs."""
    return sorted(
        {
            f.scalar_type
            for f in struct.fields
            if f.kind == "array" and f.array_mode == "variable" and f.array_element_kind == "scalar"
        }
    )


def render_adapter(struct, ros_name, tickle_header):
    """Returns (header_text, source_text) for <ros_name>__rosidl_typesupport_tickle_c.{h,c} -
    plain string assembly (not empy) since this is a fixed two-function shape, not a per-kind
    template the way struct.h.em/struct.c.em are. `tickle_header` is the *interface's* own
    generated header filename (e.g. "Arrays.h" - cli.generate_interface()'s own `<name>.h`,
    keyed off the .msg/.srv's interface name) - NOT `f"{struct.c_name}.h"`: struct.c_name is
    "ArraysData", but that struct is declared *inside* Arrays.h, not its own same-named file (a
    .srv's request/response structs share one file the same way)."""
    header_lines = [
        "#pragma once",
        "",
        "#include <stdbool.h>",
        "",
        f'#include "{ros2_header_path(ros_name)}"',
        f'#include "{tickle_header}"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        f"bool {ros_name}__to_tickle(const struct {ros_name}* ros, struct {struct.c_name}* tickle);",
        f"bool {ros_name}__from_tickle(const struct {struct.c_name}* tickle, struct {ros_name}* ros);",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
    ]

    source_includes = ["#include <stdbool.h>"]
    if needs_rosidl_sequence(struct):
        # Only a variable array's own `(uint16_t)ros->*.size` cast (see _to_tickle_field_lines)
        # actually names a stdint.h type in this file - a plain scalar/fixed-array/string field's
        # generated lines never do (the type itself is only ever named in the *struct*
        # declarations, which live in the headers this file includes, not here).
        source_includes.append("#include <stdint.h>")
    source_includes += [
        "#include <string.h>",
        "",
        f'#include "{ros_name}__rosidl_typesupport_tickle_c.h"',
        "",
        # Redundant with the paired header above transitively pulling both of these in - included
        # directly anyway, matching this generator's own misc-include-cleaner convention
        # elsewhere (every file includes what it directly uses, not just what happens to arrive
        # transitively).
        f'#include "{ros2_header_path(ros_name)}"',
        f'#include "{tickle_header}"',
    ]
    if needs_rosidl_string(struct):
        source_includes.append('#include "rosidl_runtime_c/string_functions.h"')
    if sequence_element_types(struct):
        # One header for every rosidl_runtime_c__<T>__Sequence type, regardless of how many
        # distinct primitive element types this struct's own variable arrays actually use -
        # rosidl_runtime_c/primitives_sequence_functions.h declares all of them together, it
        # isn't split per element type.
        source_includes.append('#include "rosidl_runtime_c/primitives_sequence_functions.h"')
    for adapter_header_name in nested_adapter_includes(struct):
        source_includes.append(f'#include "{adapter_header_name}"')

    source_lines = source_includes + [""] + emit_to_tickle(struct, ros_name) + [""] + emit_from_tickle(
        struct, ros_name
    )

    header = "\n".join(header_lines) + "\n"
    source = "\n".join(source_lines) + "\n"
    return header, source


def render_type_support(struct, ros_name, tickle_header, adapter_header):
    """Returns the .c source for <ros_name>__type_support.c - rmw_tickle/PLAN.md's Milestone
    1(b)/(c), the final wrapping step render_adapter() alone doesn't do: a `rosidl_message_type_
    support_t` whose `.data` is this package's own private `rosidl_typesupport_tickle_c_message_
    callbacks_t` (rosidl_typesupport_tickle_c/message_type_support.h - rosidl's typesupport
    contract never inspects `.data`'s shape, so only rmw_tickle itself and this file need to agree
    on it), pointing straight at TickLE's own already-generated codec function pointers - cast to
    the generic tt_DATA_ENCODE/tt_DATA_DECODE/tt_DATA_ENCODE_SIZE/tt_DATA_FREE typedefs the exact
    same way every examples/*/*.c's own <Name>Topic definition already casts its per-type codec
    functions - plus this same struct's own __to_tickle/__from_tickle converter from render_
    adapter() above. The accessor function follows rosidl_typesupport_interface/macros.h's
    ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME naming exactly, and `.func`/`.typesupport_
    identifier` follow rosidl_runtime_c's own get_message_typesupport_handle_function() convention
    (same one every real typesupport - introspection, fastrtps, ... - uses) - together, this is
    what makes rmw_tickle/PLAN.md's Milestone 1(c) registration (this package's own CMakeLists.txt:
    ament_index_register_resource("rosidl_typesupport_c") + ament_register_extension(...)) actually
    reachable from a real rmw_create_publisher() call's type_support handle, not just a self-
    contained offline test the way render_adapter() alone only ever was.

    Message-only for now (rmw_tickle/PLAN.md's Milestone 1(b)/(c) first cut) - `struct.c_name`
    is assumed to have its own top-level <c_name>_encode/_decode/_encode_size/_free codec
    (cli.generate_interface()'s normal output), not a .srv request/response struct sharing one
    file; .srv support follows once the message path is proven in a real ROS 2 CI build.

    get_type_hash_func/get_type_description_func/get_type_description_sources_func (jazzy's type
    description feature - rosidl_runtime_c/type_hash.h et al.) are left NULL: nothing in rmw_
    tickle's own "Supported subset" (rmw_tickle/PLAN.md) needs runtime type introspection/hashing,
    and rosidl's own dispatch code only calls these through a *different* typesupport's handle
    (e.g. rosidl_typesupport_introspection_c's), never through ours - see this module's own
    docstring for why a leaf typesupport's `.data` shape is otherwise free to be anything."""
    pkg, subfolder, type_name = ros_name.split("__")
    callbacks_var = f"_{pkg}__{subfolder}__{type_name}__callbacks"
    handle_var = f"_{pkg}__{subfolder}__{type_name}__handle"
    return "\n".join(
        [
            "#include <stddef.h>",
            "",
            # tt_DATA_ENCODE_SIZE/tt_DATA_ENCODE/tt_DATA_DECODE/tt_DATA_FREE
            "#include <tickle/tickle.h>",
            "",
            '#include "rosidl_runtime_c/message_type_support_struct.h"',
            '#include "rosidl_typesupport_interface/macros.h"',
            '#include "rosidl_typesupport_tickle_c/identifier.h"',
            '#include "rosidl_typesupport_tickle_c/message_type_support.h"',
            "",
            f'#include "{ros2_header_path(ros_name)}"',
            f'#include "{tickle_header}"',
            f'#include "{adapter_header}"',
            "",
            f"static rosidl_typesupport_tickle_c_message_callbacks_t {callbacks_var} = {{",
            f'    .ros_type_name = "{pkg}/{subfolder}/{type_name}",',
            f"    .tickle_struct_size = sizeof(struct {struct.c_name}),",
            f"    .ros_struct_size = sizeof(struct {ros_name}),",
            f"    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&{ros_name}__to_tickle,",
            f"    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&{ros_name}__from_tickle,",
            f"    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&{struct.c_name}_encode_size,",
            f"    .tickle_encode = (tt_DATA_ENCODE)&{struct.c_name}_encode,",
            f"    .tickle_decode = (tt_DATA_DECODE)&{struct.c_name}_decode,",
            f"    .tickle_free = (tt_DATA_FREE)&{struct.c_name}_free,",
            "};",
            "",
            "// .typesupport_identifier is set on first access below, not here - a plain (non-",
            "// address) extern const char* like rosidl_typesupport_tickle_c__identifier isn't a",
            "// compile-time constant in C, so it can't be a static initializer (same reason real",
            "// rosidl_typesupport_introspection_c-generated code does this the same way).",
            f"static rosidl_message_type_support_t {handle_var} = {{",
            f"    .data = &{callbacks_var},",
            "    .func = get_message_typesupport_handle_function,",
            "    .get_type_hash_func = NULL,",
            "    .get_type_description_func = NULL,",
            "    .get_type_description_sources_func = NULL,",
            "};",
            "",
            "const rosidl_message_type_support_t *",
            f"ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_tickle_c, {pkg}, {subfolder}, {type_name})(void) {{",
            f"    if (!{handle_var}.typesupport_identifier) {{",
            f"        {handle_var}.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;",
            "    }",
            f"    return &{handle_var};",
            "}",
            "",
        ]
    )


def render_service_type_support(ros_service_name):
    """Returns the .c source for <ros_service_name>__type_support.c - the service-level
    counterpart to render_type_support() above: wraps a `rosidl_service_type_support_t` whose
    `.request_typesupport`/`.response_typesupport` point directly at the two message-level
    handles this same interface package's own `<..>_Request`/`<..>_Response` `render_type_
    support()` output already provides, reached by calling their own `ROSIDL_TYPESUPPORT_
    INTERFACE__MESSAGE_SYMBOL_NAME` accessor functions directly (forward-declared here, not
    `#include`d from a generated header - see the comment in the generated output itself) -
    real `rosidl_typesupport_introspection_c`-generated code reaches a nested type's own message
    handle the exact same way. `ros_service_name` is "pkg__srv__Type" (NOT `..._Request`/
    `..._Response` - those are derived here, matching cli.py's/adapt.py's own `f"{name}Request"`/
    `f"{name}Response"` naming for the TickLE-side structs those two calls, in turn, wrap).

    `.data` is this package's own private `rosidl_typesupport_tickle_c_service_callbacks_t`
    (`rosidl_typesupport_tickle_c/service_type_support.h`) - deliberately minimal, since `.request_
    typesupport`/`.response_typesupport` (rosidl's own, standard fields) already carry everything
    needed to reach each side's own message callbacks via rmw_tickle's own rmw_tickle_get_message_
    callbacks() - this only adds what neither side has: the service's own type name, which
    TickLE's `struct tt_Service` needs the same way `struct tt_Topic` needs a message's own
    `ros_type_name` (see render_type_support()'s own doc comment)."""
    pkg, subfolder, type_name = ros_service_name.split("__")
    request_ros_name = f"{ros_service_name}_Request"
    response_ros_name = f"{ros_service_name}_Response"
    callbacks_var = f"_{pkg}__{subfolder}__{type_name}__callbacks"
    handle_var = f"_{pkg}__{subfolder}__{type_name}__handle"
    return "\n".join(
        [
            "#include <stddef.h>",
            "",
            '#include "rosidl_runtime_c/message_type_support_struct.h"',
            '#include "rosidl_runtime_c/service_type_support_struct.h"',
            '#include "rosidl_typesupport_interface/macros.h"',
            '#include "rosidl_typesupport_tickle_c/identifier.h"',
            '#include "rosidl_typesupport_tickle_c/service_type_support.h"',
            "",
            "// Forward-declared rather than #included from a generated header (rosidl_typesupport_c's",
            "// own \"single typesupport\" dispatch shortcut expects one, but that path never actually",
            "// runs against this package - see rmw_tickle/PLAN.md's Milestone 1(c) notes on the",
            "// dlopen()-based multi-typesupport path being the one that matters) - both functions are",
            f"// defined in this same interface package's build, in {request_ros_name}__type_support.c",
            f"// and {response_ros_name}__type_support.c.",
            "const rosidl_message_type_support_t *",
            f"ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_tickle_c, {pkg}, {subfolder}, {type_name}_Request)(void);",
            "const rosidl_message_type_support_t *",
            f"ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_tickle_c, {pkg}, {subfolder}, {type_name}_Response)(void);",
            "",
            f"static rosidl_typesupport_tickle_c_service_callbacks_t {callbacks_var} = {{",
            f'    .ros_type_name = "{pkg}/{subfolder}/{type_name}",',
            "};",
            "",
            "// .typesupport_identifier/.request_typesupport/.response_typesupport are set on first",
            "// access below, not here - the identifier for the same reason as render_type_support()'s",
            "// own message-level handle (a plain extern const char* isn't a C constant expression);",
            "// the two typesupport pointers because calling another translation unit's accessor",
            "// function isn't a constant expression either.",
            f"static rosidl_service_type_support_t {handle_var} = {{",
            f"    .data = &{callbacks_var},",
            "    .func = get_service_typesupport_handle_function,",
            "    .event_typesupport = NULL,",
            "    .event_message_create_handle_function = NULL,",
            "    .event_message_destroy_handle_function = NULL,",
            "    .get_type_hash_func = NULL,",
            "    .get_type_description_func = NULL,",
            "    .get_type_description_sources_func = NULL,",
            "};",
            "",
            "const rosidl_service_type_support_t *",
            f"ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(rosidl_typesupport_tickle_c, {pkg}, {subfolder}, {type_name})(void) {{",
            f"    if (!{handle_var}.typesupport_identifier) {{",
            f"        {handle_var}.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;",
            f"        {handle_var}.request_typesupport =",
            f"            ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_tickle_c, {pkg}, {subfolder}, {type_name}_Request)();",
            f"        {handle_var}.response_typesupport =",
            f"            ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_tickle_c, {pkg}, {subfolder}, {type_name}_Response)();",
            "    }",
            f"    return &{handle_var};",
            "}",
            "",
        ]
    )

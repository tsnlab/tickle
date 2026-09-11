# tickle-typesupport

Generates TickLE codecs (`<Name>.c` / `<Name>.h`) from ROS 2 `.msg` / `.srv` interface files.
See [`PLAN.md`](PLAN.md) for the full design and milestone plan, and "Interface serialization
(TickLE CDR-4)" in [`../../DESIGN.md`](../../DESIGN.md) for the wire format the generated codecs
implement.

**Status: M0** - the `.msg`/`.srv` parsing stage only. `tickle-typesupport --dump-ir <file>`
parses a real ROS 2 interface file with the vendored grammar parser and prints its fields,
constants and defaults. No CDR-4 layout, no code generation yet (M1+).

## Setup

```sh
$ python3 -m venv .venv && . .venv/bin/activate
$ pip install -e .
```

(`empy==3.3.4` - pinned to match ROS 2's own dependency - is not used yet at M0; it's only
declared so `pip install -e .` already resolves the environment M1's templates need.)

## Try it

```sh
$ tickle-typesupport --dump-ir ../../examples/UInt64.msg
$ tickle-typesupport --dump-ir ../../examples/SetBool.srv
$ tickle-typesupport --dump-ir tests/fixtures_ros2/sensor_msgs/msg/Image.msg
```

## Tests

```sh
$ python3 -m pytest tests/
```

`tests/fixtures_ros2/` holds a handful of real ROS 2 interface files (`std_msgs/Header`,
`sensor_msgs/Image`, `geometry_msgs/Twist`+`Vector3`, `std_srvs/SetBool`,
`builtin_interfaces/Time`, `diagnostic_msgs/DiagnosticStatus` - fetched unmodified from
`ros2/common_interfaces` and `ros2/rcl_interfaces` @ `jazzy`) laid out in their real
`pkg/msg/Name.msg` form, since `_rosidl_parser` validates the message name derived from that
path. `test_parse_smoke.py` proves the parser handles nested types, arrays (fixed/bounded/
unbounded), constants and defaults - not just TickLE's own four small interfaces - before any
codegen is built on top of it.

## Why a vendored parser

`tickle_typesupport/_rosidl_parser.py` is `rosidl_adapter/parser.py` from
[ros2/rosidl](https://github.com/ros2/rosidl) copied in verbatim (see the file's own header for
the exact commit) rather than a `pip` dependency: it's the canonical ROS 2 `.msg`/`.srv` grammar
(so compatibility is structural, not best-effort), it's stdlib-only, and vendoring one file
avoids version skew against whatever `rosidl-adapter` release happens to be on PyPI. It is
Apache-2.0 (see its header) and used here only as a build-time tool, never linked into
`libtickle`.

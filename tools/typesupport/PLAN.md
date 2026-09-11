# TickLE typesupport — implementation plan

A Python tool that generates TickLE codecs (`<Name>.c` / `<Name>.h`) from ROS 2 `.msg` / `.srv`
interface files. Structurally inspired by `rosidl_typesupport_fastrtps` / `rosidl_generator_c`,
but the serialization is TickLE's own ("CDR-4"), not OMG CDR — TickLE is not DDS-wire-compatible.

**Status**: M0–M6 complete (see the milestone table below) — every real TickLE interface is
generated, not hand-written; see [`README.md`](README.md) for usage.

## Locked decisions

| Area | Decision |
|---|---|
| Parser | Vendor one file: `rosidl_adapter/parser.py` → `tickle_typesupport/_rosidl_parser.py` (Apache-2.0, note origin commit, do not edit) |
| Templating | `empy == 3.3.4` (pinned), then `clang-format -i` post-pass (formatting is never the template's job) |
| Lint | A `.clang-tidy` in each `examples/<proto>/` directory (InheritParentConfig; disable `readability-identifier-naming` + `readability-magic-numbers` + `readability-non-const-parameter` - one per generated-codec directory rather than a single `examples/.clang-tidy`, since that would also reach the hand-written drivers under `examples/{linux,freertos}/<proto>/`, which keep every check). Deleted the `SetBool\|UInt64\|Ping\|Bulk` whitelist from the root `.clang-tidy` |
| Alignment | CDR-4: every primitive at an offset that is a multiple of `min(sizeof, 4)`, zero-padded. 64-bit types are 4-aligned, not 8 |
| Length prefixes | Every string and array length is `uint16` (a single-datagram payload is always < 2^16) |
| Generated structs | `#pragma pack(push, 4)` + `_Static_assert(sizeof / offsetof …)` |
| Framing | `tt_CallRequestHeader` grows 7 → 8 bytes (`uint8 _reserved`) so its CDR payload is 4-aligned. Pre-1.0 wire change, approved |
| Constants | rosidl_generator_c convention: integer → `enum`, float/string → `static const` |
| Defaults | generate `<Msg>_init()` (not serialized) |
| Layout | Flatten: `.msg`/`.srv` + generated `.c`/`.h` live together in `examples/<proto>/`; drivers stay in `examples/{linux,freertos}/<proto>/` |
| FreeRTOS | uses the generated codecs verbatim (cross-compiled). The four hand-written codecs are deleted |
| Tool path | `tools/typesupport/` |

## Serialization spec — "TickLE CDR-4"

Coordinates: offsets/alignment are relative to the first byte of the message payload (where
`*_encode` starts writing). Framing headers are not part of this.

- **Byte order**: encoder always writes host-native. Decoder byte-swaps each multi-byte scalar
  iff `is_native_endian == false`. Padding is written as zero and skipped (never inspected).
- **Alignment**: each primitive at an offset that is a multiple of `min(sizeof, 4)`.
  1-byte: any. 2-byte: %2. 4-byte: %4. 8-byte (int64/uint64/float64): **%4**.
  The payload itself starts at a 4-aligned offset in the tx/rx buffer (framing layout +
  `_Alignas(4)` on the buffers; `_Static_assert` verifies). DATA payload @24, CALLRESPONSE @16,
  CALLREQUEST @16 (after the header's 8-byte padding).
- **string**: align 2 → `uint16 length` (bytes to follow, including the `\0`; empty string =
  length 1) → `length` bytes → pad to 4. `length > tt_MAX_STRING_LENGTH` → encode error `-2`.
  Decode aliases into the buffer, verifies the trailing `\0`; no copy, no free.
- **fixed array `T[N]`**: N elements, each aligned per T. No length prefix.
- **variable array `T[]` / `T[<=N]`**: align 2 → `uint16 count` → pad to T's alignment →
  `count` elements. Decode: `count <= capacity` and fits in remaining `len`, else `-1`.
- **nested message**: fields inlined recursively at the current offset. No header, no extra
  alignment beyond the first nested field's.
- **capacity** (variable array / bounded string C buffer), in priority order:
  1. field annotation `# … @capacity <N>` (a plain ROS 2 comment) → `<N>`
  2. ROS 2 upper bound `T[<=N]` / `string<=N` → `N`
  3. auto: `floor((tt_MAX_BUFFER_LENGTH − framing − max size of the other fields) / sizeof_wire(T))`
  Always emit `_Static_assert(message max serialized size <= tt_MAX_BUFFER_LENGTH)`. An explicit
  `N` that breaks it → generate-time error.
- **whole message**: serializes within one datagram. No fragmentation.
- **generated struct**: `#pragma pack(push,4)` makes C layout == wire layout on every supported
  ABI, so any all-fixed-size message gets `encode_inplace` / `decode_inplace`, and
  `_Static_assert(sizeof/offsetof)` turns an ABI mismatch into a compile error.

Rationale: 4-byte (not 8-byte) alignment avoids padding `tt_SubmessageHeader` to keep batched
DATA payloads aligned, and 4-aligned 64-bit access is correct/fast on every TickLE target
(rv32: 64-bit is two 32-bit ops anyway; ARM64: allows it; x86-64: alignment-agnostic).
uint16 lengths save 2 bytes per string/array vs OMG CDR's uint32 and can't overflow in a
single datagram.

## Naming

`Foo.msg` → `struct FooData`, `FooTopic`, `FooData_*`, `.name = "FooTopic"`.
`Foo.srv` → `struct FooRequest` / `FooResponse`, `FooService`, `FooRequest_*` / `FooResponse_*`,
`.name = "FooService"`. Nested `pkg/Bar` → `struct pkg__Bar`. Constant `Foo/CONST` → `FOO__CONST`.
`--name X` overrides the filename-derived name.

## Generated per-interface API

`Foo.h`: packed struct(s); constants (`enum` / `static const`); `extern struct tt_Topic FooTopic;`
(or `tt_Service`); `void FooData_init(struct FooData*)` when there are defaults;
`FooData_encode_size / encode / decode / free`; `FooData_encode_inplace / decode_inplace` when
all-fixed-size; `_Static_assert(sizeof / offsetof)`.
`Foo.c`: the `tt_Topic` / `tt_Service` global + function bodies. Includes limited to
`<stdint.h> <stdbool.h> <string.h> <tickle/tickle.h> <tickle/hal.h>` (freestanding-safe, no
malloc, no stdio).

## Tool layout

```
tools/typesupport/
  pyproject.toml                 # install_requires: empy==3.3.4
  tickle_typesupport/
    _rosidl_parser.py            # vendored (ros2/rosidl @ <commit>), SPDX: Apache-2.0
    cli.py  adapt.py  resolve.py  builtins.py  layout.py  emit.py  render.py  postprocess.py
    templates/  topic.{h,c}.em  service.{h,c}.em  _struct.em  _encode_size.em  _encode.em  _decode.em  _init.em  _consts.em
  tests/  fixtures_ros2/  golden/  test_golden.py test_roundtrip.py test_crossendian.py test_capacity.py test_lint.py
```
Pipeline: `parse (_rosidl_parser) → resolve nesteds → adapt → IR → layout (CDR-4) → render (empy)
→ clang-format → write`. `--check` generates to a temp dir and diffs; nonzero exit on drift.

## Build integration

- `platform/linux/Makefile`: `%.c %.h &: %.msg` / `%.srv` pattern rule invokes `tickle-typesupport`.
- `platform/freertos/Makefile`: codec source path only (generated files are committed).
- Generated files are committed (a consumer with no Python must still `make`). `make regen` re-runs.
- `check-all.yml`: `pip install ./tools/typesupport`, then `--check` (or `make regen && git diff
  --exit-code`) + `make lint` over the generated output.
- `CONTRIBUTING.md`: do not edit generated codecs — edit the `.msg`/`.srv` and `make regen`.

## Milestones

| | Deliverable | Done when | |
|---|---|---|---|
| **M0** | DESIGN.md CDR-4 section; `tt_CallRequestHeader` 7→8; tx/rx buffers `_Alignas(4)` + static_asserts; `tools/typesupport/` scaffold (pinned empy, vendored parser, IR-dump stub CLI) | `make test-all` still PASS (framing regression); HIL PASS | ✅ |
| **M1** | scalars + string + `.srv` + constants + scalar/string defaults + `examples/.clang-tidy`; regenerate `UInt64` and `SetBool` | golden == regenerated (byte-equal post clang-format); roundtrip + cross-endian green; `make test` + `make sanitize` PASS | ✅ |
| **M2** | fixed + variable arrays (capacity rules) + `#pragma pack(4)` + static_asserts; new `Bulk.msg`; regenerate `Bulk` (non-inplace) | `test_capacity`; perf examples PASS; HIL throughput no regression | ✅ |
| **M3** | nested messages + `-I` include path + `std_msgs/Header` builtin + parser smoke corpus | `Image.msg` generates + compiles (Linux + FreeRTOS) + round-trips | ✅ |
| **M4** | auto `encode_inplace` / `decode_inplace` for all-fixed-size; fully regenerate `Bulk` | `Bulk` golden == the hand-written version; HIL throughput holds | ✅ (`Bulk` didn't fit "all-fixed-size" - it's prefix-aliasable instead, a new eligibility rule this milestone added; hand-written version deleted, not matched byte-for-byte) |
| **M5** | delete the 4 hand-written codecs; flatten to `examples/<proto>/`; Makefile codegen rules; delete root `.clang-tidy` whitelist; `make regen`; `check-all.yml` regen+lint+diff; new `PingPong.srv` | `test-all` + `check-all` + `test-freertos` green; zero hand codecs in the repo; HIL PASS | ✅ |
| **M6** | array default values; README / CONTRIBUTING updates; `.action` / multi-dim arrays documented as out of scope | `make test-all` + HIL latency/throughput baseline recorded | ✅ |

Out of scope: `.action`, multi-dimensional arrays, `wstring`, 128-bit, DDS/CDR wire
compatibility, XCDR2, type hashes, introspection typesupport.

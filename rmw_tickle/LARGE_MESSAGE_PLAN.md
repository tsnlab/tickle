# LARGE_MESSAGE_PLAN: large messages and serialized messages on rmw_tickle

The user's decision of 2026-09-27 (decision 1, "B 단계적 지원으로 가자"): support messages above 64 KB in stages, and
cut copies on the way. It covers:
- stage 1, an rmw direct typesupport, together with g1, serialized messages (RMW_GAPS_PLAN.md);
- stage 2, samples above 64 KB, together with receive-buffer lending (decision 2).

This file pre-registers stage 1 (Plan, 2026-09-27, before any code). Stage 2 is pre-registered separately after stage 1's
result. Dev implements, after g3, g5 and g6.

## Why

rmw_tickle converts every ROS message to a TickLE C struct and back (`to_tickle` / `from_tickle`), and core encodes
that struct. The struct holds each sequence in a fixed-capacity C array, sized by the capacity profile
(`capacities/profile_65507.tsv`; `sensor_msgs/msg/Image` data is 64,000 B). So:

- **a field above its capacity cannot be sent at all.** A 640x480 RGB image is 921,600 B and fails with "capacity
  exceeded";
- **every sample is copied twice per direction in user space:** ROS object to struct, then struct to wire. The
  copies themselves are now memcpy-fast (a719dfaa), but they are still two;
- **every rmw publisher and subscription holds a struct as large as its capacities**, e.g. about 64 KB per Image
  endpoint, whatever the actual message size;
- **serialized messages are missing** (`rmw_publish_serialized_message`, `rmw_take_serialized_message*`), so rosbag2
  and `ros2 topic echo --raw` do not work (g1).

## Stage 1: encode and decode the ROS message directly

*(Revised 2026-09-28 after Dev's feasibility review against `87f98472`, before any code. The first draft staged the
encoded bytes in a per-publisher buffer and published them through a new core call. Under RELIABLE, which is ROS's
default, that adds a copy instead of removing one: zero-copy is barred for a reliable or durable publisher, so the
bytes would be staged twice.)*

**Codec rules.** The direct codec must produce exactly today's bytes, so it follows today's rules and not a simplified
"4-byte CDR":
1. **Alignment is relative to the enclosing struct.** A nested struct's cursor restarts at 0, and the struct nests at
   its self-alignment (`emit.py:621,664`, `model.py:135-154`). Example: `Outer{uint16; Inner{string,string}; uint32}`
   is 20 B today; absolute alignment would give 16. Strings pad to 4 after the NUL, and nested-array elements pad
   before every element except the first (`emit.py:619-633`).
2. **Strings:** the length is uint16 and counts the NUL, capped at 65,535 (`model.py:59,68`). Encoding stops at the
   first NUL, as today's `_tt_strnlen` / `c_str()` path does (`emit.py:229`, `ros2_cpp_adapter.py:65`); bounds are
   checked on `size()`. A `size()`-based encoder would break byte identity for a string with an embedded NUL.
3. **Counts are uint16** (`model.py:61`). A count above 65,535 is refused, not truncated, and sizes are computed in
   `size_t`. Messages that encode to 0 B are the case where a large count fits under the sample limit.
4. **Primitive arrays and sequences are one memcpy,** except `std::vector<bool>`, which has no `data()` and keeps an
   element loop (`ros2_cpp_adapter.py:33`).
5. **Declined types stay declined:** `wstring` (the user's decision of 2026-09-25, with no wire form) and
   `string<=N[]` (`adapt.py:146-152,165-166`).
6. **The decoder takes `is_native_endian` and byte-swaps,** as today's does (`emit.py:202,268`; `test_crossendian.py` is
   the model).

**Where it plugs in:**
- **Publish:** no new core API. rmw already wraps the codec (`encode_size_with_psn` / `encode_with_psn`,
  `rmw_publisher.c:623-639`). The direct encoder called there writes straight into `tx_buffer`
  (`tickle.c:4027-4039`), which is one user-space copy, from the ROS object to the datagram. The reliable and durable
  caches and DATA_FRAG already work from `tx_buffer`'s bytes (`tickle.c:3883, 3899ff, 3165-3173`), so their copy is
  the inherent one.
- **Take:** decode in the subscription callback, from the rx buffer into a pooled ROS message shell, and move the shell
  at take. Queueing raw bytes to decode at take time would copy them out of the reused rx buffer. The raw view
  already exists (`view_payload`, `rmw_subscription.c:105-111, 457`).
- **g1, serialized messages:**
  - `tt_Publisher_publish_serialized(pub, prefix, prefix_len, body, body_len)` in core, so that rmw's psn header
    (4 or 8 B) goes ahead of the caller's CDR without copying it;
  - `rmw_take_serialized_message*` strips the psn header and returns native-order bytes. A foreign-endian sample is
    decoded and re-encoded, because `rmw_serialize` output carries no endianness marker (`rmw_serialize.c:31-78`);
  - `rmw_serialize` / `rmw_deserialize` / `rmw_get_serialized_message_size` use the direct codec.
- **The fixed-capacity structs leave the rmw path.** Native TickLE C users keep them.
- **Unchanged:** the wire format (no version bump), and the 65,507 B per-sample limit (that is stage 2).

## Pass (stage 1)

1. **Byte identity with today's codec, in CI on every push.** For every type in the `build_ros2_interfaces.sh -a`
   inventory plus the test messages (Primitives, Array1k, Bench, Struct16), excluding declined types by name, with 200
   random messages each covering empty, single and bound-filling sizes, nested sequences of strings and of structs,
   and embedded NULs:
   - direct `encode` is byte-identical to `to_tickle` followed by the struct encode, for both C and C++;
   - direct `decode` of those bytes equals the original, comparing floats bitwise so that NaN compares;
   - **over-bound inputs** (a sequence past its bound, a count past 65,535, a string past 65,535) are refused by both
     paths alike;
   - **differential decode:** truncated and bit-flipped bytes are accepted or rejected by both decoders alike;
   - **foreign-endian decode** matches `test_crossendian.py`'s cases.
   - **Mutants, each of which must fail:** absolute instead of relative alignment; `size()` instead of first-NUL string
     length; a length prefix off by one; the bound check removed. The last one now fails, because inputs exceed the
     bound.
2. **Refusal above the per-sample limit:** an Image at 921,600 B is refused with an error naming the sample limit,
   without a crash, and without advancing the publisher's psn or seq_no (checked). An Image at 60,000 B with an Image
   capacity profile of 16 KB now passes. That proves the capacities no longer reach the rmw path, and nothing more.
3. **Interop with a native TickLE C node** (fixed structs):
   - rmw publishing within the native side's capacity is received;
   - above it, the native side refuses to decode. That is today only a log line (`tickle.c:7072-7077`), so a counter
     (`decode_failures`) is added, and the test reads it: equal to the number of oversize samples, with no crash.
4. **g1:**
   - acceptance `bag` passes on rmw_tickle with the CycloneDDS control, with a **content check** added to the test:
     the replayed strings equal the recorded ones in order, not only their count;
   - `ros2 topic echo --raw` on rmw_tickle prints **golden bytes captured from the pre-change build** for a fixed
     message. Comparing against the new `rmw_serialize` would compare the new encoder with itself.
5. **Performance** (QoS stated: RELIABLE, the ROS default, and BEST_EFFORT):
   - **Codec alone** (conv_cost): Image at 64,000 B and Array1k, direct against struct path. Predicted: the ROS-to-struct
     step disappears (about 1.6 us per direction at 64 KB on the PC, from a719dfaa).
   - **Publish-to-take:** an A/B with ±2 SE, read with WIRE_PLAN 8.3 (placement control) and the 8.3a floors. Bench and
     Array1k rmw block RTT and pong CPU must not be WORSE.
   - **Memory:** per-endpoint slope over about 100 Image endpoints, against a `std_msgs/Empty` control, or allocator
     byte counts. VmHWM is whole-process and misses untouched calloc pages, so it is not used. Predicted: the slope
     falls by about the old struct size. A publisher's footprint after one 60 KB publish is unchanged, since it no
     longer keeps a per-publisher buffer.
6. The gates, the rmw suite in netns, and every acceptance test that passed before still pass.

## Pass 1 harness: design, fixed before code (Dev, 2026-09-28)

**What is generated.** The C adapter gains three functions per message, next to `to_tickle`/`from_tickle`, with the
codec's own signatures so rmw can later call them where it calls the struct codec today:
- `<ros_name>__direct_encode_size(const ros*)`;
- `__direct_encode(const ros*, uint8_t*, uint32_t)`;
- `__direct_decode(ros*, const uint8_t*, uint32_t, bool is_native_endian)`. It decodes into an initialised message and
  allocates through `rosidl_runtime_c`, as `from_tickle` does.

The C++ adapter gains the same three over the C++ object. All of them are appended to
`rosidl_typesupport_tickle_c_message_callbacks_t`, and `struct_size` changes with it. rmw does not call them until the
harness passes.

**The limits the direct codec checks** are the IDL bound, 65,535 for a length or a count, and the buffer length. It
never checks a profile capacity.

**The harness is one program over type support handles, not generated per type.** For each type it loads the package's
`rosidl_typesupport_tickle_c` and `rosidl_typesupport_introspection_c` libraries by symbol name. In the C++ run it
loads the `_cpp` pair instead. Introspection builds the random messages, compares them, and knows the IDL bounds. The
tickle handle supplies both paths:
- **old:** `to_tickle`, then the struct encode; on decode, the struct decode, then `from_tickle`;
- **new:** the direct functions.

The seed is printed, and `-s` replays it.

**Messages, 200 per type:**
- **Length of every string and sequence:** 0, 1 or 2-4 (25% each), or a bound-filling length (25%). That is the IDL
  bound where one exists, and otherwise 300, which is above most profile capacities, so the new-only path is exercised.
- **Strings:** 10% carry an embedded NUL.
- **Floats:** a fixed share are NaN, -0.0 and inf.
- A sample above 65,507 B is regenerated with shorter lengths, and the count of regenerated samples is reported.

**Checks per message.** "Alike" means both paths accept, or both refuse.
1. **Encode.** If old accepts, new must accept with identical bytes. If old refuses, new may accept only if the
   message is within every IDL bound and 65,535 limit, which introspection checks. That is the capacity-only class; it
   is counted, and pass 1b checks it. Any other disagreement fails.
   - **1b.** A message new accepts must decode through `direct_decode` to a message equal to the original. Floats are
     compared bitwise; strings are compared up to the first NUL, which is what the wire carries.
2. **Over-bound inputs**, built on purpose per type, with at most one each: a bounded sequence at bound+1, a bounded
   string at bound+1, a count of 65,536, and a string of 65,535 characters. Old and new must refuse alike.
3. **Differential decode**, on the bytes of every accepted sample:
   - every truncation for samples up to 256 B, and 32 random ones above that;
   - 32 single-bit flips.

   Both decoders must accept or refuse alike, and on accept produce equal messages. The one allowed exception is the
   capacity-only class: old refuses with -2 and the decoded new message is within the IDL bounds. It is counted.
4. **Foreign endian:** the same byte sets are decoded with `is_native_endian = false` by both decoders, under the same
   agreement rule. The old decoder's swapping is already proven against hand-built bytes by `test_crossendian.py`, so
   agreeing with it carries that over to every type.

**Coverage floor.** Every non-declined type needs at least 100 of its 200 samples in check 1's byte-identity branch
(both accepted). A type below the floor fails, and the program lists it with its counts.

**Inventory.**
- The `build_ros2_interfaces.sh -a` workspace, via the ament index.
- The tests package, which gains messages for:
  - the rule-1 example `Outer{uint16; Inner{string,string}; uint32}`;
  - sequences of strings, of structs holding strings, and of structs holding sequences;
  - bounded strings and bounded sequences;
  - fixed arrays of strings and of structs;
  - a sequence of `bool`, which C++ stores as `std::vector<bool>`.
- `rmw_perf_pingpong`: Array1k, Bench and Struct16.

The declined types are a checked-in list. The program fails if a declined type is not on the list, or if a listed type
is supported.

**Mutants, each of which must fail with the named check.** They are selected at generation time by
`TICKLE_DIRECT_CODEC_MUTANT`:

| Mutant | Must fail |
|---|---|
| `absolute_align` | check 1 |
| `size_strlen` | check 1, on the embedded NULs |
| `len_prefix_plus1` | check 1 |
| `no_bound_check` | check 2 |

All four are run locally, and their output is recorded here. CI rebuilds only the tests package with
`len_prefix_plus1` and requires the harness to fail, as the standing control that the harness can fail.

**CI.** Check all's ROS 2 interfaces job runs the C and C++ harness over the whole inventory after
`check_ros2_interfaces.sh`.

**Order of work:**
1. C generator, then harness over the tests package, then mutants;
2. the inventory in CI;
3. C++;
4. rmw wiring.

### Pass 1 results (Dev, 2026-09-28)

**Two amendments to the sampling, both about coverage rather than about what is checked.**
1. Three samples in four aim to be byte-compared; the fourth fills unbounded fields to 300, into the capacity-only
   class. At one in two, Primitives - 13 unbounded sequences at capacity 5 - fell below the floor.
2. A sample of the first kind that the old path refuses for a capacity is regenerated shorter (lengths capped at 2,
   then 1, then 0) instead of being counted as capacity-only. Whether a type is byte-compared at all must not depend
   on how small its profile capacities happen to be: `rcl_interfaces/msg/Parameter` managed 4 of 200 without this,
   and `rosgraph_msgs/msg/Node` 1. Shrunk samples are counted and reported per type.

**The harness does not treat rosidl's `structure_needs_at_least_one_member` as data.** rosidl gives a message with no
fields that one `uint8_t` so its C struct is valid C; TickLE's generator has no such field and neither codec carries
it, so an empty message is 0 bytes on both paths. Filling and comparing it failed 18 empty types on nothing.

**Inventory** (`build_ros2_interfaces.sh -a`, 24 packages, plus the tests packages): **283 types, 0 failures**, 1
declined (`example_interfaces/msg/WString`, a wstring), seed 1, 1 m 45 s. Byte-compared samples per type: minimum
150, median 200. Across the run: 1,441 capacity-only samples, 3,178 shrunk, 139 regenerated for size, 244 over-bound
inputs, and 7,392,852 decode inputs, of which 39,730 were capacity-only.

The declined list is `rosidl_typesupport_tickle_c_tests/declined_types.txt`. Its own control: a list naming a
supported type and omitting WString fails with both messages.

**Mutants.** Each fails the check it was assigned, and only that one.

| Mutant | Result |
|---|---|
| `absolute_align` | 10 findings, all check 1 (DcOuter 0 of 200 identical). |
| `size_strlen` | 20 findings, all check 1, on the embedded NULs. |
| `len_prefix_plus1` | 17 check 1, 3 check 1b. |
| `no_bound_check` | 4 check 2, 5 check 3. |

**CI:** Check all runs the harness over the whole inventory after `check_ros2_interfaces.sh`, then rebuilds only the
tests package with `len_prefix_plus1` and requires that to fail on check 1 - the standing control that the harness
can fail.

### Pass 1, the C++ codec (Dev, 2026-09-28)

`ros2_cpp_direct_codec.py` generates `direct_encode_size`/`direct_encode`/`direct_decode` over the C++ message,
and the C++ callbacks now carry them. Everything that decides the layout - padding, field order, a string's
uint16 length including its NUL, a sequence's uint16 count, the bound checks, the byte swap - is *imported* from
the C generator rather than written again: those helpers emit plain C statements, which are valid C++. Only the
field accessors differ (`ros.f`, `.size()`, `&ros.f[0]`, `resize()`, `assign()`). Sharing them is what answers
this stage's first stated risk, two encoders of one wire format drifting apart. `std::vector<bool>` keeps an
element loop, having no `data()`; `BoundedVector` has no const `data()` either, so every container is reached
through `&ros.f[0]`, only where there is an element.

**How it is checked.** The harness gained a C++ arm. A C++ message cannot be filled by the C introspection, so
each sample is carried across: the C message the checks already built goes through the C `to_tickle` into the
TickLE struct and the C++ `from_tickle` out of it, and that object's direct encoding must be the very bytes the
C one produced. Over the inventory at seed 7: **287 types, 0 failures, 55,922 samples byte-compared between the
two languages**, and the per-type hashes are unchanged from the pre-g12 baseline.

**What that arm cannot reach, and what covers it.** A sample carried through a TickLE struct ends at the first
NUL, so a `std::string` with a NUL inside it never reaches the C++ codec that way. `test_direct_codec_cpp.cpp`
asserts those rules against bytes written out in full instead: a string ending at its first NUL, a bounded
string refused above its bound, and a `std::vector<bool>` round trip. CI runs it beside the harness.

**Two C++-only mutants**, because the shared helpers mean the original four move both encoders together and the
two still agree:

| Mutant | Must fail |
|---|---|
| `cpp_string_size` | `test_direct_codec_cpp` (the harness passes it - that is the gap, measured) |
| `cpp_skip_pad` | the harness's check 1c, and only that check |

Both were run: `cpp_string_size` leaves the harness green and fails the test on the length prefix;
`cpp_skip_pad` fails check 1c with "C++ encodes 22 B where C encodes 24" and nothing else.

### Stage 1 wired into rmw (Dev, 2026-09-28)

**Publish.** `encode_with_psn`/`encode_size_with_psn` call the direct encoder on the ROS message itself, so the
bytes go straight into core's `tx_buffer` behind the psn header. **A publisher of a type with a direct codec now
allocates no message-sized buffer at all** - `publish_scratch_buf` is only taken for the fallback below, and
`test_shell_pool` asserts it is NULL. That is the revision's own claim, and pass 5's per-endpoint slope is where a
buffer creeping back would show.

**Take.** The payload decodes straight into a pooled ROS shell; `decode_scratch` is likewise only allocated for
the fallback, and asserted NULL. The shell is taken *before* the decode now, and returned to the pool - zeroed on
the way - whenever the decode refuses, so a half-written shell is never handed out.

**The fallback stays,** for a callbacks struct with no direct codec: rmw_tickle's own hand-written test ones, and
any interface package built before this. It is the old two-step, and `test_publish_take_reuse` exercises it.

**The pooled shells' limit, stated and tested** (the risk list, and Plan's ask):
- **How many.** The pool is sized `queue_capacity`, the subscription's depth - the most shells that can be in
  flight at once. `shell_pool_push` destroys rather than pools anything beyond that, so the count can never
  exceed it. `test_shell_pool` asserts the bound after a burst deeper than the queue.
- **When the queue is full.** KEEP_LAST drops the oldest back into the pool rather than freeing it: after depth+2
  deliveries exactly `depth` are queued, and the ones kept are the newest. Asserted by content, not only by count.
- **Reset, not reused dirty.** A shell handed back out carries nothing of the sample before it, because the
  decoder writes every field. `test_shell_pool` shows it directly - a long sample, then a short one into the same
  shell - and the generator's own **`keep_shell_tail`** mutant is the proof at the level where the reset lives: it
  reuses an allocation that is merely big enough and leaves the old count, and the pass-1 harness's **check 1b**
  fails on it, only that check. Its control on the rmw side: dropping the shell on a failed decode instead of
  returning it fails `test_shell_pool` at the line that counts the pool.

**Acceptance, on the wired build:** `inprocess`, `durable`, `samehost`, `graph`, `events`, `matched`, `takeseq`,
`range` and `peers` all pass, each with its CycloneDDS control passing. All 31 rmw unit programs pass in a netns.

**A generator change did not regenerate anything.** Found while building this: a generated file's only `DEPENDS` were
its `.msg` and the capacity table, so `rosidl_typesupport_tickle_c_generate_interfaces.cmake` left every already
generated file in place when the generator itself changed. The direct codec's first build failed on it - a freshly
generated caller called a function the dependency's cached header never declared. The generator's own sources
(`rosidl_typesupport_tickle_c` and `tickle_typesupport`, located through Python rather than guessed from a path) are
now `DEPENDS` too, and the configure fails if that list comes back empty. Control: touching `ros2_direct_codec.py`
regenerates a dependent package's adapter, checked by mtime.

## Risks stated in advance

- **Two encoders for one wire format** (the struct codec for native C, the direct one for ROS) can drift. Pass 1 runs
  in CI on every push, not once.
- **Keyed and nested sequences of strings** are where generators usually slip. The random-message generator covers
  them at empty, single and bound-filling sizes, with embedded NULs.
- **Pooled ROS shells at take** hold one message per queued sample. The queue depth bounds that memory; it is
  reported in pass 5.

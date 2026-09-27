# TickLE's structure fitted to rmw's: context, node, and nodes in discovery

Pre-registered 2026-09-27, before any code.

**The decision.** The user said "일단 TickLE의 구조를 rmw에 맞추어서 재구성하는 것을 진행하자". This approves Plan's proposal
to restructure the core around rmw's model, and replaces the "node entity" plan for defect C (WIRE_PLAN 9.3 follow-up,
COMPARISON 4.4): remote rmw_tickle nodes are invisible to `ros2 node list/info` and `ros2 param`.

**Why the structure, and not only a wire field.**
- In rmw, a *context* owns the transport: one participant, one set of sockets, one discovery.
- A context hosts any number of *nodes*: named, with a namespace, owning endpoints.
- TickLE's `struct tt_Node` is the context: sockets, scheduler, poll, liveliness, discovery. It has no concept of the
  node rmw means. rmw_tickle keeps one `tt_Node` per context and tracks ROS nodes only in its own registry, which is
  why they never reach the wire.
- Naming the core's pieces after rmw's lets a node be a real core object, carried on the wire as what it is.

Three stages, each landed and judged before the next starts.

## Stage 1 - `tt_Node` becomes `tt_Context` (mechanical, no behaviour change)

**Scope, counted on `main` at `af54d45e`.** Tracked files, excluding `results/` and the typesupport golden files:

| area | files | occurrences of `tt_Node` |
|---|---:|---:|
| tests/ | 33 | 641 |
| src/ | 3 | 359 |
| rmw_tickle/rmw_tickle/ | 22 | 240 |
| examples/perf_hil/ | 28 | 239 |
| include/ | 3 | 138 |
| examples/linux/, examples/freertos/ | 17 | 151 |
| docs (README, DESIGN, CHANGELOG, plans) | 12 | ~160 |
| **total** | **122** | **2,070** |

- `struct tt_Node` accounts for 897 of these.
- The public API has **16 symbols**. The most used are `tt_Node_poll` (233), `tt_Node_schedule` (158),
  `tt_Node_create_publisher` (94), `tt_Node_lock`/`_unlock`, `tt_Node_destroy`, `tt_Node_create_subscriber`,
  `tt_Node_set_discovery`, `tt_Node_interrupt`, `tt_Node_entity_alive`, `tt_Node_unschedule`, `tt_Node_create`,
  `tt_Node_create_server`/`_client`, `tt_Node_lock_timed` and `tt_Node_next_due`.

**What is renamed:**
- `struct tt_Node` -> `struct tt_Context`;
- every `tt_Node_*` function -> `tt_Context_*`;
- the same in callback typedefs, comments and active docs (README, DESIGN, a CHANGELOG entry).

**Plan's answers (same day), which supersede point 1 below where they differ:**
- **Public config macros users set with `-D`** (`tt_NODE_*` in `config.h`) are renamed in stage 1 to `tt_CONTEXT_*`
  where they are context-level.
  - Each old name gets a guard: `#ifdef tt_NODE_X` -> `#error "tt_NODE_X is now tt_CONTEXT_X"`.
  - Without it, an old `-Dtt_NODE_X` would be silently ignored behind the `#ifndef` defaults, the worst way a rename
    can fail.
  - A test compiles `config.h` with an old name defined and must fail.
- **Public struct fields** in `include/` that call the context's id `node_id` are renamed in stage 1 only when they
  are in a public header. Wire field names in docs read "context id (formerly node id)".
- **Local variables named `node`, and internal helpers,** are left for a later tidy-up.

**What is not renamed, and why (as first written):**
1. **`tt_NODE_*` macros** (`tt_NODE_ID_INVALID`, `tt_NODE_ID_BROADCAST`, `tt_NODE_CYCLE`, `tt_NODE_UPDATE_INTERVAL`,
   `tt_NODE_TX_INTERVAL`, `tt_NODE_PORT`, `tt_NODE_ADDRESS`, `tt_NODE_MAX_LEASE_NS`; 261 occurrences), **the wire's
   `source`/`node_id`**, and local variables named `node`.
   - "Node id" on the wire is the participant's id, a context property. Renaming it is right eventually, but it touches
     the wire vocabulary and every peer table.
   - It is kept out of stage 1 so this stage stays purely mechanical. **Plan to decide:** rename them in stage 1 as
     well (+~450 edits), or leave them to a later tidy-up.
2. **rmw_tickle's `tickle_node` field** (191 occurrences) becomes `tickle_context`. It *is* the context, and a later
   reader must not take it for a node.
3. **Dated records**: the `*_PLAN.md` result sections and `results/`. They describe the code as it was when written,
   and are left as written. A one-line note at the top of each plan file points at this rename.

**No alias.** `tt_Node` is not kept as a `typedef` or `#define`. Stage 2 gives `tt_Node` a new meaning, so code written
against the old one must fail to compile, not silently compile against the new.

**Stage 1 PASS:**
- **Identical code.** `tickle.o`, `hal_linux.o` and `encoding.o`, built -O2 before and after, have the same
  disassembly once symbol names are normalised (`objdump -d` with every `tt_Node`/`tt_Context` token mapped to one
  name). A difference is a finding, since a rename must change no instruction.
- **The same tests:**
  - `make check-gates` 9/9;
  - the rmw suite in a netns (46 tests);
  - the typesupport pytest;
  - the FreeRTOS HAL lint;
  - the examples build.
- **No `tt_Node` token left** in code, headers, examples or tests (a grep gate, kept for stage 1 only). The only
  exceptions are the dated records above.
- **The bench pair**, `core_cost_ab.sh` default and `-R`, 20 rounds: send and recv within 2 x SE. It is expected held
  by the identical-code check, and is run because Plan asked for it.
- **Coordination.** The work is done in Dev's own worktree, and Plan is told before the push: Plan's harnesses and
  results reference `tt_Node` in comments.

### Stage 1 result: PASS (2026-09-27, 25ac7fe0)

- **Identical code.** The -O2 `tickle.o`, `hal_linux.o`, `encoding.o` and `log.o`, built before and after, give the
  same `objdump -dr` once `tt_Node`/`tt_Context` and `tt_NODE_`/`tt_CONTEXT_` are mapped to one name
  (15,406 / 1,522 / 254 / 337 lines). Control: unnormalised, `tickle.o` differs in 404 lines, all names.
- **The same tests:** `make check-gates` 10/10; the rmw suite in a private netns, 46/46; the typesupport pytest and
  the FreeRTOS HAL lint (both gates); `headers-cpp`; the Linux examples; all nine perf_hil TickLE scenarios build.
- **No old token left:** no `tt_Node`, `tickle_node` or `tt_NODE_` outside the dated records, except the nine
  `#error` guards in `config.h` and the `config-renames` check that names them.
- **Old `-D` names stop the build.** `make -C platform/linux config-renames` (run by `make test`) compiles
  `config.h` with each of the nine old names set and requires the rename's `#error`; a control compile with none
  set must pass. A mutant with the `tt_NODE_TX_INTERVAL` guard removed fails it.
- **The bench pair**, `core_cost_ab.sh -r 20`, 47ddcb75 against 25ac7fe0, mean difference against 2 x SE:

  | run | send (ns) | recv (ns) |
  |---|---|---|
  | default | 185.6 -> 184.7, -0.9 (2 x SE 3.1) | 52.7 -> 52.7, +0.0 (1.0) |
  | `-R` | 240.6 -> 239.7, -0.9 (3.8) | 78.6 -> 78.1, -0.6 (0.8) |

  All four held. The bench itself had to be renamed, so each arm compiled its own tree's copy
  (`BENCH_FROM_REF=1`, new); the two copies are identical once the renamed names are mapped to one.
  Raw: `examples/perf_hil/results/core_cost_ab_context_rename{,_R}_2026-09-27.txt`.
- **As scoped:** local variables named `node`, internal helpers and `tt_get_node_id()` keep their names (Plan's
  answer); `rmw_tickle_node_t` is rmw's node and keeps its name. `.git-blame-ignore-revs` lists the rename.

## Stage 2 - a lightweight core `tt_Node` (no wire change)

**The model:**
- A `tt_Context` owns sockets, scheduler, poll, liveliness and discovery, as today.
- A `tt_Node` is a name, a namespace and an index within its context, and owns endpoints.
- Every endpoint belongs to exactly one node.
- A context creates a default node at `tt_Context_create()`, so the simple path stays one call longer at most.
  Plan's rules for it (same day), each with a test:
  - **Its name is unique per context:** `tickle_<context id>` in "/". A shared "/tickle" would appear once per
    process in `ros2 node list`, and ROS warns on duplicate names.
  - **A node that owns no endpoints and was not created explicitly is never announced** (stage 3).
  - **rmw_tickle's contexts create no default node at all**: rmw always creates its nodes explicitly, and a default
    one would be a phantom in every process's graph.

**API sketch:**
```c
struct tt_Node {
    struct tt_Context* context;
    const char* name;       // not copied - the caller's, as an endpoint's name is today
    const char* namespace_;
    uint8_t index;          // 0 = the context's default node; < tt_MAX_NODES (16), carried on the wire in stage 3
};

tt_ret_t tt_Context_create(struct tt_Context* context);              // also sets up the default node
struct tt_Node* tt_Context_default_node(struct tt_Context* context);
tt_ret_t tt_Node_create(struct tt_Context* context, struct tt_Node* node, const char* name, const char* namespace_);
tt_ret_t tt_Node_destroy(struct tt_Node* node);                      // tt_RET_ILLEGAL_STATUS while it owns endpoints

tt_ret_t tt_Node_create_publisher(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Topic* topic,
                                  const char* endpoint_name);        // and _subscriber, _client, _server
// Shorthand, unchanged in shape: stage 1's tt_Context_create_publisher(context, ...) creates on the default node.
```
- `struct tt_Endpoint` gains `struct tt_Node* node`.
- rmw_tickle's `rmw_create_node()` creates a core `tt_Node`, and its node registry keeps a pointer to it, instead of
  being the only record of the node.

**Stage 2 PASS:**
- **Behaviour:**
  - endpoints on two nodes of one context each report their own node;
  - a node with endpoints refuses destroy;
  - the default node carries every endpoint created through the shorthand;
  - rmw_tickle's nodes are core nodes, and an rmw context has no default node;
  - the default node's name differs between two contexts.
  - Each is a test with a mutant it fails against.
- **Nothing observable moves:** the announce bytes are byte-identical to stage 1's for the same endpoints, since there
  is no wire change. The rmw suite, the gates and the typesupport pytest all pass.
- **CPU:** the `core_cost_ab.sh` pair (default and `-R`) is held within 2 x SE. An endpoint holding one more pointer
  is the only change on the hot path's data.

### Stage 2 result: PASS, with two amendments to the sketch (2026-09-27, 5d9cace1)

**Amendments, each for a pre-registered criterion:**
- **An index, not a pointer, on the endpoint.** `tt_Endpoint.node_index` sits in the padding after `kind`, and
  `tt_Endpoint_node()` finds the node.
  - The sketched `struct tt_Node* node` grew every endpoint struct by 8 bytes. It cost `-R`'s receive +1.0 and +1.1 ns
    in two runs (2 x SE 0.7 and 0.4), against the CPU criterion.
  - With the index, `tt_Endpoint`, `tt_Publisher` and `tt_Subscriber` keep their size (24, 528, 1216) and layout.
  - It is also what stage 3 carries on the wire.
- **The default node comes into being lazily**, on the first `tt_Context_create_*()` shorthand call or on
  `tt_Context_default_node()`, not at `tt_Context_create()`.
  - A context whose endpoints are all on its own nodes, as rmw_tickle's are, then has none by construction, with no
    flag.
  - All three of Plan's rules hold: a unique name, never announced when empty, none in rmw.

**Behaviour - each test fails against a mutant:**

| test | mutant | failing checks |
|---|---|---|
| endpoints on two nodes report their own | owner ignored | 8 |
| destroy refused while an endpoint lives | no check | 3 |
| the shorthands use the default node | no default | 3 |
| default names differ between contexts | a fixed name | 2 |
| explicit nodes leave no default node | created eagerly | 1 |

- rmw's nodes are core nodes: `test_multi_node` checks their names, indices and that there is no default node.
- rmw's endpoints live on them: `test_graph`, `test_publish_take_reuse` and `test_service_roundtrip`. The rmw
  publisher and service put back on the shorthand fail the last two.

**Nothing observable moves:**
- `experiments/announce_bytes.c`: the announce and the summary are byte-identical to stage 1 (2bfef75e) for the same
  four endpoints, created through the shorthand and on an explicit node. The control, one endpoint renamed, differs
  by 4 bytes.
- Gates 10/10; the rmw suite 46/46 in a netns.

**CPU:**
- -O2 `tickle.o` against stage 1: only the creation paths changed instructions. Three functions differ in alignment
  NOPs alone. Every receive and publish function is identical.
- `core_cost_ab.sh -r 20`, 2bfef75e against e00e3e72 (the same tree as 5d9cace1):

  | run | send (ns) | recv (ns) |
  |---|---|---|
  | default | -0.9 (2 x SE 2.4), held | -0.7 (0.5) |
  | `-R` | -2.0 (3.4), held | -0.3 (0.3) |

- Receive is a fraction of a ns faster, just outside 2 x SE. That is code placement, given the identical instructions.
  An A/A pair of one binary put receive outside 2 x SE in 1 of 12 comparisons (+0.8, 2 x SE 0.61).
- Raw: `examples/perf_hil/results/core_cost_ab_stage2{,_R}_2026-09-27.txt` and `core_cost_ab_aa_2026-09-27.txt`.

## Stage 3 - discovery carries nodes (wire v11)

**The wire diff (amended 2026-09-27, before code, for Plan's 64-node minimum - see "Stage 3 encoding" below):**
- **A node entry in the announce list.** `tt_UpdateEntity` with:
  - `kind = tt_KIND_NODE` (0x03: TOPIC and SERVICE together, which no endpoint is);
  - `entity_id` = the node's own random id;
  - `endpoint_id` = hash(namespace, name);
  - type string = namespace, name string = node name;
  - the node's index in the spare bits (below).
  - One per node, in the same list as its endpoints.
- **Every endpoint entry** carries its owning node's index in the same spare bits. 0 bytes.
- `tt_VERSION` goes 10 -> 11.
- **The receiver:**
  - stores node entries in the discovery table like any entity;
  - forgets or tombstones them with their source;
  - never matches them as endpoints: `count_matching`, RxO, peers and the W1-era directory all skip
    `tt_KIND_NODE`, each pinned by a test.
- **An empty default node is not announced.** Only a node created explicitly, or a default node that owns at least
  one endpoint, gets an entry. This is pinned by a test on the announce bytes.
- **rmw:**
  - `rmw_get_node_names*()` returns local nodes plus alive remote node entries.
  - `rmw_get_{publisher,subscriber,service,client}_names_and_types_by_node()` match remote endpoints by
    (source, node index).

**Stage 3 encoding (pre-registered 2026-09-27, before code).** Plan asked for room for at least 64 nodes, preferably
256, at 0 bytes. A composed Nav2 bringup puts 15-20 nodes in one container, so the sketch's 4 bits (16) would fail a
standard deployment.

- **Chosen: 8 spare bits in `kind` and `qos`, for a limit of 256.**
  - `kind` uses 0x01/0x02 (topic/service) and 0x10/0x20 (sender/receiver). Its free bits 0x04, 0x08, 0x40 and 0x80
    carry index bits 4-7.
  - `qos` uses bits 0-3 (RELIABLE, DURABLE, LIVELINESS_MANUAL, KEEP_ALL). Its free bits 4-7 carry index bits 0-3.
  - The receiver masks `kind` with 0x33 and `qos` with 0x0f before any existing use, so every current comparison sees
    what it sees today.
  - 256 is the whole `uint8_t` index, the core's own limit (`tt_MAX_NODES` <= 256), so the wire does not bound it
    further.
- **Rejected: the endpoint's `entity_id` upper bits.**
  - Entity ids are not small per-context numbers: each is a random per-launch 32-bit base plus a counter
    (Milestone 47).
  - Receivers key writer proxies, durable records and tombstones on them, and rmw builds GIDs from them.
  - Taking 8 bits would cut the launch randomness to 24 bits and change the identity of every entity. Too wide a
    change for a 0-byte gain the spare bits already give.
- **The test at the limit.** Two contexts in two private netns:
  - one hosting 256 nodes (`tt_MAX_NODES` 256), each owning one endpoint, publishers and subscribers alternating.
    256 endpoints is `tt_MAX_ENDPOINT_COUNT`'s own ceiling (an endpoint slot is a `uint8_t`), so one each is the
    most a context can hold. The announce, 512 entries, goes out fragmented.
  - the other, built with `tt_MAX_DISCOVERED_ENTITIES` of at least 512, must list all 256 nodes by name and
    namespace, each with its own endpoint and no other's.
  - A mutant that drops the `kind` half (indices mod 16) must fail it.

**Stage 3 PASS, in the user's rule's terms:**
- **Function, two processes in two private netns (`ros2 node list`, `ros2 node info /talker`, `ros2 param list
  /talker`):**
  - they list the remote node and its endpoints, as CycloneDDS does on the same pair;
  - the rmw suite passes;
  - `test_rmw_implementation`'s graph tests pass.
- **Recorded costs, pre-registered as the price of the feature:**
  - **M1 join bytes:** +41 B per node per list (the node entry; endpoint entries carry the index at 0 B) for a node named "talker" in "/"
    (28 + (2 + 1 + 1) + (2 + 6 + 1) = 41).
  - A default rclcpp node's list, 998 B, becomes ~1,039 (+4%). The M1 tool, which has one node per context, adds one
    entry per list.
- **Must not be WORSE:**
  - **M2 steady state:** byte-identical, since summaries do not carry the list.
  - **M3 join time:** within +5 ms, DISCOVERY_PLAN's bar.
  - **Core CPU:** `core_cost_bench -D`, 20 rounds, held within 2 x SE.
  - **The rig:** the campaign cells A B B A, read by WIRE_PLAN 8.3. A pooled WORSE is only a candidate; confirmed
    means the same sign in every cell of its kind.
- The user's wire rule ("every test better") is met by Plan's reading: the targeted metric is function, and
  none regresses except the recorded M1 cost, which the user approved as the feature's price.

## Roadmap after stage 1 (user decisions relayed by Plan, 2026-09-27)

The user decided three open items: "1. rmw_tickle에서 64KB를 넘는 메시지: B 단계적 지원으로 가자 / 2. Native API에
수신 버퍼 대여 추가: rmw가 수신 버퍼를 지원하는 것이 맞으면 TickLE도 그대로 가자 / 3. 쓰지 않게 된 서비스 응답 대기
버퍼: 제거하자". The order, each step pre-registered before code:

1. **Stage 1** (above, done).
2. **Remove the unused pending-response storage** (user item 3): `tt_Server.pending_response_buf`,
   `set_storage()`'s pending half and rmw_service's allocation of it. A separate commit right after stage 1, so both
   API breaks ship together.
2a. **The client's adaptive retry learns only from success** (found 2026-09-27 while verifying item 2; Plan: fix
   before stage 2). Pre-registered below, under "Client retry fix".
3. **Release-built tests that test nothing** (Plan, same day): `rosidl_typesupport_tickle_c_tests`' dispatch tests
   check with `assert()`, which a Release build compiles out. Fix with `-UNDEBUG` on the test targets or explicit
   checks, with a mutant showing the test fails in the Release build.
   **Done 2026-09-27:** `-UNDEBUG` on all six test targets, as rmw_tickle's own tests already had.
   - Release, before: none of the six binaries referenced `__assert_fail`.
   - Release, after: the five that use `assert()` reference it, and all six pass. `test_primitives_cpp` already
     checked explicitly.
   - Mutant (`test_dispatch` expecting 11 instead of 10): passes in Release without the fix, and aborts (rc 134)
     with it.
4. **Stage 2** (done), a follow-up lifting the node limit (done: 256 per rmw context), then **stage 3** (wire v11).
   - **Found while sizing it:** the per-context endpoint ceiling, `tt_MAX_ENDPOINT_COUNT` = 256, is a `uint8_t` slot
     index, and it binds before the node limit does. Every rclcpp node brings about 9 endpoints by default: 6
     parameter services, rosout, and parameter_events publisher and subscriber. So a 20-node container holds about
     180 before any of its own topics. Put to Plan, 2026-09-27; not pre-registered yet.
4a. **Endpoint capacity, before stage 3** (Plan, 2026-09-27: an internal capacity, so no user decision; pre-registered
   here before code).
   - **What binds today, found by reading every use of `tt_MAX_ENDPOINT_COUNT`.**
     - No endpoint slot is indexed by a `uint8_t` anywhere: `endpoint_count` is a `uint32_t`, and
       `endpoints[]` / `endpoint_index[]` hold pointers.
     - The coupling is the per-peer tables in `struct tt_Context`: `update_generation`, `update_seen`, the
       `update_part_*` arrays, `version_mismatch_logged`, `update_last_seen`, `traffic_last_seen`,
       `liveliness_flags` and `reached_nodes`. These are indexed by a remote **context id** (`header->source`, a
       `uint8_t` on the wire) but sized by `tt_MAX_ENDPOINT_COUNT`, which happens to be 256. That is why
       `config.h` asserts `tt_MAX_ENDPOINT_COUNT <= 256`.
   - **The change.**
     - A new, fixed `tt_MAX_CONTEXT_IDS` (`UINT8_MAX + 1`, the wire's id space) sizes those tables and their loops.
     - `tt_MAX_ENDPOINT_COUNT` then bounds only the local endpoint table, and the 256 assert goes.
     - The core default stays 256.
     - rmw_tickle's CMake sets `tt_MAX_ENDPOINT_COUNT` 2048 and `tt_ENDPOINT_INDEX_SIZE` 4096. The count, from a
       composed Nav2 bringup: about 20 nodes. Each rclcpp node has 9 endpoints by default (6 parameter services,
       rosout, parameter_events publisher and subscriber); a lifecycle node adds 5 services and a transition-event
       publisher; each action server adds 3 services and 2 topics; plus the application's topics. That is about 30
       per node, so about 600, and 2048 is more than 3x that.
   - **Found alongside, the same class of limit:** rmw never set `tt_MAX_DISCOVERED_ENTITIES`, so its table of remote
     entities, which rmw's whole graph API reads, holds 16 and silently drops the rest. `ros2 topic list` against a
     Nav2 process sees 16 of its hundreds of endpoints, and stage 3's remote node list would hit the same wall.
     - rmw sets it to 2048 too. A `tt_DiscoveredEntity` is 552 B, so that is about 1.1 MB per rmw context, on the
       heap.
     - `upsert_discovered_entity()` is a linear scan, so its cost at that size is measured, not assumed.
   - **PASS.**
     - **Core default build:**
       - the -O2 instructions of every receive and publish function are identical to before, or any difference is
         explained;
       - the `core_cost_ab` pair (default and `-R`) is held within 2 x SE, read against an A/A pair;
       - the announce bytes are identical (`experiments/announce_bytes.c`).
     - **The rmw limit:**
       - a context filled to 2048 endpoints (for example 64 nodes x 32), all found locally;
       - a remote context sees the other's entities up to its discovered capacity, not 16;
       - a mutant that sizes the per-peer tables from a small endpoint count again, or truncates an index to 8 bits,
         fails.
     - **Memory:** `sizeof(struct tt_Context)` and `sizeof(struct tt_Discovery)` at the core and rmw settings,
       before and after.
     - **Cost at the rmw setting:** the time to process a 600-entry announce into a 2048-entry discovered table,
       against 16.
     - Gates 10/10, and the rmw suite in a netns.
5. **rmw gaps**. The user's words, relayed by Plan: "1, 2, 3, 4번 진행하자." Each item is pre-registered before code,
   with behaviour tests and mutants, and uses CycloneDDS's or Fast DDS's behaviour as the control where one exists:
   - **(g1) Serialized messages** (`rmw_publish_serialized_message`, `rmw_take_serialized_message*`,
     `rmw_get_serialized_message_size`), for rosbag2 and `ros2 topic echo --raw`. Control: rosbag2 record then
     play of a talker, on rmw_tickle against CycloneDDS, with the messages byte-for-byte equal.
   - **(g2) On-new-data callbacks** (`rmw_subscription/service/client_set_on_new_*_callback`,
     `rmw_event_set_callback`), for rclcpp's EventsExecutor.
   - **(g3) The remaining QoS event kinds** in `rmw_publisher/subscription_event_init`, and
     `rmw_get_clients/servers_info_by_service`.
   - **(g4)** The check pass on `ROS_AUTOMATIC_DISCOVERY_RANGE` / `ROS_STATIC_PEERS`, SROS2 and actions. Plan does it,
     with no Dev code.
   - **Order for Dev:** stage 3, then (g2), then (g3), then large messages stage 1 together with (g1) (both are ROS
     message to TickLE wire bytes, and a serialized message is exactly that output), then large messages stage 2
     with lending.
   - **(g5) `rmw_take_sequence`** is not defined in `librmw_tickle.so` at all (`nm -D`), though rmw_implementation
     dispatches it on jazzy (Plan, 2026-09-27). COMPARISON 2.7a's row is updated in the commit that closes it.
6. **Large messages, stage 1** (user item 1, "B"): rmw_tickle's direct typesupport, ROS C++ to the TickLE wire with
   no fixed-capacity intermediate struct.
7. **Large messages, stage 2, with receive-buffer lending** (user items 1 and 2):
   - samples over 64 KB: a wider fragment index and count behind a flag, so existing cells do not grow; a 32-bit
     record length; reassembly buffers sized from `FRAG_FIRST`'s total length; dynamic allocation in rmw builds
     only (the core stays static).
   - lending: retain/release in the native API (OPTIMIZATION_PLAN 12.3), designed so that
     `rmw_take_loaned_message` / `rmw_return_loaned_message_from_subscription` (gated by `can_loan_messages`) sit
     directly on it, and covering reassembled large samples, where it pays.
   - If its wire change is ready close to stage 3, one version bump may carry both; otherwise v11 and v12.

## Client retry fix (pre-registered 2026-09-27, before code)

**The defect.** With `call_retry_interval` 0 ("auto", what core's examples and generated services pass), a call is
retried every `1.5 x client->latency` and given up after `tt_CALL_RETRY_COUNT` (3) retries, so its whole budget is
about `4 x 1.5 x latency`. `latency` is an EMA updated only when an answer is accepted. So:
- It has no floor: one fast answer shrinks the budget to match it. Measured in `test_thread_safety`: call 1 answered
  in 85 us, so the interval became 128 us and the budget about 0.5 ms.
- It learns nothing from a timeout: a later answer slower than the budget times the call out, the EMA does not
  move, and every call after it times out the same way, forever. The same run: 600+ consecutive timeouts on call 2,
  `latency` frozen at 85,358 ns.
- `test_thread_safety` hits it in about 2-3% of plain runs (5/140 at 0f877d54; 0/140 at 47ddcb75, the rate moving
  with timing): as `answers_wrong` before 579dfcaf, as a hang since (the caller re-asks a timed-out call).
- rmw_tickle is not affected: `rmw_client.c` sets `call_retry_interval` explicitly. Native users on the default are.

**The fix, only on the auto path** (an explicit `call_retry_interval` is used as given, as today):
- **Floor:** the interval is never below `tt_CALL_RETRY_INTERVAL` (5 ms). A lost request on a fast link is then
  retried after 5 ms rather than after 1.5 x its latency; only core's RPC examples use the auto path (the perf_hil
  harnesses use no RPC, rmw sets its own interval).
- **Backoff on timeout:** when a call times out, the estimate doubles (from `tt_CALL_RETRY_INTERVAL` if it was 0),
  so the next call's budget grows - Karn's / TCP RTO backoff.
- **Cap:** a new `tt_CALL_RETRY_INTERVAL_MAX` (default 250 ms) bounds the interval, so the estimate never exceeds
  it, and a call on the auto path ends within `(tt_CALL_RETRY_COUNT + 1) x tt_CALL_RETRY_INTERVAL_MAX` (1 s by
  default) however often it has timed out: a dead server cannot grow the budget without bound. A server that
  needs longer than that needs an explicit `call_retry_interval`.
- **Recovery:** the first answer accepted after a backoff replaces the estimate with its own latency instead of
  moving the EMA by 1/8, so a server that is fast again gets its short budget back on the next call, not ~30 calls
  later. Later answers move the EMA as today.
- The first call's interval and every retry's come from one function, which today are two copies of the same
  arithmetic.

**PASS - unit tests with the mock clock and a simulated server** (answer after a given delay, the client's
`call_retry` timers run at their scheduled times):
- **Fast then slow:** calls answered in 100 us, then calls answered in 3 ms. Today every slow call times out; fixed,
  every one is answered (the floor alone gives a 20 ms budget).
- **Slow beyond the floor:** calls answered in 40 ms. Fixed: the first few may time out, the backoff grows the
  budget, and from then on every call is answered.
- **Recovery:** fast, then 40 ms (backoff), then fast again: the first fast call after the slow phase brings the
  interval back to the floor.
- **Dead server:** 20 calls, none answered. Every call ends in `tt_CALL_TIMEOUT` within
  `(tt_CALL_RETRY_COUNT + 1) x tt_CALL_RETRY_INTERVAL_MAX`, and the interval stops at the cap.
- **Explicit interval unchanged:** with `call_retry_interval` set, retries are at exactly that interval and a
  timeout changes nothing.
- **Mutants, each failing at least one test:** no backoff; no cap; no floor; no reset on recovery.
- **`test_thread_safety` stays on the auto path** (not pinned to an interval, which would hide the only test of
  the default path): 200 plain + 40 tsan runs before and after. Before shows the 2-3%; after must show 0.
- `make check-gates`, the rmw suite in a netns, and a CHANGELOG entry saying native users on the default interval
  were affected and rmw was not.

### Client retry fix: result (2026-09-27, 803db9d1 and a51b8d2f)

**The retry fix (803db9d1): PASS on its own tests, but it was not the thread test's cause.**
- `test_call_retry_adaptive` passes. The old code fails 7 of its checks, and each mutant fails at least one:
  - no backoff: 5;
  - no cap: 3;
  - no floor: 3;
  - no reset: 2.
- **Amended:** the backoff doubles from max(estimate, floor), not from the estimate. Doubling a 100 us estimate
  would stay under the floor for about 6 timeouts.
- With it alone, `test_thread_safety` still failed 3/200, and 11/600 in a longer count.

**The actual cause, defect F (a51b8d2f): padding on the wire was never written.**
- `end_encode()` pads each submessage to 4 bytes and counts the padding in its length. The padding was never
  written, so it held stale `tx_buffer` bytes.
- The thread test's response codec requires its string terminator last. A call whose padding held a non-zero byte
  failed to decode on every retry, because the cached encoding is resent unchanged, and the call was never
  completed.
- An instrumented hang showed it: the backoff grew to the cap as designed, the answers reached the client, and each
  was rejected with "Cannot decode response".
- My first reading, that the retry defect caused the flake, was inferred from one run without a control. It was
  wrong.
- **Confidentiality:** up to 3 bytes per submessage of an earlier datagram, possibly addressed to another peer, were
  sent.
- **Generated codecs are not affected:** core's and rmw_tickle's decoders read strings by their length prefix and
  never read the tail. `test_encode_padding` decodes a generated message, ending in a bool with 3 bytes of padding
  and ending in a string, with the padding stale and zeroed.
- `test_encode_padding` fails 12 checks with the zeroing removed.

**`test_thread_safety` on the default auto interval** (raw: `examples/perf_hil/results/thread_safety_flakes_2026-09-27.txt`):

| build | plain | tsan |
|---|---|---|
| 0f877d54 (before) | 3/200 | 0/40 |
| + retry fix | 3/200, 11/600 | 0/40 |
| + zeroed padding | 0/200, 0/600 | 0/40 |

**Cost of the padding zeroing:** `core_cost_ab.sh -r 20`, 803db9d1 against a51b8d2f, held within 2 x SE:

| run | send (ns) | recv (ns) |
|---|---|---|
| default | +1.4 (2 x SE 2.6) | +0.1 (0.8) |
| `-R` | +2.4 (3.2) | +0.4 (0.9) |

Raw: `examples/perf_hil/results/core_cost_ab_padding_zero{,_R}_2026-09-27.txt`.

## Order and ownership

- Dev writes each stage in its own worktree and pushes only after telling Plan.
- Stage 1 is one commit, the rename, plus a separate commit for its docs note, so that `git blame` can skip it
  (`.git-blame-ignore-revs`).
- Stages 2 and 3 are each pre-registered here, measured, and read by Plan before the next starts.

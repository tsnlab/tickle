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

**What is not renamed, and why:**
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

## Stage 2 - a lightweight core `tt_Node` (no wire change)

**The model:**
- A `tt_Context` owns sockets, scheduler, poll, liveliness and discovery, as today.
- A `tt_Node` is a name, a namespace and an index within its context, and owns endpoints.
- Every endpoint belongs to exactly one node.
- A context creates a default node at `tt_Context_create()` (index 0, name "tickle", namespace "/"), so the simple
  path stays one call longer at most.

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
  - rmw_tickle's nodes are core nodes.
  - Each is a test with a mutant it fails against.
- **Nothing observable moves:** the announce bytes are byte-identical to stage 1's for the same endpoints, since there
  is no wire change. The rmw suite, the gates and the typesupport pytest all pass.
- **CPU:** the `core_cost_ab.sh` pair (default and `-R`) is held within 2 x SE. An endpoint holding one more pointer
  is the only change on the hot path's data.

## Stage 3 - discovery carries nodes (wire v11)

**The wire diff:**
- **A node entry in the announce list.** `tt_UpdateEntity` with:
  - `kind = tt_KIND_NODE` (0x40, a bit no endpoint kind uses);
  - `entity_id` = the node's own random id;
  - `endpoint_id` = hash(namespace, name);
  - type string = namespace, name string = node name;
  - `qos` bits 4-7 = the node's index.
  - One per node, in the same list as its endpoints.
- **Every endpoint entry** carries its owning node's index in `qos` bits 4-7. Bits 0-3 are the four QoS flags in use;
  4-7 are unused today, so this costs 0 B.
- `tt_VERSION` goes 10 -> 11.
- **The receiver:**
  - stores node entries in the discovery table like any entity;
  - forgets or tombstones them with their source;
  - never matches them as endpoints: `count_matching`, RxO, peers and the W1-era directory all skip
    `tt_KIND_NODE`, each pinned by a test.
- **rmw:**
  - `rmw_get_node_names*()` returns local nodes plus alive remote node entries.
  - `rmw_get_{publisher,subscriber,service,client}_names_and_types_by_node()` match remote endpoints by
    (source, node index).

**Stage 3 PASS, in the user's rule's terms:**
- **Function, two processes in two private netns (`ros2 node list`, `ros2 node info /talker`, `ros2 param list
  /talker`):**
  - they list the remote node and its endpoints, as CycloneDDS does on the same pair;
  - the rmw suite passes;
  - `test_rmw_implementation`'s graph tests pass.
- **Recorded costs, pre-registered as the price of the feature:**
  - **M1 join bytes:** +41 B per node per list for a node named "talker" in "/"
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

## Order and ownership

- Dev writes each stage in its own worktree and pushes only after telling Plan.
- Stage 1 is one commit, the rename, plus a separate commit for its docs note, so that `git blame` can skip it
  (`.git-blame-ignore-revs`).
- Stages 2 and 3 are each pre-registered here, measured, and read by Plan before the next starts.

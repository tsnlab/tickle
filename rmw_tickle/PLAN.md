# rmw_tickle — implementation plan

An `rmw` implementation backed by TickLE, so ROS 2 (`rclcpp`/`rclpy`) can run directly over
TickLE's own CDR-4 wire format instead of DDS. Lives at `rmw_tickle/rmw_tickle/` (a normal
`ament_cmake` package) inside the TickLE monorepo, next to the library it wraps.

**Status**: scaffold only (`rmw_init`/`rmw_shutdown`/`rmw_context_fini` + `rmw_init_options_*`,
merged via #20). Everything below is planned, not yet built.

## Design philosophy

TickLE itself stays as unmodified as possible; only the handful of places where its own design
philosophy would otherwise conflict with the `rmw` contract get resolved by extending TickLE with
a small, generically-useful primitive (never a workaround bolted onto the `rmw` side alone), or by
absorbing the difference into `rmw_tickle` when no such primitive is needed:

| Area | TickLE | rmw_tickle |
|---|---|---|
| Threading | Stays single-threaded per `tt_Node` - driven by exactly one thread | Owns all lock/thread management. A background thread per node drives `tt_Node_poll()`; a per-node mutex serializes every other entry point (`rmw_publish`, ...) against it |
| Memory | Stays malloc-free (`src/` calls neither `malloc` nor `free` - a hard existing invariant) | Free to use `rcutils_allocator_t`-based allocation throughout, per the `rmw` contract |
| Interface coverage | N/A | Supports only the `.msg`/`.srv` subset `tools/typesupport` already generates, plus TickLE's own single-datagram size ceiling - see "Supported subset" below |
| QoS | N/A | Rejects anything outside the currently-supported set explicitly (`RMW_RET_UNSUPPORTED`-equivalent), rather than silently downgrading it - see "QoS roadmap" below |

The one deliberate TickLE core extension (Phase 0, below) is a blocking-poll wake primitive -
narrow enough that any multi-threaded consumer of TickLE would want it, not something invented
just to make `rmw_tickle` fit.

## Supported subset

A ROS 2 interface or QoS request outside this list is a hard, explicit failure (a `rosidl_
typesupport_tickle_c` generate-time error, or an `RMW_RET_UNSUPPORTED`-equivalent return from
`rmw_create_publisher`/`rmw_create_subscription`/etc.) - never silent best-effort degradation.

- **Message/service grammar**: exactly what `tools/typesupport/PLAN.md`'s own "Out of scope"
  section lists as unsupported (`.action`, multi-dimensional arrays, `wstring`, 128-bit types,
  arrays of strings, arrays of nested message types, defaults on a nested message field) - a ROS 2
  package using any of those simply cannot get a `rosidl_typesupport_tickle_c` for that interface.
- **Message size**: TickLE serializes within one datagram, no fragmentation
  (`tt_MAX_BUFFER_LENGTH`, DESIGN.md's "Interface serialization" section) - independent of the
  grammar limit above. A message using only supported grammar can still be individually too large
  (e.g. a bounded array sized past what fits) to ever encode.
- **QoS**: see the roadmap below - everything not yet listed there is rejected.

## QoS roadmap

Rejected outright until implemented, in this order (local/`rmw`-only work first, TickLE
wire-protocol work last):

| # | Policy | What it needs | Where |
|---|---|---|---|
| 1 | `HISTORY` / `DEPTH` | A bounded per-subscription queue | `rmw_tickle` only |
| 2 | `DEADLINE` | Elapsed-time monitoring since last publish/receive | `rmw_tickle`, via `tt_Node_schedule()` |
| 3 | `LIVELINESS` | Per-entity liveliness, extending Phase 0's node-level timeout | Phase 0's mechanism, generalized |
| 4 | `DURABILITY` (`TRANSIENT_LOCAL`) | A retained-sample cache per publisher + backlog delivery to a newly-discovered subscriber | TickLE core |
| 5 | `RELIABILITY` (`RELIABLE`) | ACK/NACK + retransmission - the biggest lift here | TickLE core (`tt_RELIABLE_*`, already reserved - DESIGN.md's "Known limitations") |
| 6 | `LIFESPAN` | Expiring samples past an age - needs #1/#4's storage to already exist | TickLE core + `rmw_tickle` |

## Milestones

| | Deliverable | Depends on | |
|---|---|---|---|
| **0** | TickLE core extensions: (a) a primitive letting one thread wake another's blocking `tt_Node_poll()` early (new HAL fd + a public `tt_Node_interrupt()`-shaped function, `hal.h`/`hal_linux.c`/`hal_freertos.c`); (b) liveliness timeout - N consecutive missed periodic UPDATEs from a peer ⇒ considered dead, N a `#define` of its own (separate from `tt_NODE_UPDATE_INTERVAL`); (c) a discovery API - a snapshot query over currently-known nodes/endpoints, plus a registered callback fired on any graph change (append/update/departure - departure driven by (b)) | none | ⬜ |
| **1** | `rosidl_typesupport_tickle_c` (+ `..._cmake`): hooks `rosidl_generate_interfaces`' typesupport extension point, invokes `tools/typesupport` at ROS 2 build time, wraps the generated `<Msg>_encode`/`_decode` in a `rosidl_message_type_support_t`/`rosidl_service_type_support_t`. "Supported subset" (above) written into this PLAN.md | none (parallel with 0) | ⬜ |
| **2** | `rmw_create_node`/`rmw_destroy_node`; per-node mutex + background poll thread (uses 0a). One `tt_Node` per process assumed for now - multiple ROS nodes per process explicitly deferred, tracked below | 0, 1 | ⬜ |
| **3** | `rmw_create_publisher`/`rmw_publish`; `rmw_create_subscription`/`rmw_take(_with_info)` (subscriber callback pushes into a per-entity FIFO, `rmw_take` pops it); `rmw_serialize`/`rmw_deserialize` | 1, 2 | ⬜ |
| **4** | `rmw_create_service`/`rmw_create_client`/`rmw_send_request`/`rmw_take_request`/`rmw_send_response`/`rmw_take_response` | 1, 2, 3 | ⬜ |
| **5** | `rmw_create_wait_set`/`rmw_wait`/`rmw_create_guard_condition`/`rmw_trigger_guard_condition`, built on 2's FIFOs + condvars + 0a's wake primitive | 2 | ⬜ |
| **6** | `rmw_get_node_names`, `rmw_count_publishers`/`subscribers`, `rmw_service_server_is_available`, ...; graph-changed guard condition wired to 0(c)'s callback | 0 | ⬜ |
| **7** | QoS rejection logic in `rmw_create_publisher`/`rmw_create_subscription`/etc. QoS roadmap (above) written into this PLAN.md; roadmap items implemented one at a time as separate follow-on work | 2, 3 | ⬜ |
| **8** | Packaging: `<member_of_group>rmw_implementation_packages</member_of_group>` in `package.xml`; `share/ament_index/resource_index/rmw_typesupport/rmw_tickle` resource marker + its `install()` rule; verify `RMW_IMPLEMENTATION=rmw_tickle` selection works | 1 | ⬜ |
| **9** | This document, kept current as the single place mapping `rmw` concepts to TickLE ones (`tt_Node`/`tt_Publisher`/...), scope/non-goals, threading/locking model, QoS and subset limits | ongoing | ⬜ |
| **10** | A scoped test suite (only what "Supported subset" + the QoS roadmap actually cover - not full upstream conformance); an explicit xfail/skip list for `rmw_implementation`/`test_rmw_implementation` cases that are expected to fail rather than silently ignored; a real colcon build+test CI job (today's `check-all.yml` only builds `rmw_tickle` far enough to lint it) | 2–7 | ⬜ |

Deferred, tracked here rather than solved now: **multiple ROS 2 nodes per process** (Milestone 2
assumes one `tt_Node` per process; a component container wanting several ROS nodes in one process
would need one `tt_Node` - own socket, own poll thread - per node, which works but hasn't been
built or measured).

## Build order

0 and 1 in parallel → 2 → 3 and 4 (parallel) and 5 (parallel, only needs 2) → 6 (only needs 0) →
7/8/9 alongside anything → 10 following each milestone as it lands, not saved for the end.

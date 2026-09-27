# RMW_GAPS_PLAN: closing rmw_tickle's API gaps (pre-registered 2026-09-27, Plan, before any code)

The user approved closing these gaps on 2026-09-27 ("1, 2, 3, 4번 진행하자"). They come from COMPARISON.md 2.7a, where
TickLE is ❌ and both vendors are ✅, and they appear in CONTEXT_NODE_PLAN's roadmap as g1-g6. Dev implements, in the
order the roadmap gives. Plan wrote the pass criteria below and the executable acceptance tests before any code:

- `rmw_tickle/scripts/rmw_gap_acceptance.sh -w WS [TEST...]` runs each scenario as two default-options rclpy nodes in
  two private netns joined by a veth (so "remote" is another host to every rmw, and nothing reaches the PC's LANs).
- It runs rmw_cyclonedds_cpp first as the **control**, which must PASS or the test is VOID; then rmw_tickle.
- A helper that did not run makes the test ERROR, not FAIL.
- Before any fix every test is expected to FAIL on rmw_tickle. A PASS there before its fix is a finding: the test
  cannot fail, or the gap is not what 2.7a says.

Every item also carries: unit tests in `rmw_tickle/test` with at least one mutant that fails them; gates; the rmw suite
in a private netns; the row(s) in COMPARISON 2.7a updated in the same commit; and a CPU check (below) where the change
touches a per-sample path.

## g5 - `rmw_take_sequence` (acceptance test `takeseq`)

- **Gap:** the symbol is not defined in `librmw_tickle.so`. `rmw_implementation` dispatches it on jazzy, so
  `rcl_take_sequence()` fails and rmw_implementation logs a failed symbol lookup at every start.
- **Pass:**
  - `takeseq` passes (the symbol is defined);
  - a unit test takes N ≤ count messages in one call and gets them in publish order with correct message_info, including
    `taken` below `count`, with the empty queue case returning `taken = 0` and `RMW_RET_OK`;
  - a mutant that takes only one per call fails it.
- **CPU:** none on the sample path; the existing rmw_take is untouched.
- **Contract details, added by Dev before code (2026-09-28), from rmw.h:**
  - `RMW_RET_INVALID_ARGUMENT` for a NULL argument, `count` 0, or either sequence's capacity below `count`. Both
    sequences are left unchanged.
  - Up to `count` messages are taken in queue order, each exactly as `rmw_take_with_info` takes one, so a message
    past its lifespan is dropped the same way. It stops at the first empty take.
  - `*taken` and both sequences' `size` become the number taken. With none taken, the sequences are left unchanged
    and the call returns `RMW_RET_OK`, as rmw.h requires.
  - A message delivered while the call runs may be included; there is no snapshot, as in the vendors.
  - Mutants: one message per call; the sizes not set; `count` 0 accepted.
- **Result (Dev, 2026-09-28): PASS.**
  - `takeseq`: CycloneDDS PASS (control), rmw_tickle PASS (baseline: FAIL, symbol not defined).
  - `test_take_sequence` covers the whole contract:
    - three of five in order, with their psns;
    - taken below count;
    - the empty take: `RMW_RET_OK`, 0, both sequences unchanged;
    - `count` 0 and a count past the capacity refused.
  - The three mutants fail it at the predicted asserts.
  - The rmw suite passes in a private netns, 54/54.
  - No CPU check: `rmw_take` / `rmw_take_with_info` are unchanged.

## g2 - on-new-data callbacks (acceptance test `events`)

- **Gap:** `rmw_subscription_set_on_new_message_callback`, `rmw_service_set_on_new_request_callback`,
  `rmw_client_set_on_new_response_callback` and `rmw_event_set_callback` return UNSUPPORTED, so rclcpp's
  EventsExecutor and rclpy's `EventsExecutor` cannot run on rmw_tickle.
- **Contract (from rmw.h):**
  - the callback is called with the number of new items each time data arrives;
  - when set while items are already waiting, it is called once at once with that count;
  - clearing it (NULL) stops the calls;
  - the call must not happen with rmw's own locks held in a way that deadlocks a callback that calls back into rmw.
- **Pass:**
  - `events` passes: a listener on rclpy's EventsExecutor receives the other host's talker, at least 20 of about 80
    messages;
  - unit tests for each of the four setters: the count arrives per new item; the backlog is reported at set time;
    NULL stops it; a callback that calls rmw_take from inside does not deadlock (with a timeout);
  - mutants: the backlog not reported at set time; the callback not cleared on NULL.
- **CPU:** a branch per delivery when no callback is set. Core_cost bench and the rmw block RTT rows must not be WORSE
  under WIRE_PLAN 8.3, with the placement control of its 2026-09-27 amendment.
- **Contract details, added by Dev before code (2026-09-28):**
  - **Who calls, and under which lock.** The callback is called from rmw_tickle's poll thread, after the item is in
    its queue and the queue's own mutex is released, so a callback that calls `rmw_take` / `rmw_take_request` /
    `rmw_take_response` / `rmw_take_event` from inside does not deadlock. (The context lock the poll thread holds is
    re-entrant.)
  - **The guarantee NULL gives.** Each entity has a callback slot with its own mutex, held while the callback runs,
    as rmw_cyclonedds does. So once a setter returns - NULL or a new callback - the old one is never called again.
    A callback that calls a setter on its own entity deadlocks, as in rmw_cyclonedds; that is not supported.
  - **The count at set time.**
    - Subscription: the messages waiting in its queue.
    - Service / client: 0 or 1, since rmw_tickle keeps one pending request or response.
    - Event: its unread count, the same number `rmw_take_event` would report.
    - An item arriving while its setter runs may be reported both in the set-time count and on its own, so the count
      may be one high, never low. rclcpp's EventsExecutor tolerates a high count: a take then returns
      `taken = false`.
  - **Events** covered: every type rmw_tickle's `rmw_*_event_init` accepts today - deadline missed (both sides),
    liveliness lost / changed, QoS incompatible (both sides). g3's new types get the same slot when they are added.
    The callback's count is the change in the unread count.
  - **Unit tests add:** once `rmw_*_set_*_callback(NULL)` has returned, a delivery from the poll thread does not call
    the old callback, with a mutant that clears the slot without its mutex.
- **Result (Dev, 2026-09-28): PASS.**
  - `events`: rmw_cyclonedds_cpp PASS (control), rmw_tickle PASS (it was FAIL, received=0, before g2).
  - `test_event_callbacks` covers the four setters. It checks a call per item, the backlog at set time, that NULL
    stops the calls, a take from inside the callback (with alarm() as the deadlock timeout), and that no call runs
    once set(NULL) has returned. The rmw suite passes in a private netns, 50/50.
  - Mutants, each failing at the predicted assert: backlog not reported; NULL not clearing; slot cleared without its
    mutex.
  - **Found while testing, fixed before push.** The first version read the backlog before storing the callback. A
    message queued between the two was reported by neither, a count "low", which the contract above forbids. Now:
    - the count is read after the store, under the slot mutex;
    - a delivery with no callback set costs an atomic load and a branch, as the CPU line says, and re-checks under
      the mutex when one is set.
  - A race test covers it, 20000 rounds of setting while a message is delivered. It guards the order, not the
    timing:
    - left narrow, the old order was missed 0 of 20000 times on the PC;
    - widened by 20 us, as a preemption would, it missed 19996 of 20000;
    - the right order, widened the same way, missed 0.
  - **CPU**, `rmw_lib_ab.sh`, parent vs g2, 10 reps interleaved, with an A/A (parent vs its copy) as 8.3's
    placement control. The reading was written down before the run: WORSE only if B-A exceeds |A/A B-A| plus 2 SE.
    All four rows are inside:

    | gap | metric | B-A median (2 SE) | A/A B-A (2 SE) | verdict |
    |---|---|---:|---:|---|
    | 5 ms | RTT | -4.5 us (5.9) | +2.5 us (5.6) | inside |
    | 5 ms | pong CPU | -2.0 ms (3.7) | -2.1 ms (3.1) | inside |
    | 100 ms | RTT | +22.6 us (13.4) | +14.8 us (11.8) | inside |
    | 100 ms | pong CPU | -0.0 ms (2.4) | +0.7 ms (1.9) | inside |

  - The 100 ms RTT row resolves only to about 28 us: the A/A alone moved +14.8 us. So it rules out a large cost, not
    a small one. A branch per delivery is the small kind.
  - The core_cost bench was not run: g2 changes no core source, so its binary is byte-identical to the parent's.

## g3 - the remaining event types (acceptance tests `matched`, `itype`)

- **Gap:** TickLE serves 6 of 11 event types. Missing: `PUBLICATION_MATCHED`, `SUBSCRIPTION_MATCHED`,
  `PUBLISHER_INCOMPATIBLE_TYPE`, `SUBSCRIPTION_INCOMPATIBLE_TYPE`, `MESSAGE_LOST`. On lyrical,
  `rmw_get_clients_info_by_service` / `rmw_get_servers_info_by_service` belong here too.
- **Semantics to match (the vendors' status structs):**
  - MATCHED: total_count, total_count_change, current_count and current_count_change, raised when a remote endpoint of
    the same topic and type becomes matched or unmatched (discovery add and remove, liveliness loss included).
  - INCOMPATIBLE_TYPE: raised when a remote endpoint on the same topic has a different type.
  - MESSAGE_LOST: raised by a subscription when samples are known lost. TickLE knows it for RELIABLE (a gap given up
    after retries) and for BEST_EFFORT sequence gaps.
- **Pass:**
  - `matched` and `itype` pass: both sides see at least one event;
  - unit tests: counts and changes for match, unmatch and rematch; type mismatch both ways; MESSAGE_LOST counts equal the
    samples actually skipped in a lossy test (tc/netem loss in netns, or whitebox drop);
  - on lyrical, the two service info queries return the same endpoints `ros2 service info --verbose` shows on CycloneDDS;
  - mutants: MATCHED not raised on unmatch; MESSAGE_LOST counting gaps twice.
- **CPU:** MESSAGE_LOST adds work only when a gap is detected. Same CPU check as g2.
- **Contract details, added by Dev before code (2026-09-28):**
  - **What "matched" means.** A local endpoint and an endpoint of the other kind are matched when all of these hold:
    - they are on the same topic, with the same type name;
    - their QoS is compatible (the same RxO rule behind QOS_INCOMPATIBLE);
    - a remote one is alive (`tt_Context_entity_alive`).
    - Local endpoints count too, as they do today.

    The vendors match only same-type, compatible pairs. `rmw_publisher_count_matched_subscriptions` /
    `rmw_subscription_count_matched_publishers` change to this same predicate, so the event and the query never
    disagree. Today they count by topic name alone, which counts a type-mismatched or incompatible endpoint as
    matched.
  - **When MATCHED is raised.** When rmw's discovery callback reports a remote endpoint (appear, refresh, liveliness
    lapse or revival, depart), and when a local endpoint is created or destroyed. The matched count is recomputed for
    each local endpoint on that topic.
    - `current_count` is the new count.
    - `total_count` grows by every increase, so an unmatch followed by a rematch counts twice, as in DDS.
    - Each callback reports one entity, so an increase and a decrease cannot cancel between two recounts.
    - The `*_change` fields are the difference since the last `rmw_take_event`.
  - **INCOMPATIBLE_TYPE** is raised on the same triggers. It counts endpoints of the other kind on the same topic with
    a different type name (local ones, and alive remote ones); `total_count` grows by every increase.
    - `itype` stays VOID (the baseline run), so this criterion is judged by unit tests alone.
  - **MESSAGE_LOST: counted from rmw's publication sequence number (psn), per writer, not from core's datagram
    counters.**
    - Every rmw message carries its publisher's psn, contiguous per publisher. During the subscriber callback, core
      has already set `tt_Subscriber.last_source` / `last_entity_id` to the delivering writer.
    - The subscription keeps the last psn per writer. A jump from p to q > p + 1 counts q - p - 1 lost messages.
    - The first message from a writer only sets its baseline. A late joiner has lost nothing, and neither has a
      durable replay starting mid-history.
    - **A late arrival is not a restart (Plan's fix, 2026-09-28).** A psn below the expected next one, but within 64
      of it, is a late arrival and is ignored: no reset and no decrement, since DDS does not decrement either.
      - Only a jump further back resets the baseline, as for a publisher re-created under the same endpoint id,
        which starts again at 1.
      - Core already drops a BEST_EFFORT sample that arrives behind a newer one, so a late sample normally never
        reaches rmw, and counting it as lost is correct.
      - Whitebox: 1, 2, 4, 3, 5, 6 counts 1. The mutant that resets on any backward psn counts 2, because 4 is
        counted again after 3.
    - This counts messages, not datagrams, the same way for BEST_EFFORT and RELIABLE. A RELIABLE gap shows as a psn
      jump only when core gives up on it (`gap_abandoned` / `gap_evicted`); one that is repaired shows nothing.
    - Core's own gap counters are therefore not added on top: that would count the same loss twice. That is the
      pre-registered mutant.
    - **Limits:** a loss at the tail is counted when that writer's next message arrives, as with the vendors'
      BEST_EFFORT. KEEP_LAST queue overflow at the reader is not a loss, as in DDS.
    - The writer table has tt_MAX_PEER_COUNT entries. A writer beyond that evicts the least recently seen one, so an
      evicted writer that returns starts a new baseline: under-counted, never over-counted.
  - **CPU, amending the line above:**
    - MESSAGE_LOST adds per-delivery work even when nothing is lost: a compare of the delivering writer against the
      last one, and one psn compare. The psn is already parsed on that path.
    - Recounting for MATCHED / INCOMPATIBLE_TYPE runs on the discovery path only.
    - The same CPU check as g2 applies: `rmw_lib_ab.sh` with an A/A.
  - **g2's callbacks** get the same slot for the five new types; the set-time count is the unread change.
  - **Lyrical's two service queries** (`rmw_get_clients_info_by_service` / `rmw_get_servers_info_by_service`) list
    local and remote clients / servers of the service, filled as `rmw_get_publishers_info_by_topic` fills a
    topic's. They are checked against CycloneDDS's `ros2 service info --verbose` (Pass above).
  - **Unit tests add:**
    - MESSAGE_LOST, whitebox: psn sequences from two interleaved writers with known gaps give the exact count; a
      restart and a late join count 0.
    - MATCHED, over the wire in one process: match, unmatch (destroy), rematch. Also a type-mismatched and a
      QoS-incompatible endpoint, which must not count as matched, and the query agreeing with the event.
    - INCOMPATIBLE_TYPE both ways.
  - **Mutants:**
    - MATCHED not raised on unmatch;
    - MESSAGE_LOST adding core's gap counters to the psn count (counting twice);
    - the first message from a writer counted as a loss from psn 0;
    - a reset on any backward psn (Plan's);
    - the matched predicate ignoring the type.
- **Result (Dev, 2026-09-28): PASS.**
  - Acceptance:
    - `matched`: CycloneDDS PASS (control), rmw_tickle PASS (baseline: FAIL, UnsupportedEventTypeError).
    - `itype`: rmw_tickle PASS across the two hosts, but CycloneDDS FAIL (itype_pub=0 itype_sub=0), so it stays VOID
      as pre-registered.
  - `test_g3_events`:
    - MATCHED: match, unmatch (observed through the event's callback: the event is raised), rematch.
    - Another type is INCOMPATIBLE_TYPE on both sides and never matched; a RELIABLE request against a BEST_EFFORT
      offer does not match; `rmw_*_count_matched_*` agree with the event.
    - MESSAGE_LOST over four writers: gaps, a RELIABLE gap core gave up on counted once, a late join, the late
      arrival 1, 2, 4, 3, 5, 6 counting 1, and a far backward jump resetting.
    - Lyrical's service queries: one client row and one server row, with their node.
  - Mutants, each failing at the predicted assert:
    - MATCHED not raised on unmatch. It first survived, because the test only took the event; it now watches the
      event's callback.
    - Core's gap counter added to the psn count.
    - The first message counted from psn 0.
    - A reset on any backward psn.
    - The predicate ignoring the type.
  - The rmw suite passes in a private netns, 52/52.
  - `ros2 service info --verbose` (`scripts/rmw_service_info_check.sh`) lists the same client and server on both
    rmws: svc_client_node, svc_server_node, the same type.
    - Differences: endpoint count 1 (CycloneDDS 2, its request and reply readers/writers), type hash INVALID, and
      a remote row's QoS unknown.
    - The query needs `TICKLE_NODE_ID` when it shares an address with another rmw_tickle process (rmw_init.c).
  - Found on the way: MESSAGE_LOST keys writers by core's per-instance entity id (Milestone 47). A publisher
    re-created under the same endpoint id is therefore a new writer, with a new baseline. The far-backward reset
    only covers the same instance jumping back.
  - **CPU**, `rmw_lib_ab.sh`, parent vs g3, 10 reps interleaved, with an A/A as 8.3's placement control. The reading
    was written down before the run: WORSE only if B-A exceeds |A/A B-A| plus 2 SE. All four rows are inside:

    | gap | metric | B-A median (2 SE) | A/A B-A (2 SE) | verdict |
    |---|---|---:|---:|---|
    | 5 ms | RTT | -1.9 us (2.8) | -0.1 us (8.8) | inside |
    | 5 ms | pong CPU | -0.6 ms (2.1) | -0.9 ms (5.2) | inside |
    | 100 ms | RTT | +8.9 us (22.2) | +5.9 us (21.3) | inside |
    | 100 ms | pong CPU | -0.5 ms (0.8) | +0.0 ms (0.7) | inside |

    As for g2, the 100 ms RTT row resolves only to about 28 us.
  - The core_cost bench was not run: g3 changes no core source.

## g6 - discovery options (acceptance tests `range`, `peers`)

- **Gap:** `rmw_init_options_t.discovery_options` is never read, so `ROS_AUTOMATIC_DISCOVERY_RANGE` and
  `ROS_STATIC_PEERS` have no effect.
- **Design, to be confirmed in Dev's pre-code review:**
  - SUBNET is today's behaviour.
  - LOCALHOST binds and broadcasts on loopback only, so no datagram leaves the host.
  - OFF sends no discovery at all. The ROS 2 definition allows same-process communication; check what the vendors
    actually do.
  - SYSTEM_DEFAULT is SUBNET.
  - Static peers: every discovery summary and list is also sent by unicast to each listed address, on the domain's
    well-known port, whatever the range. Unicast replies from them are accepted.
- **Pass:**
  - `range` passes: LOCALHOST gives 0 messages across the two hosts and SUBNET at least 20;
  - `peers` passes: LOCALHOST plus static peers naming each other gives at least 20;
  - unit tests on the address and port each mode binds and sends to;
  - a pcap control in the LOCALHOST arm: no datagram on the veth;
  - mutants: LOCALHOST still broadcasting on the subnet; static peers only on one side's send.
- **CPU:** none on the sample path.

## Stage 3 and g1 (acceptance tests `graph`, `bag`)

Their criteria live in CONTEXT_NODE_PLAN (stage 3) and in the large-message plan (g1, with the direct typesupport).
The acceptance tests `graph` (node list, node info and param get across hosts) and `bag` (record, then play into a
listener on the other host, at least 30 of about 140 messages both ways) are added to their PASS lists.

## Baseline run, before any fix (2026-09-27, Plan)

`rmw_gap_acceptance.sh -w /tmp/plan_accept` on `main` at `f22fd4fa` (lyrical, PC, two private netns). rmw_tickle was
built Release from a worktree, with the standard interfaces from `build_ros2_interfaces.sh`.

| test | control | rmw_tickle |
|---|---|---|
| graph | CycloneDDS PASS | FAIL (node list 0, node info 0, param 0) |
| bag | CycloneDDS PASS | FAIL (recorded 0, replayed 0) |
| events | CycloneDDS PASS | FAIL (received 0) |
| matched | CycloneDDS PASS | FAIL: rclpy raises UnsupportedEventTypeError, and the node dies |
| itype | **CycloneDDS FAIL, FastDDS FAIL** | FAIL |
| takeseq | CycloneDDS PASS | FAIL (symbol not defined) |
| range | CycloneDDS PASS | FAIL (LOCALHOST 90, SUBNET 90: the range is ignored) |
| peers | CycloneDDS PASS | FAIL (LOCALHOST 90 without peers: the same cause) |

- **Every valid test fails on rmw_tickle before its fix, as expected.**
- **`itype` is VOID.** Neither vendor raises INCOMPATIBLE_TYPE for std_msgs/String against std_msgs/Int32 on one
  topic across the two hosts within 10 s, so the test has no control. g3's incompatible-type criterion is judged by
  its unit tests alone, until a scenario is found where a vendor raises it.
- **Three fixes to the test itself, made during this run:**
  - `peers` first passed on rmw_tickle before any fix. rmw_tickle ignores the range, so LOCALHOST never isolated, and
    static peers "worked" by doing nothing. It now carries its own control arm: LOCALHOST without peers must give 0.
  - `bag` used the positional topic argument, which lyrical's `ros2 bag record` rejects (`--topics` now), so the
    control failed. It was fixed and re-run: the control passes.
  - `matched`: rclpy's UnsupportedEventTypeError is what a user's node gets on rmw_tickle, so it counts as FAIL, not
    ERROR.

## g8 - processes on one host (found 2026-09-28; top priority; acceptance test `samehost`)

- **Gap:** two rmw_tickle processes on one host do not communicate by default. A talker and a listener as two
  default-options rclpy processes in one netns: CycloneDDS received 80 messages, **rmw_tickle 0**, and `ros2 topic echo`
  next to a talker printed nothing.
  - So the most basic ROS 2 use, several terminals or a launch file with several processes on one machine, fails.
  - Dev found it while writing g3's service check. `TICKLE_NODE_ID` is the known workaround (rmw_init.c).
  - Every benchmark ran one process per Pi, and the acceptance tests ran two hosts, so none of them could see it.
- **Pass, with the criteria to be completed by Dev's characterisation, before code:**
  - `samehost` passes: the listener gets at least 50 of about 140, and `ros2 topic echo --once` prints a message, with
    the CycloneDDS control passing;
  - every acceptance test that passed before still passes;
  - N processes on one host (N = 8) all discover each other, with no TICKLE_NODE_ID set;
  - more contexts than the wire's id space in one domain is detected and refused with a clear error, never silent.
  - If the fix changes what the context id means on the wire, it goes under WIRE_PLAN's rule before code.
- **Baseline:** `samehost`: CycloneDDS PASS, rmw_tickle FAIL (listener 0, echo 0).
- **Characterisation (Dev, 2026-09-28):**
  - **The id.** `tt_get_node_id()` (hal_linux.c) takes the last octet of the local address on the broadcast subnet.
    Every process on one address therefore gets the same id; `TICKLE_NODE_ID` overrides it (rmw_init.c).
  - **The drop.** `self_sent = header->source == node->id` (tickle.c, `process_packet`). With it set, DATA and its
    fragments are suppressed, and those carry the announces, so neither discovery nor data arrives.
  - **Why the filter alone is not the fix.** Everything downstream is keyed by the 8-bit source: peer tables,
    discovery, writer proxies, announce generations. Two processes sharing an id would be merged into one peer. So
    each process needs its own id on the link.
  - **CI** passes only because `check_ros2_interfaces.sh` gives every process a `TICKLE_NODE_ID` (121-125), as every
    perf and HIL script does.
  - Every send leaves from the context's data socket, bound to its address with a port the kernel assigns, one per
    process. So (sender address, sender port) says exactly which process sent a packet.
- **Design, approved by Plan 2026-09-28, no wire-format change:**
  - **Scope.** Compile-time `tt_CONTEXT_ID_CLAIM`, default 0 in core and set to 1 by rmw_tickle's build. With it 0,
    core's default build is byte-identical to its parent, so the embedded targets and the benchmarks keep today's
    behaviour.
  - **Preferred id** is unchanged: the address's last octet. A single process per host keeps today's id, and its
    announce bytes are identical.
  - **Host registry** (hal_linux.c), one file per (port, address) in /dev/shm, updated under `flock`.
    - It records the pid holding each id. The first process takes the preferred id; a later one takes the highest
      free id counting down from 254.
    - A holder whose pid is dead (`kill(pid, 0)` gives ESRCH) is free.
    - A pid that cannot be verified (EPERM, or another pid namespace) counts as live, since the link net below
      resolves any mistake.
    - No usable /dev/shm: the context takes the preferred id and relies on the link net.
    - The id is released when the context closes.
  - **Self** is a packet whose source is this context's id and whose sender is this context's own data socket. Any
    other packet carrying this context's id is a **collision**; it is not processed.
  - **Who moves.**
    - A context within 2 announce intervals of its creation (its startup window) yields on a collision, and an
      established one keeps its id.
    - When the collision persists for 2 announce intervals, so both sides are established (a partition healed, or an
      id set explicitly), the higher (address, port) moves.
    - An established context that sees a collision announces again at once, so its peers correct any merged state
      from its authoritative announce.
  - **Moving.**
    - The mover sends no farewell under the old id, because peers would then drop the other holder.
    - It takes a free id: not seen on the link, not in the registry. With several free ids, one is picked from its
      own port, so that two movers seeing the same link do not both pick the same one.
    - It updates the registry and announces under the new id at once. Its entries left under the old id at peers are
      replaced by the keeper's next announce, which is authoritative for that source.
  - **An explicit id** (`TICKLE_NODE_ID`, `_tt_CONFIG.context_id`) never moves. A collision on it is logged at ERROR,
    once per foreign sender.
  - **No free id:**
    - at creation, `tt_Context_create` fails with an error saying so, and `rmw_init` fails;
    - found later, the context logs ERROR and stops sending. Never silent.
- **Pass (pre-registered before code):**
  - **One process, today's behaviour:**
    - the core default build (`tt_CONTEXT_ID_CLAIM` 0) has objdump-identical `tickle.o` and `hal_linux.o` to its
      parent;
    - a one-process rmw_tickle context takes the same id and sends announce bytes identical to its parent's (pcap
      diff);
    - the rmw CPU A/B, with an A/A, stays inside the swing.
  - `samehost` passes, and every acceptance test that passed before still passes.
  - 8 rmw_tickle processes in one netns, with no `TICKLE_NODE_ID`, all see each other: every node lists the 7 others
    (`ros2 node list` from a ninth).
  - A forced collision, two processes with the registry disabled (`TICKLE_ID_REGISTRY=off`), resolves to two ids;
    the talker then reaches the listener.
  - **Unit tests (core, mock HAL):**
    - a packet with this context's id from another address is not taken as self;
    - a newcomer yields and the established context keeps its id;
    - both established: the higher (address, port) moves after 2 intervals;
    - three contexts: when the newcomer moves away from the keeper's id, the third context keeps the keeper's
      endpoints and matches;
    - an explicit id never moves;
    - with no free id, the context refuses and stops sending.
  - **Unit tests (registry, a temporary file):** a first claim gets the preferred id and a second a different one; a
    dead pid's id is reclaimed; an EPERM pid (pid 1) counts as live; a release frees the id.
  - **Mutants:** the self filter by id only; the established side yielding; no move on collision; the registry
    treating dead pids as live; a farewell sent under the old id (the three-context test fails).

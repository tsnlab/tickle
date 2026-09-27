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

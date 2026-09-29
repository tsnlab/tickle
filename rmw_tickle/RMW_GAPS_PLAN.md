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
- **Contract details, Dev before code (2026-09-28), Plan-approved design, checked against the vendor:**
  - **What CycloneDDS does**, its behaviour as read (rolling) and probed (lyrical); per the user's rule of
    2026-09-28 the implementation is our own, and no vendor code is copied:
    - NOT_SET makes node creation fail with an error. `rmw_init` itself succeeds.
    - SUBNET: multicast plus the static peers.
    - SYSTEM_DEFAULT: the vendor's own defaults, with static peers ignored and a warning.
    - LOCALHOST: localhost plus the static peers, no multicast.
    - OFF: nothing - a domain tag carrying the PID, and static peers ignored with a warning. Probed: 0 messages on
      one host and across hosts, where LOCALHOST gives 121 on one host and 0 across.
    - Also: `rmw_init_options_init()` sets LOCALHOST, and rcl sets SUBNET when `ROS_AUTOMATIC_DISCOVERY_RANGE` is
      unset.
  - **rmw_tickle** follows that, per range:
    - NOT_SET: `rmw_create_node` fails, with an error of our own wording.
    - SUBNET: today's behaviour plus the static peers.
    - SYSTEM_DEFAULT: today's behaviour; static peers ignored with a WARNING.
    - LOCALHOST: broadcasts go to 127.255.255.255 only. The data socket is bound to 127.0.0.1, or to any address
      when static peers are given. A receive filter drops a datagram from any sender that is neither loopback, this
      host's own link address, nor a static peer. `TICKLE_BROADCAST_ADDR` is overridden, with a WARNING.
    - OFF: nothing is sent to the link and nothing received is processed; sends succeed, since the user chose this,
      with one INFO line. In-process delivery (g9) still works. Static peers are ignored with a WARNING.
  - **Static peers reuse core's link table** (`_tt_CONFIG.links`, Plan's preference):
    - a peer is a link of its own, with a flag: `peer`, one remote address, a /32;
    - its "broadcast" is the peer's address on the domain's well-known port, so announces, summaries and every
      broadcast-class datagram also go to it by unicast;
    - a datagram from it matches that link.
    - Forms accepted: an IPv4 address, a hostname (resolved at init, IPv4), or a CIDR subnet (its directed broadcast).
      IPv6 is skipped with a WARNING. There is room for tt_MAX_LINK_COUNT - 1 of them, and one beyond that is a
      WARNING, never silent.
  - **All of it behind a compile flag set by rmw_tickle's build** (`tt_DISCOVERY_OPTIONS`), so core's default build
    stays byte-identical. The wire is unchanged.
- **Unit tests (pre-registered):**
  - LOCALHOST: the bind address and broadcast destination; the filter drops a foreign sender and passes loopback and
    a peer.
  - OFF: nothing sent, nothing processed.
  - Static peers: they are among the destinations of a broadcast-class datagram; a CIDR peer gives its directed
    broadcast; IPv6 and overflow warn.
  - NOT_SET: node creation refused.
  - Mutants: LOCALHOST still broadcasting on the subnet; the filter off; peers not added to the destinations.
  - Acceptance: `range` and `peers`, plus Plan's OFF arm once it is added.
- **Result (Dev, 2026-09-28): PASS.**
  - **Acceptance.** `range` PASS and `peers` PASS (both failed before, at 90 and 90), with CycloneDDS as the control.
    Every other test as before: graph, events, matched, takeseq, samehost, inprocess and durable PASS; itype VOID;
    bag FAIL (g1).
  - **`test_discovery_range`** (core, mock HAL):
    - LOCALHOST drops a foreign sender and counts it, and processes loopback, a peer, and a member of a /24 peer;
    - OFF processes nothing and sends nothing;
    - SUBNET processes everything, as before;
    - a broadcast-class datagram goes to both peer links on the well-known port, the /24 at its directed broadcast.
  - **`test_discovery_options`** (rmw):
    - `rmw_init_options_init` now defaults to LOCALHOST, as the vendors do; it was left zeroed, which reads as
      NOT_SET;
    - LOCALHOST binds and broadcasts on loopback, or binds to any address when static peers are given;
    - static peers become peer links: `localhost` looked up to 127.0.0.1, a /24 kept, IPv6 skipped with a WARNING;
    - OFF and SYSTEM_DEFAULT ignore the peers with a WARNING;
    - NOT_SET lets rmw_init through, and rmw_create_node refuses.
  - **Mutants**, each failing at the predicted assert: the filter off; LOCALHOST passing a foreign sender; peers not
    among the destinations.
  - **Found on the way:**
    - `rmw_init_options_copy` shallow-copied the options, and `_fini` never freed discovery options. Both now deep
      copy and free them, with `rmw_discovery_options_copy` / `_fini`.
    - With the vendors' default, the rmw unit tests now run under LOCALHOST. They pass.
  - Core's default build (`tt_DISCOVERY_OPTIONS` 0): `tickle.o` and `hal_linux.o` are md5-identical to the parent's.
  - The rmw suite passes in a private netns, 59/59.
  - **Pcap control** (`experiments/g6_localhost_pcap.sh`), a talker and a listener in one netns:
    - LOCALHOST put 0 UDP datagrams on the veth while the listener received 101;
    - SUBNET, the control, put 66 there with the same 101 received.
  - **CPU**, `rmw_lib_ab.sh` (SUBNET, the default, where the filter is one branch per datagram), parent against g6,
    10 reps, with an A/A. All four rows are inside:

    | gap | metric | B-A median (2 SE) | A/A B-A (2 SE) | verdict |
    |---|---|---:|---:|---|
    | 5 ms | RTT | -0.2 us (11.3) | +0.1 us (5.9) | inside |
    | 5 ms | pong CPU | +0.6 ms (4.4) | -0.0 ms (2.3) | inside |
    | 100 ms | RTT | -1.6 us (11.6) | -4.5 us (14.3) | inside |
    | 100 ms | pong CPU | -0.1 ms (0.5) | +0.4 ms (0.9) | inside |

  - Not covered yet: Plan's OFF arm in `range` (in-process only, nothing between processes). `test_discovery_range`
    covers OFF in core.

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
- **Result (Dev, 2026-09-28): PASS.**
  - **One process, today's behaviour.**
    - Core's default build (`tt_CONTEXT_ID_CLAIM` 0): `tickle.o` and `hal_linux.o` at -O2 are byte-identical to the
      parent's (same md5, identical objdump). Control: the same compile with the flag set differs.
    - A lone rmw_tickle talker, parent (A, twice: A and A2) against g8 (B), captured on the veth
      (`experiments/g8_announce_identity.sh`): three runs of three arms, IDENTICAL each time. Same id (1), same
      lengths; B differs from A only in positions where A2 does.
    - The comparison had to be amended after its first runs, and says so in its header:
      - the parent alone sends its startup either as one datagram (156 B) or as two (24 + 128 B), depending on
        whether rmw's poll thread or the first publisher wins, so datagrams are compared from the end;
      - a time field's high byte can match between A and A2 by chance, so the header area's varying positions are
        pooled across datagrams.
  - **Acceptance.** `samehost` PASS: 110 of 140 at the first run; the two processes took ids 254 and 253 from the
    registry. Every test that passed before still passes: graph, events, matched, takeseq (itype VOID as before; bag,
    range and peers still FAIL, as they are g1 and g6).
  - **`rmw_samehost_many.sh`**, with CycloneDDS as the control, both PASS:
    - `many`: 8 processes in one netns, no `TICKLE_NODE_ID`. Every node sees the other 7, and a ninth's
      `ros2 node list` names all 8.
    - `collision`: talker and listener with the registry off take the same id. The newer one logs "moves to 115",
      and the listener gets 180 of 181.
  - **Unit tests:**
    - `test_context_id_claim` (core, mock HAL): a foreign packet with this context's id is not taken as self; the
      newcomer yields even with the lower address; two established contexts, where the higher address moves after
      the window; the three-context repair; an explicit id never moves (one ERROR per foreign holder); no free id
      refuses at creation and a collision leaves the context silent; the id at creation.
    - `test_context_id_registry` (the Linux registry on a temporary file): the preferred id, then the highest free
      one; release; a dead pid's id taken back; pid 1 (EPERM) holds its id; `avoid` and `salt`; no registry.
  - **Plan's fix before push: a muted context is never silent towards its user.** At first a context left without an
    id had the HAL report its sends as done.
    - Now every send fails: `tt_Publisher_publish` returns `tt_RET_IO_ERROR`, the HAL returns -1 with ENETDOWN, and
      `rmw_publish` returns `RMW_RET_ERROR` naming the cause.
    - It logs ERROR once when muting starts, and counts every refusal in `id_muted_drops` on rmw's shutdown line.
    - Test: forced exhaustion, 3 publishes, 3 errors, `id_muted_drops` 3.
  - **Mutants**, each failing at the predicted assert:
    - the self filter by id only;
    - the established side yielding;
    - no move on collision;
    - an announce under the old id on moving;
    - the registry treating dead pids as alive;
    - a muted publish reported as a success.
    - The old-id-announce mutant first survived: the mock clock gave the keeper's repair and the mover's stray
      announce the same generation, so the third context took one for the other. The test now lets delivery take
      time, as it does on a link.
  - The rmw suite passes in a private netns, 56/56.
  - **CPU**, `rmw_lib_ab.sh`, parent against the final g8 library, 10 reps interleaved, with a fresh A/A, read as
    pre-registered. All four rows are inside:

    | gap | metric | B-A median (2 SE) | A/A B-A (2 SE) | verdict |
    |---|---|---:|---:|---|
    | 5 ms | RTT | -0.4 us (1.2) | +1.1 us (1.7) | inside |
    | 5 ms | pong CPU | -1.0 ms (0.7) | +0.5 ms (1.6) | inside |
    | 100 ms | RTT | +21.0 us (25.6) | +14.2 us (29.2) | inside |
    | 100 ms | pong CPU | +0.3 ms (0.4) | +0.0 ms (0.7) | inside |

    - The first run, on the library before the muting fix, was inside the swing on every row as well (`/tmp/g8_ab`).
    - The 100 ms RTT row resolves only to about 40 us, as in g2 and g3.


## g9 - nodes in one process (found 2026-09-28 by Dev while probing g6; ranked above g6 by Plan; acceptance `inprocess`)

- **Gap:** two nodes in one process never communicate. One rclpy process with a talker node and a listener node in
  one executor: CycloneDDS received 100 of 101, **rmw_tickle 0 of 101**.
  - All of a process's nodes share one tt_Context, and core drops its own DATA as `self_sent` (`process_submessage`),
    with no in-process path. `test_publish_take_reuse.c` documented this as a known gap.
  - So composition, component containers, composable-node launch files and any multi-node process get nothing on
    their in-process topics. Actions composed in one process fail too: their feedback and status are topics.
  - Services already work in one process: CALLREQUEST / CALLRESPONSE are exempt from the self filter (Milestone 17),
    and `test_service_roundtrip` runs a client and a service on one node.
- **Design, Dev before code (2026-09-28).** Compile-time `tt_LOCAL_DELIVERY`, default 0 in core and set to 1 by
  rmw_tickle's build, as `tt_CONTEXT_ID_CLAIM` is, so core's default build stays byte-identical. No wire change.
  - **Which subscribers.** Each Publisher keeps a count of the local Subscribers on its endpoint id (topic and type):
    - kept when either side is created or destroyed;
    - a publish with none pays one branch.
  - **When.** After the sample has gone to the link, the publish hands its encoded payload to each such Subscriber
    through `deliver_payload()`, the path a received DATA takes. It is delivered once, in publish order, as coming from
    this context and this Publisher's entity id.
    - The payload is copied first to a stack buffer of the sample's length. A callback that publishes again can
      then reuse tx_buffer, which the original encoding sat in.
    - Covered: the staging path, a sample that goes as fragments, and the zero-copy path, whose payload is the
      caller's own.
  - **QoS, as for a remote pair:**
    - an RxO-incompatible local pair is not delivered: a RELIABLE Subscriber with a BEST_EFFORT Publisher, or
      durability, deadline or liveliness as `qos_incompatible()` compares them;
    - a direct call loses nothing, so RELIABLE holds;
    - KEEP_LAST depth is the Subscriber's own queue, which drops the oldest as for remote samples.
  - **TRANSIENT_LOCAL.** A durable local Subscriber gets the durable backlog of each local durable Publisher on its
    endpoint id, oldest first, lifespan-expired entries skipped. A sample cached as fragments is reassembled; one with
    a fragment missing is skipped.
    - `tt_Subscriber_deliver_local_backlog()` does this. rmw_tickle calls it once the subscription's QoS is set,
      which happens after core has created it.
  - **No double delivery.** A context's own datagrams stay dropped as self (g8: its own socket), and a context never
    becomes its own unicast peer. So a sample reaches a local Subscriber only in-process, and a remote one only over
    the link, once.
- **Pass (pre-registered):**
  - `inprocess` (Plan) passes: talker and listener nodes in one process, plus an in-process service call, with the
    CycloneDDS control.
  - **Unit tests (core, mock HAL, `tt_LOCAL_DELIVERY` 1):**
    - every sample of a local pair arrives, in order, once;
    - a local and a remote Subscriber together: each sample arrives once at each, counted per sample id;
    - an RxO-incompatible local pair gets nothing;
    - a fragmented sample arrives whole;
    - a late durable local Subscriber gets the backlog, in order, including a fragmented sample;
    - a publish with no local Subscriber delivers nothing and sends exactly what it did before.
  - **rmw unit test:** two nodes in one context, one publishing, one subscribing, with depth 3 and 10 samples published
    before any take. The take gets the newest 3: KEEP_LAST as for remote samples.
  - The rmw suite, and every acceptance test that passed before.
  - **Mutants:** in-process delivery removed; delivered twice; the durable backlog not delivered locally; the local RxO
    check removed.
  - **CPU:**
    - `rmw_lib_ab.sh` with an A/A, as before: the benchmark has no local Subscriber, so it measures the one branch;
    - the in-process path's own cost is recorded: `conv_cost`-style, a publish with one local Subscriber against one
      without, reported and not judged.
  - Core's default build (`tt_LOCAL_DELIVERY` 0) is objdump-identical to its parent.
- **Result (Dev, 2026-09-28): PASS.**
  - **Acceptance.** `inprocess` PASS (the baseline received 0; the service half passed before and still does).
    Every test that passed before still passes: graph, events, matched, takeseq, samehost; itype VOID; bag, range and
    peers are g1 and g6.
  - **`test_local_delivery`** (core, mock HAL, fragmentation compiled in):
    - a local pair gets all 10 samples in order, once, including a 5000 B one sent as fragments;
    - a local and a remote Subscriber get each sample once, and the context's own datagrams handed back to it add
      nothing;
    - a RELIABLE Subscriber with a BEST_EFFORT Publisher gets nothing;
    - a late durable Subscriber gets the backlog of 3 in order, the fragmented one whole, then the live samples;
    - a local Subscriber changes nothing sent: the same datagrams byte for byte.
  - **`test_inprocess`** (rmw, two nodes, one context):
    - 10 messages of 60 KB published before any take, at depth 3: the newest 3, intact;
    - a TRANSIENT_LOCAL subscription (depth 2) created after 4 publishes gets the last 2, in order and whole (each
      about 42 fragments, reassembled).
  - **Mutants**, each failing at the predicted assert: delivery removed; delivered twice; the durable backlog not
    delivered locally; the local RxO check removed.
  - **Plan's review before push: no stack buffer the size of a sample.** Local delivery reads from a scratch buffer
    of tt_MAX_SAMPLE_LENGTH that the context owns, since sending reuses tx_buffer. The zero-copy path reads the
    caller's own bytes in place. The one exception is a sample published from inside a local delivery, which takes a
    stack copy of its own, because the scratch still holds the outer sample. The durable backlog reassembles
    fragments in the same scratch.
  - **Found on the way, not g9's:** a durable publisher of a type this large starts with an arena of one sample and
    grows it only once `depth` messages have gone out. At depth 4, after 4 publishes, it retained one sample, so a
    late joiner - local here, remote the same, since both read one cache - got one of 4. Reported to Plan; the rmw
    test uses depth 2, which the growth reaches.
  - Core's default build (`tt_LOCAL_DELIVERY` 0): `tickle.o` and `hal_linux.o` are md5-identical to the parent's
    (control: the flag on differs). clang-tidy over rmw_tickle's compile database (the flags on) is clean.
  - The rmw suite passes in a private netns, 58/58.
  - **In-process cost** (`experiments/g9_local_delivery_cost.c`, mock HAL, -O2, medians of 5 x 200000, 3 repeats):
    - no local Subscriber: 71-75 ns a publish with the flag on or off, so the branch is not measurable;
    - a 64 B sample to one local Subscriber: about +19 ns (91-93 ns);
    - a 60 KB sample: about +1.6 us (10.1-10.6 us against 8.4-9.0 us), mostly the copy.
  - **CPU**, `rmw_lib_ab.sh`, parent against g9, 10 reps interleaved, with an A/A, read as pre-registered (the
    benchmark has no local Subscriber, so it measures the branch). All four rows are inside:

    | gap | metric | B-A median (2 SE) | A/A B-A (2 SE) | verdict |
    |---|---|---:|---:|---|
    | 5 ms | RTT | -7.3 us (6.0) | +3.9 us (9.3) | inside |
    | 5 ms | pong CPU | -2.7 ms (3.8) | +3.4 ms (5.0) | inside |
    | 100 ms | RTT | +2.4 us (9.2) | +0.6 us (16.3) | inside |
    | 100 ms | pong CPU | -1.0 ms (1.4) | -0.5 ms (0.8) | inside |
## Checkpoint run (2026-09-28, Plan): main at `958e909e`, after stage 3, g2, g3, g5 and g8

`rmw_gap_acceptance.sh -w /tmp/plan_accept`, rmw_tickle rebuilt Release at `958e909e`. The script printed the
library's md5 `e282cf1b5f53`; its symbols confirm g2, g5 and g8.

| test | control | rmw_tickle | baseline (2026-09-27) |
|---|---|---|---|
| graph | CycloneDDS PASS | **PASS** | FAIL |
| bag | CycloneDDS PASS | FAIL | FAIL (g1 is still open) |
| events | CycloneDDS PASS | **PASS** | FAIL |
| matched | CycloneDDS PASS | **PASS** | FAIL |
| itype | CycloneDDS FAIL (VOID) | PASS | FAIL |
| takeseq | CycloneDDS PASS | **PASS** | FAIL |
| samehost | CycloneDDS PASS | **PASS** | FAIL |
| range | CycloneDDS PASS | FAIL | FAIL (g6 is still open) |
| peers | CycloneDDS PASS | FAIL | FAIL (g6 is still open) |

- Six gaps are closed, each confirmed by its own acceptance test against the control.
- The three still failing are exactly the open items, g1 and g6.
- `itype` stays VOID. rmw_tickle raises the event, but no vendor does in this setup, so there is nothing to compare
  it against.
- g8's two further cases (8 processes on one host; a forced collision with the registry off) are in
  `rmw_samehost_many.sh`, which uses the same WS and the same CycloneDDS control.

## g9 - nodes in one process (found by Dev 2026-09-28; ahead of g6; acceptance test `inprocess`)

- **Gap:** two nodes in one process never exchange topics. All of a process's nodes share one tt_Context, and core
  drops its own DATA as self-sent, with no in-process delivery path. Composition, component containers, composable
  launch files and every multi-node rclpy or rclcpp process are affected.
- **Baseline** (`inprocess`, one rclpy process with a talker node and a listener node in one executor, then a
  std_srvs/Trigger call between them): CycloneDDS PASS; **rmw_tickle FAIL, topic received 0, but the in-process
  service call succeeds** (service_ok=1). So g9 is about topics; services already work within a process.
- **Pass** (Dev pre-registers the details):
  - `inprocess` passes;
  - QoS as for remote: RELIABLE without loss, KEEP_LAST overflow, and TRANSIENT_LOCAL backlog for a later local
    subscriber;
  - no double delivery when local and remote subscribers coexist;
  - mutants: the delivery removed, delivered twice, and no local backlog;
  - CPU: a publish with no local subscriber costs at most a branch.
- The acceptance overlay now also builds `std_srvs` with TickLE typesupport. Without it the service half fails for a
  reason unrelated to g9.

## g10 - TRANSIENT_LOCAL / KEEP_LAST keeps fewer than depth for large types (found by Dev 2026-09-28; after g9; acceptance test `durable`)

- **Gap:** a durable KEEP_LAST publisher of a large type starts with an arena for one sample and grows only after
  `depth` messages have gone out (`keep_last_wants_more_arena`, rmw_publisher.c). A late joiner, local or remote,
  gets fewer than `depth` samples, where DDS gives `depth`. The durability contract is broken.
- **Baseline** (`durable`: TRANSIENT_LOCAL KEEP_LAST 4, 6 sensor_msgs/Image samples of 60,000 B, a subscriber on the
  other host created afterwards): CycloneDDS PASS (the last 4, in order); **rmw_tickle FAIL, it received 3 (4, 5, 6)**.
  - The first version of the test used 16,000-byte samples and passed on the unfixed build, because the first arena
    already held 4. It could not fail, so it now uses 60,000 bytes, and the reason is in its comment.
- **Pass** (Dev pre-registers the details): `durable` passes.
  - The arena grows on need: a publish never evicts while fewer than `depth` are retained, up to the budget.
  - A budget that cannot hold `depth` x the sample warns once at the point it binds, and counts it. It never retains
    fewer silently.
  - The same check applies to the RELIABLE KEEP_LAST retransmit cache.
  - Mutant: the current growth rule.
- **Design, Dev before code (2026-09-28).** Growth moves to the moment of need, inside core's publish path, where the
  sample's cache footprint is known and nothing is evicted yet.
  - **Core** gains a hook, `tt_Publisher.cache_grow`, a callback that grows the Publisher's arena and says whether
    it did. NULL, core's default, means it never grows.
    - Before a KEEP_LAST sample is cached, core asks whether caching it would evict one of the newest
      `sample_depth - 1` samples for bytes: `reliable_cache_keeps_depth()`, a simulation of the eviction the write
      would do, as `reliable_cache_admits()` does for KEEP_ALL.
    - If it would, core calls the hook until it would not, or until the hook cannot grow any more.
    - If room is still short, the sample is cached as before, and `tt_ReliableCache.depth_shortfalls` counts it.
    - The ordinary path costs a branch (no hook, or room), plus the simulation's few compares when a hook is set.
  - **rmw_tickle** sets the hook to `grow_reliable_cache()`, which doubles toward the budget limit. That is the
    same growth, and the same budget, as today. The publish path already holds the context lock that growth needs.
    - The after-publish rule (`keep_last_wants_more_arena()`: grow only once `depth` messages have gone out) is
      removed.
    - A publisher whose cache falls short logs one WARNING, with the depth it can keep and `RMW_TICKLE_CACHE_BYTES`
      as the setting to raise.
    - The shortfalls are summed on rmw's shutdown line as `cache_depth_shortfalls=`.
  - **RELIABLE KEEP_LAST** uses the same cache and the same path, so its retransmit window gets the same guarantee.
    KEEP_ALL is unchanged: its own admission refuses rather than evicts.
- **Pass (pre-registered):**
  - `durable` passes.
  - Unit test (core, mock HAL):
    - a durable KEEP_LAST 4 Publisher with a growing hook and fragmented samples keeps all 4 of 4 and of 6;
    - with a hook that cannot grow, it keeps fewer and counts every shortfall;
    - no hook: behaviour as before.
  - rmw test (`test_inprocess`, reusing its 60 KB type): depth 4, 6 publishes, then a late durable subscription
    gets the last 4. The test's depth-2 workaround goes.
  - With `RMW_TICKLE_CACHE_BYTES` too small for 4, a late joiner gets fewer, with one WARNING and the shutdown
    count.
  - A RELIABLE KEEP_LAST publisher: its cache retains depth samples of 60 KB after depth publishes.
  - The rmw suite and every acceptance test that passed before.
  - Mutants: the old rule (growth after `depth` sends, no hook call); the hook not called; the shortfall not counted.
  - CPU: `rmw_lib_ab.sh` with an A/A, and the core bench (`core_cost_bench`) for the new branch, since core changes.
- **Result (Dev, 2026-09-28): PASS.**
  - **Acceptance.** `durable` PASS: the late joiner on the other host gets the last 4 of 6 60 KB samples, where the
    baseline got 3.
  - **`test_depth_growth`** (core, mock HAL, fragmented 5000 B samples, an arena that starts with room for one):
    - with a growing hook, 6 publishes at depth 4 leave 4 retained and no shortfall, for TRANSIENT_LOCAL and for
      RELIABLE alike;
    - with no hook, or one that cannot grow, fewer are retained and every short publish is counted.
  - **`test_inprocess`** (rmw):
    - depth 4, 6 publishes of 60 KB: a late durable subscription gets the last 4. The depth-2 workaround is gone.
    - With `RMW_TICKLE_CACHE_BYTES=150000`, it kept 2 of depth 4, logged one WARNING naming the setting, and the
      context counted 4 shortfalls, shown on the shutdown line as `cache_depth_shortfalls=4`.
  - **Mutants**, each failing at the predicted assert:
    - the hook not called (core: retained 1 of 4);
    - a shortfall not counted;
    - rmw setting no hook (test_inprocess: fewer than 4).
    - The old rule, growth only after `depth` sends, is the parent build itself, and it fails `durable` in Plan's
      baseline (3 of 4).
  - The rmw suite passes in a private netns. Every acceptance test that passed before still passes.
  - **Core bench** (`core_cost_ab.sh`, 11 rounds, parent twice as the A/A): medians inside the A/A spread.
    - BEST_EFFORT: send 178.7 ns against 179.8 / 178.7, recv 51.3 against 51.5 / 51.6.
    - `-R`: send 233.7 against 233.6 / 234.8, recv 80.8 against 78.6 / 78.8, with spreads of 19-30 ns.
    - The bench's publisher sets no `sample_depth`, so this measures the branch.
  - **rmw CPU**, `rmw_lib_ab.sh` with the ping-pong in `--reliable` mode (KEEP_LAST 8, so every publish goes
    through the new check; the default BEST_EFFORT bench has no cache), parent against g10, 10 reps interleaved,
    with an A/A. All four rows are inside:

    | gap | metric | B-A median (2 SE) | A/A B-A (2 SE) | verdict |
    |---|---|---:|---:|---|
    | 5 ms | RTT | -0.7 us (6.6) | +4.6 us (8.4) | inside |
    | 5 ms | pong CPU | -0.4 ms (4.5) | +2.2 ms (4.8) | inside |
    | 100 ms | RTT | +12.5 us (17.8) | -7.6 us (15.8) | inside |
    | 100 ms | pong CPU | -0.4 ms (0.8) | -0.1 ms (1.4) | inside |

    (`rmw_lib_ab.sh` gained `PINGPONG_ARGS` for this; its default is unchanged.)

## Checkpoint run 2 (2026-09-28, Plan): main at `e14d4439`, after g2, g3, g5, g6, g8, g9 and g10

`rmw_gap_acceptance.sh -w /tmp/plan_accept`, rmw_tickle rebuilt Release at `e14d4439`. `range` now also has the OFF arm:
nothing between processes, CycloneDDS's observed behaviour.

| test | control | rmw_tickle | baseline (2026-09-27/28) |
|---|---|---|---|
| graph | CycloneDDS PASS | **PASS** | FAIL |
| bag | CycloneDDS PASS | FAIL | FAIL (g1 is still open) |
| events | CycloneDDS PASS | **PASS** | FAIL |
| matched | CycloneDDS PASS | **PASS** | FAIL |
| itype | CycloneDDS FAIL (VOID) | PASS | FAIL |
| takeseq | CycloneDDS PASS | **PASS** | FAIL |
| samehost | CycloneDDS PASS | **PASS** | FAIL |
| inprocess | CycloneDDS PASS | **PASS** | FAIL |
| durable | CycloneDDS PASS | **PASS** | FAIL |
| range (LOCALHOST, SUBNET, OFF) | CycloneDDS PASS | **PASS** | FAIL |
| peers | CycloneDDS PASS | **PASS** | FAIL |

**Every gap on the list is closed except g1** (serialized messages, with large-message stage 1), each confirmed by its
own acceptance test against the control. SECURITY_PLAN stays parked, at the user's word.

## g11 - one wrong-version datagram ends a node's poll loop (found 2026-09-28; HIGH; a remote denial of service)

- **Gap:** `check_version()` returns false for a wire version it does not know (tickle.c:9656-9660); its caller turns
  that into `tt_RET_PROTOCOL_ERROR` (tickle.c:9916-9917); and every poll loop we ship stops on anything but OK or
  TIMEOUT, e.g. `reliable_throughput/server.c:344`. So **one UDP datagram carrying a different version byte ends the
  receiving node**, before any discovery, matching or authentication.
- **Observed on the rig, 2026-09-28 14:49:31.** A v11 server received one v10 datagram from a leftover v10 process:
  `Illegal version from node 2: 10 != 11`, `Cannot process packet`, then its traffic summary and `RESULT: recv=0`, in
  the same second, with `-d 20` on its command line. Its peer then timed out with nothing to match, which is what
  voided that measurement.
- **Why it matters:**
  - anyone able to send a datagram to the well-known port can stop a node, with no state and no handshake. It belongs
    in SECURITY_PLAN's threat list, though that plan stays parked;
  - a rolling upgrade is impossible: one old node takes down every new one, and the wire version has been bumped
    three times this week;
  - it can void a measurement silently, as it did here.
- **Pass** (Dev pre-registers the details):
  - an unknown wire version is counted (`version_mismatch_drops`) and dropped, the rate-limited log stays, and the
    poll returns OK so the loop continues;
  - a whitebox peer sends a wrong-version datagram, then a valid one: delivery continues and the count is 1;
  - the same for a malformed submessage, and for every other path that returns `tt_RET_PROTOCOL_ERROR` from a
    received datagram - a remote peer must not be able to end a local loop;
  - a mutant that keeps the fatal return fails those tests;
  - on the rig, a v10 sender beside a v11 pair does not stop them.

### The rule this establishes

**A received datagram must never make `tt_Context_poll()` return an error.** Whatever a peer sends - a version we do
not speak, a bad magic, a truncated header, a submessage that does not walk - is a dropped datagram with a counter,
not a fatal return. An error return is for this node's own failures: an encode that overflows, a socket that breaks.
The five other `tt_RET_PROTOCOL_ERROR` sites in `tickle.c` are all on the send path (`data_encode_size` out of
range, `data_encode` failing, and so on), which is the caller's own error and stays as it is.

### The change

- `validate_packet_header()` counts `version_mismatch_drops` on a version mismatch, keeping its existing
  rate-limited log.
- `process_datagram_locked()` counts `rx_malformed_drops` - every datagram dropped as unprocessable, of which
  `version_mismatch_drops` is the named subset - and returns `tt_RET_OK`.
- Both counters join the rmw shutdown line, beside `rx_out_of_range`.

### Pass criteria, fixed before the code

1. **The unit contract.** `process_datagram()` returns `tt_RET_OK` for each of: a wrong wire version, a bad magic, a
   datagram shorter than a header, and a submessage length that runs past the datagram. The right counter moves by
   exactly one in each case, and no other counter does.
2. **The loop does not stop.** A poll whose first datagram is one of those, with a backlog behind it, drains the
   backlog (`test_mock_try_receive_remaining` reaches 0, `rx_datagrams` counts every one) and returns OK. This is
   the property the rig lost.
3. **Delivery afterwards is unaffected.** A valid datagram after a hostile one is processed in full: its source's
   `traffic_last_seen` is stamped, where the hostile one's is not.
4. **Mutants, each of which must fail one of the above:**
   - `fatal_version`: the version mismatch returns `tt_RET_PROTOCOL_ERROR` again - fails 1 and 2.
   - `silent_drop`: the drop is not counted - fails 1.
   - `fatal_malformed`: only the version case is made non-fatal, a truncated datagram is still fatal - fails 1 and 2.
5. Acceptance: Plan's `v10 sender beside a v11 pair` case, and every test that passed before still passes.

### Result (Dev, 2026-09-28)

Fixed in core: `validate_packet_header()` counts `version_mismatch_drops`, `process_datagram_locked()` counts
`rx_malformed_drops` and returns `tt_RET_OK`, and both join the rmw shutdown line.

`tests/test_hostile_datagram.c` runs all four hostile shapes - wrong wire version, bad magic, truncated header,
overlong submessage - through each criterion. Against the unfixed code every one of them failed: `process_datagram()`
returned -3, and a poll whose first datagram was hostile left 3 of its 4 queued datagrams unread, which is the rig's
failure reproduced in-process. Afterwards all pass, and the other 41 unit programs still do.

Mutants, each killed by the criterion it was aimed at: `fatal_version` (criteria 1, 2 and 3), `silent_drop`
(criterion 1 only, as intended), `fatal_malformed` (criteria 1, 2 and 3).

One thing the test records rather than changes: a datagram whose header validates but whose submessages do not
*does* stamp its sender's liveliness, because `process_packet()` stamps between the two. The header proves a peer
speaking this wire version is transmitting, which is what that evidence claims. It is now asserted per case, so a
later change to it has to be deliberate.

Not affected, checked rather than assumed: rmw_tickle's own poll thread ignores the return, so no ROS node ever
stopped on this. What stopped was every program written the way `examples/perf_hil/tickle`'s fourteen poll loops
are written.

## g12 - two packages with a same-named message share one codec symbol (found by Dev 2026-09-28; HIGH; shipped defect)

- **Gap:** the ROS typesupport generator names the TickLE struct and its codec `<Name>Data`, with no package in the
  name (`adapt_message`, mirrored by `Ros2Resolver`). Two packages with a same-named message therefore define the same
  global C symbols, and the dynamic linker serves whichever library loaded first to both. rosidl's own generators
  qualify every symbol by package for exactly this reason.
- **Found by pass 1's harness**, which loads every package of the inventory at once. `check_ros2_interfaces.sh` loads
  one package at a time, which is why this survived the whole P2 standard-message effort.
- **Observed:** `actionlib_msgs/GoalStatus` encoded 25 B whose bytes 8-15 and 17-24 are heap pointers - which is
  `action_msgs`' fixed-size `GoalStatus` encoder (uuid[16] + Time + int8 = 25 B) copying the first 25 bytes of the
  other type's memory, `char*` fields and all. Confirmed three ways: `nm -D` shows `GoalStatusData_encode` defined `T`
  in both libraries; the harness passes on actionlib_msgs alone and fails as soon as action_msgs is loaded beside it;
  and a message ending in a string can only encode to a multiple of 4, which 25 is not.
- **Scope: 31 colliding symbols in the inventory.** std_msgs and example_interfaces share 28 message names,
  diagnostic_msgs/KeyValue collides with type_description_interfaces/KeyValue, and actionlib_msgs and action_msgs share
  GoalStatus and GoalStatusArray. The 29 with identical layouts have been silently interchangeable; the two GoalStatus
  ones differ, so an application using both packages puts **garbage on the wire today, with no error** - a defect in
  shipped rmw_tickle, not in the harness.
- **It also discloses addresses.** Those pointer bytes are ASLR'd heap addresses sent to every peer on the topic, which
  weakens ASLR for anyone listening. Same class as the padding leak fixed this morning, and larger. It belongs in
  SECURITY_PLAN's threat list when that plan starts.
- **Fix (Dev):** qualify the ROS path's generated names as `<pkg>__<subfolder>__<Type>Data`, and `...Request` /
  `...Response` for a `.srv`. TickLE's own core generator keeps `<Name>Data`, because its users write those names by
  hand.
- **Pass, as Dev pre-registered it:** no `Data_encode` symbol is defined by two libraries of the inventory, checked with
  `nm` in the CI step; the pass-1 harness is green over `--all`; the acceptance suite still passes.
- **Pass, added by Plan:**
  - **the wire bytes must not change** for any type that had no collision. The rename touches names, not encodings, so
    for every inventory type the post-fix encoding is compared byte for byte against the **pre-fix** build's, not only
    against the old path within one build. A mutant that also changes an encoding must fail this.
  - **services and actions are covered too,** with their own symbol check: a `.srv` pair and the six implicit
    interfaces a `.action` generates are where a collision is easiest to miss.
  - **the 29 latent collisions are asserted to be layout-identical** before the fix, so the claim "they were
    interchangeable by luck" is checked rather than assumed - and after the fix each has its own codec.
  - **the rebuild requirement is in the CHANGELOG:** generated symbol names change, so every interface package must be
    rebuilt, as with this morning's callbacks `struct_size` change. Anything naming `<Name>Data` on the ROS path fails
    to compile, which is the safe direction.

### Result (Dev, 2026-09-28)

**Measured before the fix, as criterion 3 asked.** 31 type names were defined by two packages each. 29 were
layout-identical, so they had been interchangeable by luck: `Bool`, `Byte`, `ByteMultiArray`, `Char`, `Empty`,
the `Float`/`Int`/`UInt` family and their `MultiArray` forms, `KeyValue`, `MultiArrayDimension`,
`MultiArrayLayout` and `String`. Two were not: `action_msgs/GoalStatus` is `{GoalInfo, int8}` where
`actionlib_msgs/GoalStatus` is `{GoalID, uint8, string}`, and their `GoalStatusArray` likewise. Counting every
exported symbol rather than type names, 144 collided.

**The change.** The ROS 2 path's generated symbols are package-qualified: `<pkg>__<subfolder>__<Type>Data`,
and `<pkg>__srv__<Name>Request`/`Response` for a service. `TopicIR` and `ServiceIR` gained a `header_name`, so
a generated file keeps its own name while its symbols carry the package - the include and the symbol base were
one string before. TickLE's own generator passes no `symbol_base` and is unchanged, because its users write
those names by hand.

**Criterion 1, the wire bytes.** All 287 types in the inventory and the test packages encode identically
before and after, on both paths, at seed 7 - compared by a per-type FNV-1a over every encoding, which the
harness now prints as `old_hash`/`new_hash`. Zero differences. A change that altered an encoding rather than a
name would move a hash.

**Criteria 2 and 4, the symbol check.** `rmw_tickle/scripts/check_interface_symbols.sh` counts strong
definitions across every interface library - messages, services and an action's implicit interfaces alike -
and refuses a workspace where two packages define one symbol. Weak symbols (C++ template instantiations) and
TickLE core's own `tt_` symbols are excluded, with the reason in the script. Its control: the pre-fix build
fails it with 144 collisions; the fixed build passes with 4,013 symbols across 44 libraries. Check all runs it
before the harness, since a collision makes every later result meaningless. The CHANGELOG says every interface
package must be rebuilt.

**One thing to know when rebuilding.** The generator's own sources became `DEPENDS` earlier today, but CMake
picks that up only on a fresh configure: a build tree configured before it regenerates nothing when the
generator changes, and produces a mix of old and new names that does not compile. `--cmake-force-configure`
once, or a clean build, which is what CI does anyway.

## g13 - a subscription cannot be KEEP_ALL, so `ros2 bag record` cannot subscribe (found by Dev 2026-09-28; **closed 2026-09-29**)

- **Gap:** `rmw_tickle_validate_qos_profile()` refuses `RMW_QOS_POLICY_HISTORY_KEEP_ALL` on a subscription, calling it
  "an unbounded queue". `ros2 bag record` subscribes with KEEP_ALL, so with g1's entry points in place and the domain
  default fixed (`a34979e7`) the recorder now gets as far as `rmw_create_subscription` and stops there. A publisher's
  KEEP_ALL has been accepted since 2026-09-25, bounded by `RMW_TICKLE_KEEP_ALL_BYTES` (512 KB default, env override) -
  only the reader side still refuses.
- **The premise to correct first: KEEP_ALL does not mean an unbounded queue.** In DDS, HISTORY KEEP_ALL is bounded by
  RESOURCE_LIMITS, and what distinguishes it from KEEP_LAST is not capacity but **what happens when the cache is full**:
  KEEP_LAST overwrites the oldest unread sample, KEEP_ALL refuses to, and a RELIABLE writer is held back instead. So the
  policy TickLE cannot honour is not "keep everything" - it is "never destroy an unread sample" - and that one is
  implementable in bounded memory, which is the whole reason to accept it.
- **Decision (Plan, 2026-09-28): accept KEEP_ALL on a subscription,** with these semantics:
  - **a bounded capacity**, sized by a byte budget in the publisher's own idiom (`RMW_TICKLE_KEEP_ALL_BYTES`' reader
    counterpart, its own name, its own env override, the derivation documented where the publisher's is);
  - **no overwrite.** Full queue plus an arriving sample must not evict a sample the application has not taken;
  - **back-pressure under RELIABLE.** The arriving sample is not accepted *and not acknowledged*, so the writer
    retransmits it - which is the hold-back DDS produces, reached through the tracking bitmap that already exists;
  - **BEST_EFFORT drops the arriving sample and counts it,** which is also what DDS does: BEST_EFFORT KEEP_ALL still
    loses samples once the cache is full. The count must be readable and logged, never silent.
  - **Never claim an unbounded queue.** Document the failure mode this buys: a reader that stops taking stalls its
    RELIABLE writers. That is what KEEP_ALL means, and it is the behaviour rosbag2 is asking for.
- **Verify the premise the design rests on, before writing the implementation.** The back-pressure arm assumes that a
  sample left unmarked in the tracking bitmap is retransmitted by the writer. Show that with a test, not a reading of
  the code (this plan's own rule, and it has caught two careful readings already). If it does not hold, say so and fall
  back to dropping the *arriving* sample with a visible counter - the no-overwrite guarantee survives, the no-loss one
  does not, and criterion 5 then has to prove zero loss from the counter rather than from the semantics.
- **Premise result (Dev, 2026-09-28): it holds behind a gap and fails in order, which changes the design.** Three
  pre-registered arms in `tests/test_reliable_pubsub.c`, with a control, and each assertion mutation-checked rather
  than trusted for being green.
  - **Arm A holds.** A sample arriving *ahead of a gap* can be declined, and core already owns the move:
    `hold_for_reorder()` clears the bit when it cannot hold a sample and calls it un-receiving, with its own comment
    that leaving the bit set and dropping the payload is "the one outcome a RELIABLE reader must never produce". The
    test drives a reader with no reorder buffer, declines a sample behind an open gap, and shows the watermark held,
    the bit clear, an ACKNACK sent, and both samples delivered once they come back.
  - **Arm B fails.** The same refusal applied to a sample arriving *in order* cannot be made at all.
    `deliver_data_to_subscriber()` calls `update_reliable_ack()` before the subscriber callback, so the watermark is
    already past the sample by the time anything above core sees it, and the writer is entitled to forget it. The
    refusal is also final: the same `seq_no` arriving again is recognised as already delivered and skipped, so the
    callback never fires for it. On a healthy link every arrival is in order, so this is the normal case a full
    KEEP_ALL queue meets, not a corner of it.
  - **Control.** The same stream with nothing declined delivers every sample and sends no ACKNACK, so arm A's ACKNACK
    was arm A's doing.
- **Consequence: the stated fallback is not available to the RELIABLE arm.** Dropping the arriving sample and counting
  it keeps the no-overwrite guarantee and loses the no-loss one, and criterion 5 asks for a zero loss counter on the
  `bag` acceptance. Both cannot be true at once, so for RELIABLE the fallback is a different feature, not a smaller
  version of this one.
- **Design correction, pre-registered before the code: an opt-in accept hook on `struct tt_Subscriber`.** Consulted
  in `deliver_data_to_subscriber()` **before `update_reliable_ack()`**, beside the existing RxO-incompatible drop;
  returning false means the sample is simply never received. The first draft of this put the hook after the watermark
  advanced and rolled it back, and reading `update_reliable_ack()` closely killed that: its own tail calls
  `maybe_arm_acknack_retry()`, which can send an ACKNACK carrying the freshly advanced `ack_seq_no` before the
  function has even returned. An ACKNACK reports the watermark, and a watermark implicitly acks everything below it,
  so the rollback would have had to undo something already on the wire. Declining earlier removes the instant rather
  than reasoning about it: no bit is set, no watermark moves, and nothing can report what never happened. Core already
  states this is the right place - the RxO drop immediately above carries the comment that an incompatible
  Publisher's DATA is dropped "before any reliable-tracking side effects too (`update_reliable_ack()` below), not
  just before delivery". Recovery then needs no new machinery: `ack_seq_no` stays at the declined `seq_no`, the next
  arrival is out of order against it, and the ordinary gap path requests the declined sample back.
  The hook is additive (the existing callback typedef keeps its `void` return and no current caller changes), it is
  generic flow control with no ROS in it (`project_core_no_ros_dependency`), and it introduces no second kind of
  un-receive. BEST_EFFORT keeps drop-and-count as planned.
  - **Controls:** a subscriber with no hook set must behave exactly as today (the existing suite is that control), and
    a hook that always accepts must be indistinguishable from no hook at all.
  - **Mutants, run and caught (Dev, 2026-09-28):** (a) consult the hook *after* `update_reliable_ack()` - this is the
    discarded design, kept as a mutant because it is the mistake a reasonable person actually makes here rather than
    one invented for the purpose. It is caught three times over: the declined sample's watermark advances, its bit is
    set in the tracking bitmap so it is never named as missing, and the recovery arm delivers the wrong sample. (b)
    count the decline but do not honour it - the declined sample is handed up anyway, caught by six assertions across
    both arms. Both revert green.
  - **Two mutants from the first draft do not apply to the final placement, said rather than quietly dropped:**
    "decline but set the bit" and "decline a sample below the watermark" were both written against the rollback
    design. With the hook consulted before any tracking there is no bit to set, and a below-watermark arrival that is
    declined is simply not delivered - it cannot be requested again, because the watermark is already above it and no
    gap names it, so there is no double-delivery hazard to mutate. The load-bearing property is the hook's position,
    and (a) is the mutant that tests it.
  - **The reader's capacity is capped at 4096 entries** on top of the byte budget, deliberately: a 24-byte message
    against the 512 KiB default divides out to roughly twenty thousand entries, which is a queue nobody drains. When
    the cap binds rather than the budget, a reader that wanted more unread samples than that meets back-pressure
    earlier than its byte budget implied - which is a smaller surprise than the queue, and is why the cap is not a
    tuning knob: `RMW_TICKLE_READER_KEEP_ALL_BYTES` is the number with a meaning.
  - **The full-queue branch under KEEP_ALL counts and warns once** (Plan, 2026-09-29). It should be unreachable - the
    hook declines before core records anything, and `rmw_take()` only makes room - so it drops the arriving sample
    rather than evicting an unread one. But if the hook is ever not consulted, that branch is silent loss of a sample
    the writer was told was delivered, which is the single thing g13 promises never to do. An instrumented
    unreachable branch says it was reached; a commented one says nothing.
  - **Failure mode to document where the hook is declared:** a reader that never accepts stalls its RELIABLE writers.
    That is what KEEP_ALL means and what rosbag2 is asking for, but it must be stated at the hook, not only here.
  - **That nothing reports a declined sample is itself a claim, so it is tested, not stated** (Plan, 2026-09-28).
    Plan asked for an arm where a decline is attempted after the watermark has been reported, refused or impossible
    by construction rather than merely unlikely. It is impossible by construction, and the test says so directly:
    with the hook consulted before any tracking, the reader sends no ACKNACK naming the declined `seq_no` and never
    raises its watermark past it - asserted over a stream with an open gap elsewhere, which is the case that would
    otherwise have sent one. The arm that made the first design fail is kept as mutant (a): move the hook after
    `update_reliable_ack()` and the declined sample is acked, which this test must catch.
  - **The stall is observable or it is not shipped** (Plan, 2026-09-28): a counter and a one-time warning, plus a test
    that the writer actually backs off rather than spinning. A reader that never drains stalling its writer is what
    RELIABLE plus no-overwrite means, and whoever meets it must be able to read what happened.
  - **It costs nothing when unused** (MODULE_PLAN decision 6): a build with the hook present but no subscriber setting
    it must be indistinguishable from the parent, read against a placement control per WIRE_PLAN 8.3's amendment
    rather than from `cpu_s` alone. If a cost does appear, `sched_cpu_s`/`sched_by_thread=` in the RESULT line
    (`7698f669`) is the instrument to attribute it with.
    **This is a measurement and cannot be argued from the source** (Plan, 2026-09-29). "A KEEP_LAST subscription sets
    no hook, so nothing on its path changed" is true of the source and of the function bodies, and says nothing about
    the binary: adding `subscriber_accept()` and the KEEP_ALL sizing grows the translation unit, function addresses
    move, and a hot function can land differently against a cache line, the loop buffer or branch-predictor aliasing.
    That is precisely the mechanism now standing for the p1 1.1% (WIRE_PLAN 10.3): `5d9cace1`'s own -O2 comparison
    found every receive and publish function byte-identical, with three differing in alignment NOPs alone, and it
    still costs -1.061% at t = -16.45. Byte-identical bodies are not a null result - they are the signature of the
    thing that cost a percent. So the construction argument is struck, and the placement-controlled reading is the
    criterion.
- **Pass, pre-registered:**
  1. **Matching.** A KEEP_ALL subscription is created without error and matches a KEEP_LAST publisher. HISTORY is a
     local policy, not a requested/offered one, so this must hold - checked against the CycloneDDS control arm in the
     same test, so the claim rests on an observation and not on Plan's reading of the spec.
  2. **No overwrite.** Capacity C, publish C+K samples with nothing taken, then take C times: the C samples taken are
     the **first** C published. Mutant: restore the evict-the-oldest branch - this test must fail.
  3. **Back-pressure, RELIABLE.** A writer sending faster than the reader takes, capacity small: samples delivered
     equals samples sent, and the writer's send rate falls. Mutant: acknowledge the refused sample anyway - loss must
     appear.
  4. **BEST_EFFORT.** Overflow drops the arriving sample, the counter is non-zero and the log says so. A run that loses
     samples with a zero counter fails.
  5. **`bag` acceptance** (g1's criterion 5): `ros2 bag record` on a topic published from the other host records N
     messages; the bag holds N, each one's content matches what was sent, and the loss counter is zero. CycloneDDS runs
     the same test first as the control.

     **Amended 2026-09-29, on Dev's finding, because the unamended criterion could not be met by any implementation.**
     A KEEP_ALL reader's zero loss is bounded by the **writer's** retained depth, not by anything the reader controls.
     The chain: the reader declines, `ack_seq_no` stays put, the gap logic asks for the declined sample, and the writer
     retransmits it *if it still holds it*. If the reader declines for longer than the writer's cache retains, the
     writer answers with Phase 1-c's eviction Heartbeat, the reader skips the sample, and it is gone -
     `test_process_acknack_skips_expired_sample` already pins that path and `tt_Subscriber.gap_evicted` already counts
     it. Against an ordinary KEEP_LAST publisher of depth 10 - which is what `ros2 bag record` meets in the field - a
     recorder that stalls for more than ten samples loses them, and back-pressure does not save it, because a KEEP_LAST
     writer does not block, it evicts.

     So criterion 5 becomes three things rather than one:
     - **it names the publisher's QoS**, because the claim is about a pair and not about the reader;
     - **zero loss is required where the writer retains enough** (a KEEP_ALL publisher, or KEEP_LAST with depth above
       the reader's stall): the bag holds N, contents match, and `gap_evicted` is 0;
     - **where the writer does not retain enough, loss is permitted but must be counted and must not exceed the
       control's.** `gap_evicted` non-zero and reported, and the same stall against CycloneDDS losing at least as much.

     **Result of the stall arm (Dev, 2026-09-29).** A recorder SIGSTOPped for 8 s behind an ordinary KEEP_LAST depth-10
     talker; loss is published minus recorded, taken from rosbag2's own output on **both** sides so the two arms are
     measured the same way. Five pairs:

     | | runs | mean lost | SE | spread |
     |---|---|---:|---:|---:|
     | `rmw_cyclonedds_cpp` (control) | 39, 54, 59, 65, 72 | 57.8 | 5.6 | 33 |
     | `rmw_tickle` | 37, 39, 39, 39, 39 | **38.6** | 0.4 | 2 |

     **Both lose, so this is DDS semantics and not a TickLE shortfall** - the first branch of the criterion. And within
     that, TickLE loses **33% fewer samples**, difference 19.2 at t = −3.4, with a spread of 2 against the control's 33.
     `gap_evicted` was 13-16 against a loss of 37-39 and the two are reported side by side rather than one standing for
     the other: the counter counts what the writer said it no longer held, and the rest went while the reader was
     stopped and its window moved past. The control reports no equivalent at all.

     **A sixth pair, 2026-09-29: the control lost 78 where we lost 39.** The figures above are not updated on one more
     pair - that is the rule, not an exception made here - but it widens the control's observed range to 39-78 across six
     runs while ours stays 37-39. So the caveat is stronger rather than weaker: **the control's spread is now nearly the
     size of its own mean**, and any future comparison at this cell needs its repetition count set by that, not by ours.

     **Caveats, because a single-condition advantage is not a general one:** n = 5, one stall duration, one publisher
     depth. And the methodological point Dev found on the way is worth more than the number: **the first pair came out
     39 against 39** and would have been written up as "level", while the second pair alone would have claimed 20
     better. The control's own spread is 33. That is WIRE_PLAN 8.3's confirmation rule arriving from the other side -
     one pair is a candidate, not a finding - and the first time it has been *the control* that is the noisy arm.

     **The control is what decides whether this is a gap at all**, and it must be run before the plan says which:
     a DDS reader behind a KEEP_LAST writer of depth 10 should also lose samples when it stalls, because the writer
     evicts and the reader gets a GAP - in which case TickLE is level, this is DDS semantics rather than a TickLE
     shortfall, and the incomplete-delivery rule is satisfied by the counter. **If CycloneDDS loses nothing where we
     lose, that is a real gap and gets its own number** - it would mean its writer retains more than the QoS says, and
     we would have to decide whether to match it. Either way the answer comes from the control arm, not from our
     reading of the spec.
  6. **No regression.** The KEEP_LAST path keeps its behaviour (the existing subscription and QoS tests pass unchanged),
     and p1 shows no WORSE row that survives WIRE_PLAN 8.3's confirmation rule.

     **Amended 2026-09-29 (WIRE_PLAN 10.4): this criterion has a floor, and the verdict is three-valued.** g13 grows the
     translation unit, which is precisely the case the floor was measured for - a semantically neutral alignment flag
     moved p1 throughput by 0.859% and reversed the sign of a 0.6% difference. So the reading is *better*, *WORSE*, or
     **"below the instrument's floor"**, and the third is not the first. "Satisfied by construction" is not available
     either: source-identical functions still move when the binary grows, which is what 10.3 and 10.4 are about. If the
     KEEP_LAST reading comes back under 1% at p1, the honest report is the floor, not the number and not its sign.
- **Criterion 3/6, "costs nothing when unused", answered exactly rather than statistically (Dev, 2026-09-29).** Both
  instruments that would have answered it have a floor at roughly this size (WIRE_PLAN 10.3/10.4), so the claim is
  carried on the figures that are deterministic. Measured by compiling the same size program against the parent's
  headers (`a073a6fc`, the commit before the accept hook) and against the current ones:

  | | parent | with g13 | cost |
  |---|---|---|---|
  | `struct tt_Subscriber` | 1216 | 1240 | **+24 B per subscriber** |
  | `rmw_tickle_subscriber_t` | 2448 | 2480 | **+32 B per subscription** |
  | `struct tt_Publisher` | 536 | 536 | 0 |
  | `rmw_tickle_publisher_t` | 1312 | 1312 | 0 |
  | `struct tt_Context` | 74008 | 74008 | 0 |
  | every wire struct (`tt_Header`, `tt_DataHeader`, `tt_AckNackHeader`, `tt_HeartbeatHeader`, `tt_SubmessageHeader`, `tt_UpdateEntity`) | - | - | **0** |

  So a publisher pays nothing, a context pays nothing, the wire pays nothing, and a subscription pays 32 bytes whether
  or not it is KEEP_ALL. The per-sample cost on the KEEP_LAST path is one NULL test on `accept_callback` before
  delivery, and nothing else: no allocation, no lock, no wire byte.
- **Falsification run (Plan's amendment, threshold 3%): not falsified.** Ran `p1_layout_check.sh a073a6fc 04fea91a 3`
  on the rig, four arms, six runs each. Only the pre-registered verdict is reported, and no figure is offered as a
  cost or a gain:

  the unforced pair moved by **0.659%** in magnitude, which is below the 3% threshold, so criterion 6 is **not
  falsified** and the exact evidence above carries the claim. Nothing was found that one extra branch and 32 bytes
  cannot explain.

  Two things about the run that have to be said plainly rather than smoothed over. Its own printed verdict is
  **VOID**, and that is correct *for its own question* - it exists to ask whether an earlier 1.1% loss is layout, and
  the unforced pair here does not reproduce that 1.1%, so it says nothing about that. It is not void for this
  question: this question is "is the difference 3% or more", the unforced pair is exactly the comparison that answers
  it, and six runs an arm with SEs of 2000-4400 on 3.7M samples answer it clearly. Two questions, one harness, and
  only one of them void.
  And the direction is deliberately not reported above, because reporting it would be the mistake WIRE_PLAN 10.4
  warns about: the alignment flag alone moved one arm by 0.472%, the same order as the whole difference, so whichever
  way it points it is layout, not g13. A sign that means nothing reads as news when it is favourable.

- **Why the timing and RSS readings are not quoted as numbers.** 100 subscriptions of the structural cost above is
  3.2 KB, and the peak-RSS floor is about 10 KB for two builds whose code differs in size - so the RSS instrument
  cannot resolve this cost even in principle, and a figure from it would be layout, not g13. The same holds for p1
  throughput: g13 grows the translation unit, an alignment flag alone moved p1 by 0.859% and reversed the sign of a
  0.6% difference, and the real per-sample delta here is one predictable branch. A placement-controlled p1 run would
  therefore produce a number below its own floor **by construction**, which is not a result. The exact figures above
  are the claim; the run can be made if the measurement is wanted on record, but its reading is known in advance and
  that is the reason it has not been made.
- **A size comparison is as easy to get wrong as a timing one.** The first attempt here substituted the parent's rmw
  header but compiled it against the *current* `tickle.h`, so `rmw_tickle_subscriber_t` came out unchanged at 2480 -
  a null result that was really the parent struct measured with the new core inside it. Both header sets have to move
  together. Same family as the rest of this section: the check ran, gave an answer, and the answer was about
  something else.

- **Why this is worth a gap of its own rather than a line in g1:** rosbag2 is not the only KEEP_ALL subscriber - it is
  the idiom for any consumer that must not lose messages - and under the incomplete-delivery rule a recorder that
  silently drops is a LOSE, not a partial win. The semantics above are what make the difference visible.

## Known gap in the local gates: rmw behaviour is only tested in CI (recorded 2026-09-28)

`make check-gates` is core-only and deliberately cheap. rmw_tickle's own ctest suite cannot join it as it stands: it
needs the whole colcon workspace (rmw_tickle, both typesupport packages and the generated interface overlay, with
`AMENT_PREFIX_PATH` set) and it has to run inside the `dev_rmw_tests` netns with a default route on `lo`. So **a change
to rmw_tickle that makes an rmw test stale is green locally and red in CI**, which is what happened on 2026-09-28: g1
implemented two serialized entry points, `test_unsupported_entry_points` still asserted they were unsupported, and
`Check all` stayed red for three commits.

Two answers were considered and one taken. A separate `make check-rmw` target assuming an already-built workspace was
offered by Dev and declined: a gate that only helps when someone remembers to run it is not a gate. Instead the
specific class was closed statically - `make check-unsupported-list` (`9f7f40f6`) cross-checks
`test_unsupported_entry_points.c` against `rmw_unsupported.c` in both directions from their text alone, with no build,
and runs in `check-gates` and in `Check all`.

**What stays open:** every other rmw behaviour test - events, graph, QoS, discovery, the acceptance suite - is still
CI-only locally. The lesson to carry rather than rediscover: when a change moves an entry point between supported and
unsupported, or changes what a test asserts about rmw behaviour, the CI run is the first real check, so watch that run
rather than the local gate table. Whoever next wants this closed properly should cost out running the ctest suite
against a pre-built workspace in the netns, and decide whether it belongs in `check-gates` or in a pre-push hook.

## g14 - we reported a QoS profile we would not accept (found by Dev 2026-09-29 through g13's `bag`; HIGH; shipped defect)

- **Gap:** `rmw_get_publishers_info_by_topic()` filled RELIABILITY and DURABILITY for a discovered endpoint and left the
  rest of the profile at `rmw_qos_profile_unknown`. rosbag2 records the profile it is told and offers it back on
  playback, and `rmw_tickle_validate_qos_profile()` refuses UNKNOWN - so playback of our own recording failed with
  "Ignoring a topic '/accept_chatter' ... rmw_tickle only supports RMW_QOS_POLICY_LIVELINESS_AUTOMATIC /
  MANUAL_BY_TOPIC" (`rmw_qos.c:109`). **We told a tool a profile and then refused it when the tool repeated it back.**
- **How it surfaced:** g13's `bag` acceptance, first run after the KEEP_ALL fix: `recorded` went 0 -> 73, so the
  recorder worked, and `replayed` stayed 0. The two bags side by side say it plainly - ours history unknown, liveliness
  unknown, deadline 0; CycloneDDS keep_last/10, automatic, infinite.
- **Why it is its own number and not part of g13.** g13 is about a reader's history policy. This is an asymmetry between
  what rmw_tickle *announces* and what it *validates*, it was in shipped code before g13 existed, and it breaks any tool
  that round-trips a reported profile - rosbag2 is the one we happened to run.
- **Fix (Dev, 2026-09-29):** report what is actually announced and participates in matching - LIVELINESS and both
  durations - rather than leaving them UNKNOWN. A wire 0 maps to `RMW_DURATION_INFINITE`, because 0 means "no
  requirement" on the wire while a literal zero reads as a deadline of no time at all; the control's bag reports the
  same infinity, so that mapping is **measured against CycloneDDS rather than chosen**. HISTORY and LIFESPAN are
  genuinely not announced, stay UNKNOWN, and did not block playback.
- **Pass, and the reason it is stated as an invariant rather than as a policy list:** `test_reported_qos.c` hands every
  profile rmw_tickle reports straight back to rmw_tickle's own validator, over three endpoint shapes, and requires it to
  be accepted. A per-policy assertion would have to be extended by hand every time a policy is added or announced;
  the invariant covers policies nobody has thought of yet. The profiles are printed, not only asserted, so a failure
  says what was compared.
- **The general rule this is an instance of, worth applying past QoS:** *any value we report through an introspection
  API must be one we would accept back.* `rmw_get_*_info_by_topic`, the graph APIs and the event payloads all hand data
  to tools that may return it, and a reported value we refuse is a defect even when the reporting is "only
  informational". Plan should check the other introspection surfaces against this before the next COMPARISON re-measure.
- **Result:** `bag` passes and is level with the control - rmw_tickle replayed 77, `rmw_cyclonedds_cpp` replayed 77,
  zero gaps on both - against FAIL(recorded=0) the day before, so the pre-registered "expected to fail before the fix"
  is on record and the test can fail.

### A test-shape defect that happened three times in one night, recorded once

In `test_keep_all_reader`, in the `bound` test and again in `test_reported_qos`, a pre-registered mutant failed **before
reaching the claim it was aimed at** - on a specific per-policy or per-decline assertion several lines above the general
property the file exists to pin. Each time the suite was green and the mutant failed, which together look like proof and
are not: the check answered, but it was not the thing being checked that answered. The fix is one shape, applied to all
three: **put the general property first and let the specific values follow to explain it.** The same defect in Plan's
own work the same night: `check_unsupported_list.sh` passed its emptied-source arm because it pulled `rmw_ret_t` out of
`rmw_ret_t rmw_set_log_severity(` on both sides and matched the stray name against itself, and printing the two lists is
what exposed it. Print what was compared, not only whether it matched.

### g14's generalisation, as a Plan task: the other introspection surfaces (opened 2026-09-29)

g14 was one instance of *a value we report that we would refuse back*. The surfaces below hand data to tools that may
return it, so each gets the same round-trip check. **This is a task list, not a finding: nothing here is claimed to be
broken.** Each line says what would be handed back and by what, so the check is a test and not a reading - two careful
readings have been wrong on this repository already.

| surface | the value | what could hand it back | the check |
|---|---|---|---|
| `rmw_get_publishers_info_by_topic`, `..._subscribers_...` | the QoS profile | rosbag2, `ros2 topic pub --qos-profile`, any bridge | **g14, done and verified both ways:** every reported profile passes our own validator, and the `introspect` acceptance case fails against a build with the fix reverted |
| `rmw_get_topic_names_and_types`, `..._node_names...` | topic, service and node names | `ros2 topic pub`, a bridge recreating an endpoint, a launch file | a reported name is accepted by `rmw_validate_full_topic_name` / `..._node_name` / `..._namespace` **and** by `rmw_create_publisher` with it |
| the same | the type name | anything that looks up typesupport by name | the reported type name resolves through `rosidl_typesupport_c` to a handle |
| `rmw_get_gid_for_publisher`, `..._client`, event payloads | the GID | `rmw_compare_gids_equal`, a tool matching a sample's `publisher_gid` to a discovered endpoint | a reported GID compares equal to itself and unequal to every other endpoint's, and a taken sample's `publisher_gid` equals the one the graph reports for that writer |
| liveliness, deadline, incompatible-QoS, matched events | the status counts | a monitor summing them, `ros2 doctor` | the counts are self-consistent: `alive + not_alive` never exceeds the endpoints we report, `*_change` deltas match the totals, nothing negative |
| `rmw_get_serialization_format` | `"cdr"` | `rmw_deserialize`, rosbag2's stored format field | the format we report is the one `rmw_serialize` produces and `rmw_deserialize` accepts - g1 makes this checkable end to end |

**One constant worth noting now, because it is the kind of edge the name check is for and it needs no workspace to see:**
`tt_MAX_NAME_LENGTH` is 255 and rmw's own topic-name limit is also 255, so a name at our maximum sits exactly at the
edge of what rmw accepts, and any prefix rmw_tickle adds on the way to the wire comes out of the same budget. Whether
that can actually produce a reported name our own `rmw_create_publisher` refuses is **not established by that
observation** - it needs the round-trip test, at the maximum length and one past it, with the control being the same test
on CycloneDDS. Filed as the first case of the name row above rather than as a defect.

**The `introspect` case is verified in both directions (2026-09-29), and it took three attempts to get there.** Against
a build with g14's fix reverted, rmw_tickle FAILS and the control PASSES; against the fixed build both PASS, with
different binary ids each run so it was the reverted binary being measured. Its passing is therefore evidence. The three
defects it had before that, all of the same family and all worth more than the case itself:

1. **It asked its question of the endpoint that never had the defect.** The node read its own publisher back, which is
   served from the real profile it was created with; g14 lived in the discovered-entity branch that starts from
   `rmw_qos_profile_unknown`. Dev found this by reverting the fix and re-running rather than by reading the code, and
   the case passed with the defect present. It now uses two processes and the reading node creates no publisher on that
   topic, so every endpoint it is told about is remote by construction.
2. **A check that could not pass.** The finish test went through `ran()`, which reads a `sent=` field this role never
   prints - so it failed whatever the node did. The mirror image of a test that cannot fail, and **the control failing
   identically is what exposed it**: had only our arm failed, it would have looked like a finding about rmw_tickle.
3. **A failure that was silent exactly when it had something to say.** `field()` extracts numeric values only, so the
   text `detail=` came back empty and the failure printed as a bare `FAIL()` - at the moment it was holding
   `recreate_refused:RCLError:...rmw_tickle_only_supports_RMW_QOS` with `liveliness=4` (UNKNOWN) in the profile handed
   back, which is g14 exactly. **A reporting path that is quiet on success and empty on failure looks healthy
   indefinitely and fails you on the one day it matters.** Read as text at that call site rather than widening
   `field()`, since every other caller depends on it being numeric.

**Order:** the name and GID rows first, since a bridge or a `ros2 topic pub` round-trips them in ordinary use, then the
event counts, then the format. Before the next COMPARISON re-measure, so the table is not published beside a surface
that contradicts itself.


## g13 closed (2026-09-29): criterion by criterion, and one deliberate non-measurement

| # | criterion | rests on |
|---|---|---|
| 1 | matching | `test_keep_all_reader.c` creates a KEEP_ALL subscription against a KEEP_LAST publisher, and the `bag` acceptance matched a real rosbag2 recorder to a real talker across the netns pair with `rmw_cyclonedds_cpp` passing the same test. **The control is an observation, not a reading of the spec** - which is why it was asked for that way |
| 2 | no overwrite | capacity from the byte budget and not `qos.depth` (depth 2, capacity 4); capacity samples queued, three refused, and what the application takes back is the **first** four in order. Two mutants - and writing them found the test could not fail on its own claim until it was reordered |
| 3 | back-pressure, RELIABLE | the premise was tested **before** the code and failed as stated: an in-order sample is acked before the callback and the refusal is final. That is why the accept hook exists and why it sits above `update_reliable_ack()`. A declined sample is delivered with its own content, in the ordinary case and with a gap open elsewhere; the bound is tested too, with `gap_evicted` counting what the writer says it has dropped |
| 4 | BEST_EFFORT | a decline is a drop, counted and warned about, **with the RELIABLE arm as its control** - identical refusal, opposite outcome, the only difference being `reliable`. That the sample is gone for good is tested, not reasoned |
| 5 | `bag` | PASS and level with the control: 77 replayed each, zero gaps, against FAIL(recorded=0) the day before. The stall arm: both lose, so DDS semantics, and within that 38.6 against 57.8 over five pairs with its caveats. Running it turned up **g14**, now fixed with an acceptance case verified in both directions |
| 6 | no regression, and the cost | rmw suite 34/34, core green. **No timing number, by decision** - see below |

**Criterion 6 is the interesting one, and the absence of a number there is a choice rather than an omission.** Both
instruments have a floor at about the size of the effect (WIRE_PLAN 10.4: ~1% on p1 throughput, ~10 KB on peak RSS), so
Dev measured what is exact instead: `tt_Subscriber` 1216 -> 1240, `rmw_tickle_subscriber_t` 2448 -> 2480, publisher
unchanged, context unchanged, **every wire struct identical**. That is 32 bytes per subscription and one NULL test per
sample on the KEEP_LAST path. A hundred subscriptions is 3.2 KB against a ~10 KB RSS floor, so that instrument cannot
resolve it *in principle*, and p1 is below its floor by construction.

**Plan's decision on whether to run the placement-controlled p1 anyway: run it, but only as a falsification, with the
threshold written down first.** The argument against was good - a number produced by an instrument that cannot
discriminate is a number someone later quotes as a cost - but "the reading is known in advance" is also how a
measurement stops being made at all. So the rule is: it is worth running **only because there is an outcome that would
change the conclusion.** One extra branch cannot cost 3%; if p1 showed that, something unexpected happened (an inlining
cliff, a hot function crossing a boundary) and the exact evidence above would be wrong. So:

- **pre-registered threshold: 3%**, well clear of the ~1% floor;
- **below it, the plan records "not falsified" and the exact evidence carries the claim** - never a cost figure, and
  never its sign;
- **above it, criterion 6 is not met** and the cause is found before g13 is called closed.

**Result (Dev, 2026-09-29, `bd43ea2b`): not falsified.** The unforced pair moved by 0.659% in magnitude, well below the
3% threshold, over four arms of six runs each. So nothing turned up that one extra branch and 32 bytes cannot explain,
and criterion 6 stands on the exact figures. **The direction is deliberately not recorded**, here or in the raw summary:
the alignment flag alone moved one arm by 0.472%, the same order as the whole difference, so whichever way it points it
is layout and not g13 - and quoting a favourable sign would be exactly the mistake WIRE_PLAN 10.4 exists to prevent.
The raw output is kept at `/tmp/g13_p1_falsify_kept.txt`.

**One harness, two questions, and only one of them void.** `p1_layout_check.sh` printed `VERDICT=VOID`, correctly: it
exists to ask whether the earlier 1.1% is layout, and this unforced pair does not reproduce that 1.1%, so it cannot
speak to it. But the question here was "is the difference 3% or more", the unforced pair is exactly the comparison that
answers it, and six runs an arm with SEs of 2000-4400 on 3.7M samples answer it clearly. Dev did not read the printed
verdict as the answer - which would have left criterion 6 unfalsified **for the wrong reason**, the same shape as
everything else in this section from one more direction. **A harness's verdict is about the harness's question. When a
run is reused for a second question, the second question needs its own reading.**

**A fifth, in a waiter rather than in a test (Dev, 2026-09-29).** The until-loop waiting for the falsification run
grepped case-insensitively for "layout", and the harness's own first line is `=== p1 layout check ... ===`. So the wait
returned immediately, on the header, and a half-built run was nearly read as a result. It waits on `=== done` now, which
exists only at the end. **This is CLAUDE.md rule 2's exact shape in a waiter rather than in a `pgrep`:** the check
matched the checking's own output. Any wait condition has to be something the job prints only when the thing waited for
has happened - and it must match on failure too, or a crash looks identical to still running.

**And one more null result that was null for the wrong reason,** which is why a size check is not automatically safer
than a timing one: Dev's first size comparison substituted the parent's rmw header but compiled it against the *current*
`tickle.h`, so the struct came out unchanged. That is the old struct measured with the new core inside it - a null
result produced by the measurement rather than by the code. Same family as `introspect` asking its question of the
endpoint that never had the defect, and as a check that reads a field its subject never prints.


### An operational rule the two sessions found by colliding (2026-09-29)

**The netns acceptance suite and a rig campaign must not overlap.** They do not contend for the rig lock - the suite runs
entirely in PC-side namespaces - so nothing stops them, and that is the trap: they contend for the **PC's CPU**, and it is
the suite that suffers. Running together at load average 13.7/18.1, two of thirteen acceptance rows went VOID because the
**CycloneDDS control** failed (`itype` with `incompatible_type_events=0` on both endpoints, `samehost` with
`listener=110 echo=0`), and `samehost`'s control had passed earlier the same evening on the same binaries.

**Why that is worse than a wasted run:** a control failure under load looks exactly like a vendor finding. Two rows with
CycloneDDS failing and TickLE passing are the most flattering artefact available, and they would be wrong. The suite's own
rule - a failed control voids the row - is what stopped it from being reported, which is the rule earning its place.

**The campaign side was checked rather than assumed**, since the same load could in principle have touched it: 38 rows,
0 VOID, `loss_pct=0.0` throughout while the load was at its peak - its clients and servers run on the Pis and the PC only
orchestrates over ssh - and the rig logs showed no `10.1.1.x` peer from the netns run, the only matches being nine days
old. So the asymmetry is real: the campaign does not notice, which is exactly why the campaign is the side that has to
announce itself.


## Work list: the shared-memory module and the tests that can judge it (opened 2026-09-29, the user's instruction)

The user asked for the implementation **and** the test cases that can properly test it and compare it fairly with the
competing products, as work-list items rather than as a plan paragraph. Design and staging are in `SHM_PLAN.md`; the
tests are its section 6a. What belongs here is the order of work and who owns each piece.

| # | item | owner | done when |
|---|---|---|---|
| S1 | the transport seam in core, with UDP still carrying everything | Dev | wire bytes identical to the parent, and CPU, RSS and binary size read against a placement control - all three against the floors in WIRE_PLAN 10.4, not as bare numbers |
| S2 | the transport-identity test **first**, before any segment exists | Plan | a match reports the transport it used; forcing the attach to fail makes the test fail. Without this every later test is green over loopback and says nothing |
| S3 | the segment, with a copy on arrival | Dev | the whole acceptance suite passes with the module on, unchanged; `samehost` asserts `shm`; beats loopback UDP at p1-p4 by more than 2xSE with nothing worse |
| S4 | core unit tests against a fake segment in the mock HAL | Dev | SHM_PLAN 6a items 4-8 - single writer with many readers, no reuse before release, exhaustion counted, a reader that exits holding a record, reclaim after a dead owner |
| S5 | the kill test | Plan | reader killed mid-run, writer still making progress, segment reclaimed. A kill test, not a code reading |
| S6 | **the fair-comparison harness** | Plan | see below - this is the item the user named and the one with the most ways to be unfair |
| S7 | lending (`tt_Sample_retain`/`release`) | Dev | S3's numbers improve again, and by how much, so the win is attributed to lending rather than to "shared memory" |
| S8 | the CI matrix arm | Plan | the suite **runs**, not only compiles, with the feature on as well as off |

### S6, the fair comparison, in detail - because "same conditions" is the whole question

Both vendors have a shared-memory transport of their own and **their defaults differ**: FastDDS ships its SHM transport
on, CycloneDDS's shared-memory path is off unless configured. So a single number cannot be fair, and the plan is to score
**both comparisons and report both**, with any win naming which one it rests on:

- **engineering arm** - each framework's own shared-memory path against ours, all three explicitly configured;
- **default arm** - each framework's out-of-the-box configuration against ours, which is what a user actually meets.

**Feasibility settled 2026-09-29, and neither vendor has to be excluded.** The engineering arm needs each vendor's own
shared-memory path to actually work on the rig, which was an open question and is now checked on the client Pi:

| framework | shared-memory path | what the arm needs |
|---|---|---|
| FastDDS | `libfastrtps.so` present; its SHM transport ships **on** | nothing - the default arm and the engineering arm may be the same run, which has to be verified rather than assumed |
| CycloneDDS 0.10.5 | **available**: `libddsc.so.0.10.5` carries the `CycloneDDS/Domain/SharedMemory` config path (19 matches for SharedMemory/iceoryx), and the Iceoryx bindings are installed (`ros-jazzy-iceoryx-binding-c`, `iceoryx-hoofs`) | `SharedMemory` enabled in its config XML **and `iox-roudi` running** - the daemon is at `/opt/ros/jazzy/bin/iox-roudi`. That is an operational requirement, not a flag |
| TickLE | the module being built | `tt_SEGMENT_ENABLED`, on by default |

**This corrects an implication in the earlier draft** of this item, which read as though CycloneDDS might have no
shared-memory path to compare against. It has one. So the incomplete-delivery rule does not get to excuse an arm here:
if the CycloneDDS engineering arm cannot be made to run, that is a setup failure of ours to report, not a vendor that
lacks the feature.

Four requirements that the existing campaign does not yet meet, each of which is a way to be accidentally unfair:

1. **Both processes on one host.** Every campaign cell today is cross-host, so the same-host tier has no cells at all.
   A same-host cell set has to be added, on one Pi, with the same payload shapes as p1-p4.
2. **Each arm proves which transport it used**, on every framework and not only on ours - a vendor arm that quietly fell
   back to loopback would flatter us exactly as ours would flatter itself. Read it from each framework's own
   introspection or counters, and VOID the row when it cannot be read.
3. **The QoS stays identical across the three**, as the user required on 2026-09-26 and as `campaign_sweep.sh` already
   enforces by voiding a row whose RESULT line does not show the values.
4. **The floors apply.** A same-host difference under about 1% on throughput, or 10 KB on RSS, is not a result
   (WIRE_PLAN 10.4), and the in-process tier (g9) is the lower bound: a same-host number faster than in-process delivery
   means the harness is wrong, not that the transport is fast.


#### S6's transport witness, and why it is the loopback counter rather than each vendor's introspection (2026-09-30, Plan)

Requirement 2 above - *each arm proves which transport it used, on every framework and not only on ours* - is the one
that decides whether this comparison is worth running, and the obvious implementation is the wrong one. Reading each
framework's own introspection means three different instruments, each of which can report what it was **configured**
with rather than what it **did**: FastDDS can have its SHM transport enabled and still fall back per-datagram,
CycloneDDS reports SharedMemory enabled whether or not `iox-roudi` is actually answering, and our own `tx_shm` is a
counter we wrote and would be grading our own homework with. Every one of those is a configuration read, which is the
failure this project has now made four times in one week - a `transport=tcp` field printed by a dead session, a
sanitizer that was never linked, a ThreadSanitizer gate that never created a segment, a `sudo -n kill` that killed
nothing.

**So the witness is the loopback interface's own packet counter, which no framework controls and none can flatter.**
On a same-host cell the data interface is `lo`, and `BenchStats.h` already reads `wire_tx_packets` / `wire_rx_packets`
and reports `wire_packets_per_sample` for all four harnesses from the same code. That number answers the question
directly:

**The bands have been measured rather than guessed, and the guessed ones were wrong** (`s6_witness_check.sh`,
2026-09-30, `results/s6_witness_573cfed9_2026-09-30.txt`, both roles on one Pi, `BENCH_IFACE=lo`, n=3 per arm). The
first version of this design proposed "at or below 0.05 means shared memory, at or above 0.80 means the kernel". The
run refused both, by its own pre-registration, and the two reasons are worth more than the thresholds were:

| arm | `wire_packets_per_sample` on `lo` | `tx_shm` | `tx_udp` | send Mbps |
|---|---:|---:|---:|---:|
| module on | 0.273 (0.234-0.306) | 3,162,505 | 7 | 434.18 |
| module off | **2.000** (every rep) | 0 | 895,691 | 108.91 |

- **The kernel-path baseline on loopback is 2.0 per sample, not 1.0.** `wire_tx_packets` and `wire_rx_packets` are both
  898,467 for 898,463 samples: on `lo` a datagram is counted once leaving and once arriving, on the same interface. Any
  absolute band written for a two-interface cell is wrong here by a factor of two.
- **The shared-memory arm is not zero, and should not be.** The client put 7 UDP datagrams on the wire in total, all
  broadcast (`tx_udp_broadcast=7`), with 3.0 million samples through the segment - and `lo` still shows 449,926 packets
  each way. Those carry no samples: the transport legitimately signals over the socket while the data goes through the
  ring (the doorbell that wakes a blocked reader is the mechanism here). **A transport using shared memory for data
  does not stop using the network for control**, and a witness that expects zero mistakes control traffic for a
  fallback.

**So the witness is real but must be calibrated per cell from its own kernel-path arm, as a ratio rather than an
absolute.** Here that ratio is 0.273 / 2.000 = **13.7%**, a 7.3x separation, which discriminates without ambiguity. The
rule for S6:

| ratio of the arm's `lo` packets-per-sample to the same cell's kernel-path arm | reading |
|---|---|
| at or below ~0.25 | shared memory carried the data; what remains is control traffic |
| at or above ~0.75 | the kernel carried the data, whatever the configuration claimed |
| between | a mixed or partial path - **VOID**, and the row says so rather than picking a side |

**Every cell therefore needs its own kernel-path arm measured beside the arm under test**, which for the vendors means
their shared-memory path off as well as on. That is more runs than the original design implied and it is not optional:
without the paired baseline the ratio has no denominator. It also means each framework's control traffic is measured
rather than assumed, which matters because discovery, acknowledgements and iceoryx signalling will each put a different
non-sample load on the interface.

The middle band is the valuable one. A vendor arm that quietly fell back for some fraction of its traffic lands there,
and the honest report is "this arm did not use one transport" rather than a number averaged over two.

**Our own counter and the witness agreed, which was the outcome that mattered most.** On the module-on arm `tx_shm`
was 3,162,505 against `tx_udp` 7, and the witness independently saw the data leave the network path; on the off arm
`tx_shm` was 0 against `tx_udp` 895,691 and the witness saw a full kernel path. Two instruments, one fact, no
disagreement - so neither is currently suspect and the pairing below stands as a live check rather than a formality.

**Our own counter then becomes a control rather than the evidence.** `tx_shm / (tx_shm + tx_udp)` and the loopback
witness are two independent measurements of one fact, so they must agree: on a TickLE same-host row, a high `tx_shm`
share with a loopback witness near 1.0 means our counter is lying, and a low share with a witness near 0 means the
witness is. **Either disagreement VOIDs the row and is a finding about the instrument, not about the transport.** That
pairing is the only part of this design that could catch a defect in the thing we built, which is why it is in.

**The two bounds from requirement 4, stated as refusals rather than as guidance:**

- **Lower:** a same-host throughput difference under about 1%, or an RSS difference under 10 KB, is not a result
  (WIRE_PLAN 10.4). At same-host rates - the module measured 2,793 Mbps against 1,902 in CI - a real shared-memory
  effect is tens of percent, so anything near the floor here is far more likely to be layout than transport.
- **Upper:** g9's in-process delivery is the ceiling. A same-host row faster than in-process delivery of the same
  payload means the harness is wrong - it is claiming that crossing a process boundary beat not crossing one - and the
  row is VOID with the harness named, not published as a record.

**What is still open in this design, so it is not mistaken for finished.** The engineering arm needs `iox-roudi`
running for CycloneDDS, which is an operational prerequisite rather than a flag, and the arm must fail loudly when the
daemon is absent rather than silently measuring CycloneDDS's network path and labelling it shared memory. That is the
same shape as everything above, and it is the first thing to build once the cells exist.

## g15 - `publish_zerocopy()` sends datagrams that `tx_datagrams` never counts (found by Dev 2026-09-29; LOW severity, HIGH consequence for S1)

- **Gap:** `tt_Context.tx_datagrams` is incremented in `send_datagram_to()`, `send_datagram()` and the fragment batch,
  covering eleven of the twelve `tt_send*` call sites in `src/tickle.c`. The twelfth, `publish_zerocopy()` (line 3377),
  calls `tt_send_iov()` directly at 3417 and 3425 and increments nothing.
- **Measured, not read** (mock HAL counting every datagram handed to it):

  | path | published | `tx_datagrams` | datagrams the HAL saw |
  |---|---:|---:|---:|
  | ordinary publish | 5 | 5 | 5 |
  | `publish_zerocopy` | 5 | **0** | 5 |

- **Severity as a defect today: low.** It is a diagnostic counter, not delivery - nothing is lost, and only the traffic
  line under-reports. But it under-reports on **the zero-copy path**, which is the path someone debugging throughput is
  most likely to be on, so the number is wrong exactly when it is being trusted.
- **Consequence for the shared-memory work: high**, and it is why the fix is shaped the way it is. Per-transport counters
  placed at the three existing counting sites would inherit the hole, on the path where shared memory has the most to
  offer, and S2 would report `shm` at zero for a payload that really did travel over the segment. **A false negative
  shaped exactly like the failure S2 exists to catch.**
- **Fix:** taken inside S1 rather than separately. The seam wraps the HAL calls, `tx_datagrams` moves into it, and the
  twelve sites become one place - so this closes as a side effect of the seam being built correctly. Filed anyway,
  because a defect that is fixed as a by-product still needs a record saying it existed and how it was found.
- **Pass:** `publish_zerocopy` and the ordinary path both report `tx_datagrams` equal to the datagrams the HAL was handed,
  with the mock HAL's own count as the control; and no send site increments a counter outside the seam, checked by there
  being one place that does it.
- **How it was found is worth as much as the finding.** Dev first read the direct `tt_send_iov` calls, concluded the
  *ordinary* publish path was uncounted, and was about to report that. The test said `published=5 tx_datagrams=5`: the
  ordinary path is fine, and the calls being read belong to `publish_zerocopy`, which that path never reaches. Ten
  minutes of three arms turned a wrong claim into a right one - and a defect report about the wrong function into a
  correct one about the right one.

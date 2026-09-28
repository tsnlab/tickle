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

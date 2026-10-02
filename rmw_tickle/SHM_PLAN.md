# The shared-memory module: design, staging and what each stage must prove

The detailed plan the user asked for in decision 5 (`MODULE_PLAN.md` section 0), written after decision 4 settled the
shape: **a core-only module.** `MODULE_PLAN.md` holds the seams and the acceptance criteria both optional modules answer
to; this file is how the shared-memory one is actually built, in what order, and what each step has to show before the
next one starts. Nothing here is implemented yet.

## 1. What it is, and what it is not

**It is a second transport for a peer on this host.** Same wire semantics, same QoS rules, same discovery: only the
carrier changes. A sample that would have gone into a UDP datagram to 127.0.0.1 is instead placed in a segment the
peer already has mapped.

**It is not a second protocol.** No new message kinds, no separate handshake, no alternative reliability. The datagram
that lands in the segment is the datagram that would have gone on the wire, byte for byte, so every rule core enforces
on arrival - version check, QoS compatibility (RxO), the g8 context-id rules, and, once TickLE Security exists, its
authorization and protection - runs unchanged and in the same place. This is `MODULE_PLAN.md` criterion 5 stated as an
implementation constraint rather than a test: the same bytes through the same acceptance path is what makes the
bypass impossible instead of merely tested for.

**It is not in rmw.** There is no rmw-side half. rmw reads an environment variable and reports which transport a match
used, both in code that already exists for other purposes.

## 2. Where it attaches

Three attachment points, all of them already present in core for UDP:

| Point | What the module provides | What core keeps |
|---|---|---|
| Send | place a datagram in the peer's segment, or fail | the decision of *which* transport this peer is reached by |
| Receive | hand core a datagram and, later, release it | the whole acceptance path, unchanged |
| Peer lifecycle | create, attach to, detach from and reclaim a segment | when a peer appears, times out or is replaced |

**Corrected 2026-09-29, and the correction matters more than the claim it replaces.** This section said the transport
choice rests on "g8's host registry in /dev/shm gives each context on a host an id of its own". **It does not: that
registry is keyed per network, not per host.** `registry_path()` builds `tickle-context-ids-<port>-<addr>-<broadcast>`,
and six of them exist on this machine as this is written, including `tickle-context-ids-8282-0.0.0.0-10.77.0.255` beside
`tickle-context-ids-8282-0.0.0.0-192.168.10.255`. That is correct for what the registry is for - an id must be unique per
network - and it means **two contexts in different namespaces can both hold context id 5.**

The consequence is exactly the failure section 1 is most concerned with, arriving through naming rather than through the
protocol: **a segment named by context id would collide across the very pair S2 runs in.** Two contexts would open the
same segment, each believing it was the peer's, and nothing would report an error - they would read each other's records.

**What is true, and is what the transport choice actually rests on:** `/dev/shm` is shared across network namespaces -
verified by creating a file in one and reading it from another with the same inode, and stated already at
`hal_linux.c:368` in the code that depends on it, "two network namespaces with different addresses share /dev/shm but
never an address". So two namespaces on one machine are a same-host pair, which is what makes S2's environment represent
what this module has to serve. **No new discovery mechanism.** If the module is not built, or the peer is not on this host, or attach
fails, the peer is reached over UDP exactly as today - that fallback path is the module's own failure mode, and it is
silent by design in the sense that correctness does not depend on it, but it is counted so a user can see it happened.

## 3. The segment

Decisions to make explicitly, because each one has a failure mode that only shows under load or after a crash:

- **One segment per writing context, not per topic or per pair.** A reader attaches to a writer's segment read-only.
  Per-pair segments multiply by peers; per-topic segments multiply by topics and make a late-created topic need a new
  segment mid-run. Per-writer is the smallest number that still lets a reader map only what it needs.
- **Slot size and the budget (decided 2026-09-29, Plan's call on the numbers since it owns the measurement).**

  `slot_bytes = tt_CONTROL_MAX_LENGTH`, `tt_SEGMENT_BYTES` default **512 KiB** per writing context, compile-time and
  overridable by `-D` like every other core capacity. That gives **256 slots** in both an rmw build and a native one,
  because `tt_CONTROL_MAX_LENGTH` is 1472 in both - **the identity that was the whole point of the objection**, and it is
  checked by compiling the size program both ways rather than by reasoning about the constants.

  The derivation, carried here because the next person to change the budget needs it: the slot header is 8 bytes, so the
  stride is 1480; 512 KiB divided by that is 354; and it **rounds down to a power of two** so the ring indexes with a
  mask, giving 256 slots and 378,880 bytes actually used of the 512 KiB ceiling. **The budget is a cap, not a target** -
  the 28% left over is the price of masked indexing, and it is cheaper than a modulo on every record. (This corrected an
  earlier "about 340" in this section, which was the raw division written down without the rounding that follows it.)

  **Why not `tt_MAX_BUFFER_LENGTH`, which is the obvious pick:** an rmw build compiles with
  `-Dtt_MAX_BUFFER_LENGTH=65507`, so a 64-slot ring would be **4.2 MB per writing context** against 94 KB in a native
  build. Measured rmw peak RSS is 12.3 MB against CycloneDDS's 14.7 and FastDDS's 23.8 - a 4.2 MB ring would spend most
  of the margin we win on, in the metric we win on, as slack in slots that never fill.

  **Why not a fixed budget with 65507-byte slots either**, which was the counter-proposal: 512 KiB then yields **8
  slots**, and 8 is too shallow, by a number from our own measurement rather than by feel. The argument for it was that a
  deeper ring is less necessary when datagrams are larger, which is true and does not carry the conclusion: **a reader's
  drain rate is set by its poll period, not by the datagram size**, so the time a ring covers is slots times per-datagram
  cost and 8 slots is 33 us however large the datagrams are. Worth recording as its own shape, because it is the one this
  repository keeps meeting in new clothes - **a plausible mechanism standing in for a number.** The S2 run sent 967,475
  datagrams in 4 s, about **4.1 us per datagram**; a reader drains on its poll, nominally every 100 us; so a ring must
  hold at least one poll period of output, roughly **25 datagrams**, and comfortably more to absorb a burst. 8 slots is
  33 us of output and would be full most of the time, with the writer blocking or falling back - and stage 1 would then
  fail its own throughput criterion for a sizing reason rather than a transport one. 340 slots is 1.4 ms of output.

  **What that costs, stated rather than buried: a datagram larger than a slot goes by UDP.** Dev established the case
  this matters for, and the correction is worth keeping because it contradicts a claim made here first: **service
  requests and responses do not fragment.** `config.h:415` says it where it is decided - "a build that raises
  tt_MAX_BUFFER_LENGTH (rmw_tickle, to 65507) ... its samples then fragment at the control datagram, while service
  requests and responses, which do not fragment, keep the large datagram" - and `valid_msg_size()` bounds a request and a
  response by `tt_MAX_BUFFER_LENGTH`, not by the control datagram. So in an rmw build a large service call really is a
  single 65507-byte datagram, and MTU-sized slots cannot carry it.

  **So it goes by UDP, deliberately, counted, and written down here** - which is the whole difference from the failure
  Dev was warning about. A module that carries topics and quietly never carries services is worse than one that does not
  work; a module that carries topics and says, in a counter and in this plan, that oversized datagrams use UDP is a
  scope limit. Three things make it that rather than the other:
  - a dedicated counter for oversized datagrams routed to UDP, so the cause is named rather than inferred from which
    shapes show `shm`;
  - **S2's expectation for a service shape is `udp` by design**, recorded here so a future reader cannot take it for a
    partially wired seam - which is exactly what that matrix is otherwise for;
  - a `static_assert` that the budget yields at least a structural minimum of slots (4), so a configuration that would
    leave a one-slot ring is a build error. The *performance* depth requirement above is a measured criterion of stage 1
    and not an assert, because the `ipfrag` diagnostic arm legitimately has a 65507-byte control datagram and must still
    build.

  **And the fix if measurement says services matter:** one ring with variable-length records spanning several
  contiguous slots, padding to the end rather than straddling the wrap so Dev's no-half-records property survives. That
  carries every datagram at one slot size and removes the scope limit, at the cost of a reserve-N-slots writer and a
  span-aware reader. It is deliberately **not** stage 1: stage 1 measures topics, and complexity added before a
  measurement asks for it is complexity that cannot be attributed.

- **Fixed capacity, chosen at context creation and stated,** like every other core structure. A ring of records sized
  from the same budget idiom the KEEP_ALL publisher cache uses (`RMW_TICKLE_KEEP_ALL_BYTES`' sibling). Exhaustion is
  counted and warned about once - never silent, never unbounded.
- **Single writer, many readers.** The only writer is the owning context, which removes the general multi-writer
  concurrency problem. Readers coordinate with the writer through a per-record sequence and the release accounting
  below.
- **Naming (decided 2026-09-29, Dev's proposal with two additions).** The segment is named from **(peer address, peer
  port, peer context id)**, all three already in `struct tt_Peer` from ordinary discovery and known to both sides. The
  address is what differs between two namespaces, so the triple is unique where the id alone is not - for the same reason
  the id registry keys on it. **No wire change and no host token in the announce:** the address already distinguishes what
  a token would, and stage 0's claim is that the wire is untouched. "Are we on the same host" is then answered by
  *attempting to attach*: if the segment opens we are, if it does not we use UDP, which is the fallback this section
  already describes rather than a new mechanism. A HAL-provided segment on FreeRTOS can key the same triple however it
  likes.
  - **Addition 1, which is what makes the naming safe rather than merely unique: never trust the name.** The segment
    carries a header - magic, version, the owner's own (address, port, id) and a **per-launch incarnation token** - and the
    reader **validates it after attaching**. A context id is re-handed once its holder dies, so a stale segment can
    legitimately carry a name a new reader will compute, and the name alone cannot distinguish incarnations. With the
    header a collision or a stale segment is *detected* and the pair falls back to UDP; without it the failure is the
    silent one. It is the same reasoning the wire already applies with its version check, in the one place a wire check
    cannot reach.
  - **Addition 2: attach failures are counted by reason.** "The attach failed" is ambiguous between a different host, a
    permission refusal and a missing segment, and all three fall back to UDP safely - which is exactly how a module that is
    **permanently inert in the field** would look identical to one correctly deciding "not same host". S2 catches an inert
    module in the test environment; only a counter catches it in production.
- **Permissions.** Created with the owner's own user and no wider access. A segment a process cannot open is a fallback to
  UDP, not an error - and it is counted, per addition 2.
- **Reclaim after a crash is a requirement, not a nicety.** A killed writer leaves its segment mapped by its readers
  and its name in place. The owner unlinks at teardown; a reader that finds the owner gone detaches and falls back;
  and a stale name whose owner is no longer in the host registry is reclaimed by the next context that would create
  it. `MODULE_PLAN.md` criterion 4 is the test, and it is a kill test, not a code reading.

## 4. Lending is where the win is

The prize is not a cheaper datagram - it is that the reader reads the sample **in place**. With a copy out of the
segment on arrival, this transport is loopback UDP minus a syscall. With lending, it approaches g9's in-process cost,
which is the tier above it (+19 ns at 64 B, +1.6 us at 60 KB).

So the module is designed on top of receive-buffer lending (`tt_Sample_retain` / `tt_Sample_release`, approved
2026-09-28), and the two are staged so the measurement can tell them apart:

- a record is not reusable by the writer until every reader that took it has released it;
- a reader holding a record must not be able to stall the writer indefinitely - a record held past the ring's wrap is
  the reader's own back-pressure problem, and under RELIABLE it becomes the same un-receive the g13 accept hook
  introduces, not a silent overwrite;
- a reader that exits without releasing must not park a record forever: release accounting is per attached reader, and
  a reader that leaves the host registry releases everything it held.

**This is the dependency that decides how large the win is, so the pre-registration below attributes the win to one or
the other rather than to "shared memory".**

## 5. Stages, each with what it must prove

Each stage lands on `main` with its own tests and its own measurement. A stage that cannot show its criterion does not
hand over to the next one.

**Stage 0 - the seam, with UDP still doing the work.** Introduce the transport attachment points and route every peer
through them with the UDP implementation behind them. Nothing new is sent. *Must prove:* identical wire bytes against
the parent build, and CPU and binary size indistinguishable from it read against a placement control (`WIRE_PLAN.md`
8.3's amendment), using `sched_cpu_s` / `sched_by_thread=` for the CPU comparison rather than tick-accounted `cpu_s`.
This is the stage that makes decision 6 - "using a module must cost no performance" - a measurement instead of a hope,
and it is worth its own commit for exactly that reason.

**Stage 1 - a segment, with a copy.** Same-host peers exchange datagrams through the segment; the reader copies out on
arrival. *Must prove:* every acceptance and unit test that passes over UDP passes over the segment, unchanged; the
`samehost` acceptance passes; a datagram that should be refused (wrong version, incompatible QoS, wrong context) is
refused identically; and it beats loopback UDP on **throughput and CPU per sample** at p1-p4 by more than 2xSE with
nothing worse by the same rule.

**Stage 1's claim is deliberately scoped to throughput and CPU, and its latency cells are VOID by construction (decided
2026-09-29; Plan's call, since it owns the measurement).** Dev asked, before building the ring, whether stage 1 should
carry a notification mechanism. It should not - and the reason it should not also decides what stage 1 may claim:

- **A UDP reader is woken by the kernel** when a datagram arrives: the poll blocks on the socket, which is why
  `wait=block` is a scored case at all. **A segment with no notification wakes the reader on its own next poll tick
  instead**, so a stage 1 latency figure would be dominated by up to one poll period - about 100 us - rather than by the
  transport. Shared memory would *lose* on latency for a reason that has nothing to do with shared memory, and the loss
  would be attributed to it.
- **That is not a smaller version of the design, it is a different wait model.** A reader watching a socket and a segment
  has to block on both, so the notification is not an optimisation on top of the ring - it is what lets a shm reader block
  at all. Building one now means building it on a guess and measuring afterwards, which is the wrong order here: open
  question 1 (futex/eventfd versus spin) is a measurement, not a preference.
- **So** stage 1 builds no notification, the ring has no wakeup mechanism to undo when the measurement picks one, and the
  **latency cells are void for the shm arm with the reason stated** rather than printed as numbers someone will quote.
  Throughput and CPU per sample are unaffected by the wait model at saturation, and are the honest claim.
- **One requirement that keeps the rows unreadable-in-the-wrong-way impossible: every row states the reader's wait
  model.** The rmw rows already carry `wait=block|poll`; the native shm arm must say it is timer-woken. A timer-woken shm
  latency beside a socket-woken UDP latency is an invitation to the wrong conclusion, and a row that cannot be misread is
  cheaper than a correction.

**Stage 1a - notification, as its own experiment.** Three arms on the same ring - futex, eventfd, and a bounded spin -
measured at p1 for latency and p4 for throughput, with CPU per sample as the cost side and **g9's in-process path as the
floor** (a same-host latency below in-process delivery means the harness is wrong, not that the transport is fast). Only
after this do the latency cells stop being void, and only then does this plan claim "beats loopback UDP on latency".

**Stage 2 - lending.** The reader reads in place and releases. *Must prove:* the stage 1 numbers improve again, and by
how much - this is the number that says what lending itself bought; no record is reused before release, shown by a test
that holds a record across a wrap; and a reader killed while holding records does not park them.

**Stage 3 - the vendor comparison, on decision 7's terms.** Both comparisons scored and both reported: each vendor's
own shared-memory path against ours, and each vendor's default configuration against ours. FastDDS ships its SHM
transport on; CycloneDDS's shared-memory path is off unless configured; so the two comparisons are different questions
and a win has to say which one it rests on.

## 6. How the measurement will be read, written before it is run

- **The cell matrix** is the campaign's own p1-p4 on the rig, plus a same-host pair on one Pi, run ABBA-interleaved
  (`campaign_ab_chain.sh`) so drift lands on both arms.
- **The floor:** g9's in-process path. A same-host result faster than in-process delivery means the measurement is
  wrong, not that the transport is fast - that arm exists to catch a harness error, and it is the first thing to check.
- **The ceiling:** loopback UDP, the path being replaced.
- **A WORSE cell is a candidate, not a finding** (`WIRE_PLAN.md` 8.3): at this many comparisons, 2xSE alone fails a
  build that changed nothing. Confirmation is consistency across at least half the cells, or a targeted re-run.
- **Tick-quantised CPU is not evidence.** `sched_cpu_s` and the per-thread breakdown are, which is why the instrument
  landed first (`7698f669`).
- **Every run states which arm it is** - transport, lending on or off, and the build - or it is void.

## 6a. The tests, because a transport is unusually easy to test into a false pass

The user's instruction of 2026-09-29: the feature arrives with tests of its own. The reason this transport needs its test
list written before the code is specific to it - **if shared memory silently falls back to loopback UDP, every functional
test still passes and proves nothing.** So the list starts with the test that catches that, not with the feature's own
behaviour.

**S2's contract, decided 2026-09-29 before the seam was written, because the seam has to carry it**

The transport-identity test cannot be a mode flag. There are **four send entry points** in core - `tt_send` (2 call
sites), `tt_send_to` (2), `tt_send_iov` (4), `tt_send_batch` (4), twelve in `src/tickle.c`, counted rather than assumed -
and one receive point, `process_packet()`. A seam that wires `tt_send` and leaves the iov and batch paths on UDP would
report "shm" to a mode flag while **fragmented samples and batched ack paths went over loopback**, and every functional
test would still be green.

So what the test reads is **how many datagrams went each way**:

- per context, counts of datagrams **sent and received per transport** - `shm` and `udp` at minimum, with room for a
  third;
- **counted by wrapping the HAL calls themselves**, not at the places that count today - so all twelve send sites are
  covered by construction rather than by remembering to instrument each one. That is not a precaution: **remembering has
  already failed once in this exact code** (see the box below);
- readable by a **native** harness and not only through rmw, since the campaign runs native clients while the acceptance
  suite runs rclpy nodes;
- printed in the RESULT line by `BenchStats.h`, as `retransmitted` and `gap_evicted` already are, so a row carries its
  own evidence instead of depending on a log.

**And the counters get an external control for free, on every row.** `wire_tx_packets` in the same RESULT line is the
kernel's own `/proc/net/dev` count for the interface, so `tx_udp + tx_shm` can be compared against it without a dedicated
test: a first run of the native harness gave `tx_udp=946982` against `wire_tx_packets=946984`, agreeing within two
packets. A seam counter that drifts from the interface is then caught by any row rather than by remembering to check -
which matters because the alternative was trusting the counters that S2's whole verdict rests on.

**S2's assertion corrected 2026-09-29, before its first real run rather than in it.** `tx_udp == 0` is unachievable and
was wrong as written. A segment carries **unicast datagrams to a known peer** - the name is computed from that peer's
(address, port, context id) - so a **broadcast destination has no name to compute and cannot go over a segment even in
principle**. Announces and summaries are broadcast, so a running context always has some `tx_udp` (Dev's finding, made
before the run).

The repair is an **exact** assertion rather than a weaker one:

> `tx_shm > 0`, **and** `tx_udp` equals the sum of the by-reason fallback counters -
> `segment_broadcast_to_udp + segment_oversize_to_udp + segment_unattached_to_udp + segment_full_to_udp`.

"tx_udp small and flat" would need a threshold nobody can derive. "Every UDP datagram has a named reason" needs none and
is **strictly stronger**: it fails on an *unexplained* fallback, which is the most valuable failure this test could
report and exactly what a threshold would hide. A shape that did not go over the segment then appears as a large
unexplained remainder - the partial-seam signature in its exact form. And when the RESULT line carries no by-reason
counters the shape is **VOID and says so**, because a pass that skipped the assertion is the thing this script exists to
prevent.

**That also fixes the number of fallback counters at four, one per cause**, which is the same argument as keeping
oversized and unattached apart, extended: *full* is a sizing signal, *unattached* a discovery signal, *oversized* a scope
signal, and *broadcast* a by-design signal. One counter for all four would say only "something fell back", and the
assertion above would have nothing to check against.

**And the fourth counter immediately earned itself, in a way worth recording as its own lesson.** Adding it exposed that
`segment_deliver()` returns false for a broadcast - a broadcast has no context id - and the caller was counting that as
**unattached**. So a healthy running context would have reported a large *unattached* count, which is a discovery signal,
for traffic that was never a candidate for the segment at all. **The assertion above would have held numerically and the
diagnosis would have been wrong**, which is worse than a failure: a failure gets investigated. The rule that follows is
not about counters specifically - *an invariant can hold while one of its components means the wrong thing*, so a sum
that checks out is not evidence that its parts are correctly attributed.

**The invariant is maintained by construction, not only asserted** (Dev's decision, and the better of the two): one
helper increments `tx_udp` and exactly one reason counter together, so no path can count a UDP datagram without naming
why. S2 still earns its place, because it checks the same thing end to end across a process boundary where a helper
cannot reach - but the drift it would have caught after the fact is now impossible to write.

The assertion is then exact rather than interpretive: for a same-host pair, `udp_sent == 0` for the shape under test,
**checked per shape** - one payload that fits a datagram, one that fragments, and a request/reply so `tt_send_to` is on
the list - rather than once for the run. The partial-seam case fails it, which is the only reason the test exists.

**The evidence that "by construction" is the requirement and not a style preference (Dev, 2026-09-29).**
`tt_Context.tx_datagrams` is incremented in three places - `send_datagram_to()`, `send_datagram()` and the fragment batch
- which cover **eleven** of the twelve send sites. The twelfth is `publish_zerocopy()` (`src/tickle.c:3377`), which calls
`tt_send_iov()` directly at 3417 and 3425 and increments nothing. Measured with the mock HAL counting every datagram it
is handed:

```
ordinary publish:  published=5  tx_datagrams=5  mock_send_calls=5
publish_zerocopy:  published=5  tx_datagrams=0  mock_send_calls=5
```

Five datagrams left the node and the counter said none did. Filed as its own defect below, and decisive for S1: **per-
transport counters placed at the three existing counting sites would inherit that hole exactly**, and the hole is on the
zero-copy path - which is where a shared-memory transport has the most to offer. S2 would then report `shm` at zero for a
payload that really did travel over the segment, and the natural reading would be "the seam is not wired" when the truth
is "the counter is not there". **A false negative shaped exactly like the failure the test exists to catch.** So the seam
wraps the HAL calls and `tx_datagrams` moves into it: twelve sites become one place, and the zero-copy gap closes as a
side effect rather than as a separate fix.

**Field names (Dev's, adopted):** an array indexed by transport rather than a field per transport, so a third transport
needs no new fields and the seam cannot acquire a counter someone forgets to add -
`enum tt_Transport { tt_TRANSPORT_UDP = 0, tt_TRANSPORT_SHM = 1, tt_TRANSPORT_COUNT }`, with
`tx_datagrams_by_transport[]` and `rx_datagrams_by_transport[]` on `struct tt_Context` beside the existing
`tx_datagrams`/`rx_datagrams`, printed flat in the RESULT line as `tx_udp= tx_shm= rx_udp= rx_shm=` because
`BenchStats.h`'s consumers parse `key=value`.

**A defect the first verification found, before anything depended on the counters (2026-09-29).** The new arrays were
added without being reset: `tt_Context` is caller-owned and `reset_node_state()` initialises field by field rather than
memset-ing, so a field that is only ever incremented is never zero. A run that really sent 949,078 datagrams reported
`tx_udp=140723338891185` - a stack address. **A counter that starts from garbage cannot be told from one the seam failed
to reach**, which is precisely the false negative this section exists to prevent, so it would have discredited S2's
verdict in the direction hardest to notice. Fixed beside the scalars they belong to, and found only because the RESULT
field was run rather than assumed to work once it compiled.

**This also gives stage 0 a test it would not otherwise have:** with the counters in and no segment yet, every datagram
is `udp` and `shm` is zero. That is a real assertion about the seam being wired without changing behaviour, and it is the
arm that proves the counters work **before** anything depends on them - the same ordering that this plan asks for
everywhere else.

**The tests that can otherwise pass for the wrong reason**

1. **Which transport carried the sample is asserted, never assumed.** A match reports the transport it uses, and the test
   requires `shm`. Mutant: force the fallback (make the attach fail) - the test must fail. Without this, all of B and C
   below are green over UDP and say nothing about the module.
2. **Byte identity.** The datagram placed in the segment equals, byte for byte, the datagram the UDP path would have sent
   - golden bytes captured from the UDP arm. This is section 1's constraint as a test: the same bytes through the same
   acceptance path is what makes the same-host bypass impossible rather than merely checked for.
3. **A datagram that must be refused is refused identically** over the segment: wrong wire version, incompatible QoS
   (RxO), wrong context id, and - once TickLE Security exists - its authorization and protection. This is the anti-bypass
   test, and it exists before the code so the seam cannot be shaped in a way that makes it impossible.

**The segment's own rules, as core unit tests with a fake segment in the mock HAL** (so they run in `make test`, with no
`/dev/shm` and no second process)

4. **Many writers, one reader** - two writers into one segment, both records seen, none lost, and the slot claim
   contended deliberately rather than by luck.

   **Topology decided 2026-09-29, and the plan was the half that was wrong.** This list said "single writer, many
   readers: two attached readers each see every record", which is a segment per *writer* that readers attach to.
   Section 2 said the opposite - "place a datagram in the peer's segment" - and the implementation followed section 2:
   **one segment per context, created and drained by its owner, written by every peer that wants to reach it.** The two
   are not compatible and only one can be tested. **The implementation's direction stands and this item is corrected to
   match it**, for three reasons:
   - the reader drains **one** place rather than polling a ring per peer, and the poll loop is the latency-critical
     path; a per-writer topology makes the reader's cost grow with the number of peers it hears from;
   - the name is computed from **the peer being sent to**, which is what the send path already has in `struct tt_Peer` -
     a per-writer topology would need the reverse lookup;
   - **lending still works**: the writer copies into the receiver's segment, and the reader lends *that* copy to the
     application without a second copy, which is exactly what stage 2 asks for. So this does not have to be rebuilt
     between stages, which was the constraint that ordered lending before loans in the first place.

   **What it costs, recorded rather than discovered later:** a publisher with N local subscribers writes N copies, where
   a per-writer segment would write one that all N read. That is irrelevant to every cell we measure (one publisher, one
   subscriber) and it is the condition under which the per-writer or a hybrid descriptor design becomes worth building -
   **a cell the campaign does not have**. If same-host fan-out ever matters, that cell comes first and this decision is
   revisited with a number rather than with a preference.

   **And the defect this reading found, which is the reason item 4 exists at all:** `segment_write()` did a plain load
   of `write_index` and a store of `write_index + 1` - a read-modify-write with no atomicity, in a ring that is
   many-writer by construction. Two peers publishing to one context concurrently claim the same slot: one record lost,
   one slot written twice, **no error anywhere**. It had not shown up because every test drove a single writer and the
   acceptance cases have too few peers at too low a rate for a collision to be likely - **which is the worst kind of not
   showing up**, and precisely why this item's test has to contend the claim deliberately.
5. **No reuse before release** - a record a reader still holds is not overwritten even when the ring wraps. This is the
   lending contract, and it is the one whose failure is silent corruption rather than an error.
6. Capacity exhaustion is counted and warned about once, never silent.
7. A reader that exits without releasing does not park a record forever.
8. An owner whose registry entry is gone leaves a segment the next context reclaims.

**Two processes, on Linux, in the acceptance suite**

9. `samehost` gains a shared-memory arm and asserts the transport, with CycloneDDS as the control as everywhere else.
10. Every acceptance case that passes over UDP passes unchanged with the module on - the suite run twice, not a new suite.
11. **The reader is killed mid-run** and the writer keeps making progress, with the segment reclaimed afterwards. A kill
    test, not a code reading: `MODULE_PLAN.md` criterion 4 is not satisfiable any other way.

**The build matrix, which is where tonight's evidence changed the list**

12. **CI builds and runs the suite with the feature on AND off.** On 2026-09-29 the same-host flag's default was flipped to
    on, and the tsan target failed to **link** - that harness defines its own HAL and had no stubs for the feature's three
    entry points. It had never covered the feature at all, and nobody knew, because the flag was off. **A flag-gated
    feature with no CI arm that enables it is an untested feature**, and the arm has to run the suite rather than only
    compile it.

**Performance** is stages 1-3's own criteria in section 5 and is not repeated here: beat loopback UDP, do not beat g9,
and read every timing and RSS figure against the floors in `WIRE_PLAN.md` 10.4.

## 6b. Stage 1's results so far (Dev, 2026-09-29)

**S2, transport identity, ALL PASS on both shapes** - the assertion that had to exist before the code:

    p1: PASS(sample_path=datagram sent=51863 tx_shm=108700 tx_udp=5108 all named)
    p4: PASS(sample_path=frag     sent=15110 tx_shm=65382  tx_udp=3793 all named)

Both shapes, so the seam is wired for the single-datagram and the batch/fragment paths alike rather than for one of
them - which is what a per-shape assertion buys over a mode flag. "all named" is the invariant: every UDP datagram
has one of the four by-reason counters against it.

**The external control confirms it by absence.** `wire_tx_packets` is the kernel's own count for the interface, and
on the first crossing run `tx_udp=4547` against `wire_tx_packets=4548` - drift 1 - while `tx_shm=98303` was invisible
to it. The datagrams claimed for UDP are the ones the interface saw, and the rest demonstrably did not go that way.
That comparison is two-sided: a datagram wrongly counted as shm makes `tx_udp` fall below the interface count, one
wrongly counted as udp makes it exceed.

**The acceptance suite passes with the module on, and the module engaged** (item 10). All thirteen cases pass, with
`itype`'s control failing for the reason already recorded (CycloneDDS reports a type mismatch as INCOMPATIBLE_QOS).
The second half matters as much as the first, because a suite that passes with the transport permanently inert says
nothing - so the segment's use is read off the same traffic line:

    graph tx_shm=74   bag tx_shm=154   events tx_shm=114   matched tx_shm=25
    samehost tx_shm=196 (against tx_udp=95)   durable tx_shm=552   introspect tx_shm=25
    inprocess tx_shm=1 - correct: nodes in one process deliver locally, not over a transport

**Byte identity and the anti-bypass test** (items 2 and 3) are core unit tests with mutants: flip one bit into the
slot and byte identity fails; have the drain count an arrival and skip the acceptance path and the wrong-version
datagram is no longer refused.

**The regression the campaign caught, and the one the tests then caught (2026-09-29 evening).**

Plan measured cell 1 (reliable_throughput p1) after the seam landed and found TickLE's cross-host throughput
**halved** - 114.62 Mbps on `9dbffd40` in the morning against 52.88 on `d4413383` - with CPU per sample doubled,
while CycloneDDS was flat across the same runs on both metrics. The control settled it on its first block: the same
commit with `-Dtt_SEGMENT_ENABLED=0` came back at 114.19 Mbps, within 0.4% of the morning's figure, against 52.35
with the segment on. The binaries' hashes differed, so the flag reached the compiler - a check the harness makes
because "the flag changed nothing" and "the flag never arrived" otherwise look identical.

The cause was in `peer_segment()`, and its own comment was the specification it failed to meet: *"a peer that
appears and disappears costs one attempt rather than a subscription to its lifecycle"*. Only the **positive**
answer was cached. The failure path wrote nothing to the entry, so a peer on another host - where a segment can
never exist even in principle - was asked again for every datagram, an `open()` that walks `/dev/shm` and fails,
about 87,000 times a second per sender. The fallback was working exactly as designed: `tx_udp_unattached` was
433,875 of 433,881. **The cost was not the fallback, it was asking again.**

Two things follow, and the second is the more important.

**The fix is a cache with an expiry on both answers, not on one.** A "no" is remembered for
`tt_SEGMENT_ATTACH_RETRY_SENDS` (256) sends, so a peer that binds later still becomes reachable. A "yes" is
remembered for `tt_SEGMENT_REVALIDATE_SENDS` (4096) sends, and that half was found by the tests for items 7 and 8
rather than by the rig: **an owner that is killed leaves its region mapped, intact, and carrying the incarnation
the peer recorded, so nothing inside a mapping can ever say its owner has gone.** The incarnation check reads the
header *through the mapping we already hold*, which is the old file. Only asking the name again has a different
answer. Without that expiry a peer writes into an orphan for as long as it lives, counted as `tx_shm`, delivered
nowhere - and the first version of item 8's test asserted the incarnation would catch it, which is how this
surfaced.

For an owner killed with **no successor**, even asking the name again finds the same orphaned file, so there is a
third signal: a ring that refuses `tt_SEGMENT_FULL_STREAK_ABANDON` (1024) sends in a row with nothing in between.
A reader that is merely behind breaks the streak with a single drained slot; one that is dead never does. The
peer then gives the mapping up and reaches that peer over UDP, which is where it still is - it was the reader that
died, not the link. That is item 11's property made testable without the rig, and the local run confirms the
control arm too: at 697 Mbps a genuinely slow reader produced 382,083 full-ring fallbacks and triggered the
abandon exactly **once**, recovering 256 datagrams later.

**And the row could not have shown any of it, which is the part worth fixing permanently.** `segment_attach[]`
already counted every attempt by outcome and nothing published it. Had `shm_attach_absent` been on the RESULT
line, "436,722 absent attempts for 436,728 datagrams" would have been on the face of every row from the day the
seam landed. It is there now - `shm_attach_attempts`, `shm_attach_ok`, `shm_attach_absent` - by the same argument
that put the four by-reason fallback counters there: **the fallback counters exist so a UDP datagram can never be
uncounted, and an attach that is retried deserves the same.** The comparison is one glance: attempts should be a
small fraction of datagrams, never one per datagram.

`examples/perf_hil/experiments/shm_attach_counters_local.sh` reads the line back in a private netns rather than
trusting that a field which compiles also prints - the `fallbacks` buffer was 160 bytes and nearly full when these
three were added, and a truncated field would have reproduced exactly the blindness they were added to remove.

**Items 6, 7 and 8 are done, with a mutant apiece** (`tests/mutants_shm_stage1.py`, six mutants, all die):

- **6, capacity exhaustion counted and warned once** - and the warning carries the ring's shape and how much got
  through before it first filled, because that number is what separates the two causes. A ring too small for the
  load fills after about as many records as it has slots; a ring whose slot sequences were never seeded is a ring
  of **one** and fills after the first. Both move `segment_full_to_udp`, and reading the second as the first sends
  you to `tt_SEGMENT_BYTES` for a bug that is in `create_own_segment()`. Plan asked for that case by name; the test
  runs both arms and asserts they differ.
- **7, a reader that exits does not park a record forever** - in this topology the reader is the owner, and until
  today `tt_Context_destroy()` unlinked nothing, so every context that ever ran left a file in `/dev/shm`. The leak
  is invisible in a test because `tt_segment_create()` unlinks first and the next context at that name replaces it.
  Its dual is in the same place: a **writer** that dies between claiming a slot and publishing it wedges the reader
  at that index for good, and an empty head and an abandoned one are identical from the slot alone. The owner now
  counts consecutive drain passes that find something outstanding and nothing readable, and says so once.
- **8, a segment left by a dead owner is reclaimed** - `tt_segment_create()` unlinks before creating, so the
  successor gets a new region with a new incarnation and the orphaned records do not come with it.

**The defect that mattered most, found in CI's own numbers twelve hours after it shipped (2026-09-29).**

Main's CI went red at `1c69658a`, "a datagram crosses the segment", and stayed red for ten commits because nobody
looked - the rule to check every workflow after every push exists for exactly this and was applied to the workflows
that seemed relevant instead of to all of them. Two tiers failed, both on services (`test_service_roundtrip`,
`test_event_callbacks`, and `set_bool` at 17/20), and a third tier **passed** while collapsing:

    same-host perf, from each commit's Test all
    dbd9a932  (last green)                     recv 5,309,077   dropped          0   loss  0.0%   1,017.9 Mbps
    1c69658a  (a datagram crosses the segment) recv 3,751,560   dropped          0   loss  0.0%     719.3 Mbps
    54c27bbc  (the module engaged)             recv   768,104   dropped 30,975,424   loss 97.6%     147.3 Mbps

The same-host cell is *the* cell this module exists to win, and with the module engaged it was eight times worse
than our own UDP loopback had been without it. Plan found this by reading the numbers inside a tier that reported
PASS; its criterion is `perf_server received N message(s) (need >= 5)` against a run that normally delivers 5.3
million, which is a check that cannot fail.

**The cause was one line above the RESULT, in the subscriber's own delivery stats:**

    rx_shm=36,125,590   rx_udp=892,835
    delivered=895,209   out_of_order_discarded=36,123,071

Every datagram that arrived over the segment was discarded, and `delivered` is `rx_udp`. Not a full ring, not
corruption: **the fallback itself.** A datagram rerouted to UDP because the ring was full arrives *ahead* of the
records already queued in that ring, because the socket does not wait for the reader's next drain. The subscriber
delivers the newer one and then discards every older record behind it. One logical stream carried over two paths
of different latency reorders itself, and best-effort delivery discards the loser by design.

**So a full ring now drops the datagram instead of rerouting it.** A full queue drops - which is what the kernel's
socket buffer was already doing on the UDP path - and the reliable path's own retransmission covers it. The
alternative considered and rejected was to keep the fallback and have the reader drain the ring to empty before
every socket read: that fixes the ring-behind case and breaks on the refill, where a record written *after* a UDP
datagram is delivered *before* it. Every rule that lets a stream change path under load has a window like that,
and the only sound version is not to change path.

`segment_full_to_udp` is therefore `segment_full_dropped`, and **S2's invariant is three reasons, not four**:

> `tx_udp == tx_udp_broadcast + tx_udp_oversize + tx_udp_unattached`

with `shm_full_dropped` printed beside them and deliberately outside the sum. It is not a fallback any more, so
including it would make the invariant false - and it is now the only place a full ring shows up at all.

**Two mixed-stream cases remain, named rather than left to be found the same way.** A service request or response
larger than a slot still goes by UDP (`tx_udp_oversize`) while smaller datagrams to the same peer go by segment;
services are request/reply and largely self-paced, so the risk is low but real. And the first datagrams to a peer
go by UDP until the attach succeeds, then switch - safe in practice because the ring is empty at that point, but
by circumstance rather than by construction.

**The service failures are a second, independent defect: the segment has no notification.** `node_poll()` drains
the ring once at the top of the call and then blocks in `tt_receive()` on the socket. Nothing a peer writes into
the segment can end that wait, so a record placed there is invisible until the wait ends - and the wait is bounded
by the next scheduler entry, not by any small slice. The claim in the code that the cost is "up to one poll period
of latency" was wrong: for an idle node there is no poll period, there is the next timer. A service client blocked
on a reply that crossed the segment sleeps until something unrelated falls due, which is inside the deadline
sometimes and not others - 17 of 20. Plan established it is a race rather than a logic error by a natural control:
`1ac62742` passed 20/20 and its parent `54c27bbc` failed 17/20 with a **byte-identical** `set_bool` binary, the
only changes between them being a zenoh harness file and this document.

The fix is a doorbell rather than a polling slice, since a slice would trade correctness back for the idle wakeups
the poll loop was rebuilt to remove. The owner publishes a `reader_waiting` flag in its segment header, set
immediately before it blocks and cleared when it wakes; a writer that has just published a record re-reads that
flag and, only if it is set, sends a zero-length UDP datagram to the owner's data port - the one descriptor the
reader is already waiting on. Under load the reader is never blocked, so the flag is never set and the doorbell
costs nothing. A zero-length datagram is not a valid TickLE datagram under any circumstance, so the receive path
drops it before the magic check and counts it: no new parsable wire form, nothing another implementation can
observe. The flag is checked *after* the write, which is what closes the window where the reader sets it between
the writer's check and the writer's write.

**And the defect that was actually stopping the suite was neither of the reordering ones: uninitialised memory.**

`tt_Context` is caller-owned and `reset_node_state()` initialises field by field. `segment_peers[256]` and
`own_segment` were added to the struct and not to that function, so a context began life with whatever was on the
caller's stack in those fields - and `release_segments()`, added the same evening, walks all 256 entries at teardown
and calls `munmap()` on every non-NULL one:

    SEGREL peer id=1   mapping=0x71ff46f0d000   <- the one real mapping
    SEGREL peer id=98  mapping=0x279f8
    SEGREL peer id=104 mapping=0x3
    SEGREL peer id=124 mapping=0x40

It unmapped parts of its own process at random, and every node in `make test-linux` segfaulted at exit. **This
section already records this exact trap** - "a field added to this struct and not to this function is never zero" -
about the per-transport counters, where it produced `tx_udp=140723338891185`. The counters only lied. These are
pointers, and the consequence was a crash in a different place each time.

No unit test could see it, for a reason worth keeping: every test in `tests/test_transport_seam.c` memsets its
context before use, which is exactly what a real caller is not required to do. The one arm that *did* fill a context
with 0xAA asserted only on the counters, because that is what it was written for. It now asserts on `own_segment`
and on every `segment_peers[]` entry, and there is a mutant for it.

**Four attempts before that each looked like an answer, and the cheap one worked:**

- **gdb on one side of the pair.** The traced process never crashed and the untraced one always did, in *both*
  directions. Tracing moved the timing, so "which side crashes" was an artefact of where gdb was attached.
- **`CFLAGS=-fsanitize=address` passed to `make test-linux`.** `platform/linux/test.sh` re-invokes make itself
  without the caller's CFLAGS, so the examples were never built with ASAN. The run produced a bare "Segmentation
  fault" and no report - **a sanitizer that was never linked, whose silence read exactly like a clean run.** The
  reproduction script now refuses to run unless `ldd` shows the sanitizer in the binary.
- **A first fix to `release_segments()`** for a double-unmap that exists only under the mock's shared-region
  semantics; the real HAL maps a fresh region per attach, so it was a fix for a bug that was not there. The crash
  continued and that read as "a second cause" rather than "wrong cause".
- **A false failure from the mutant harness itself.** Its restore used `shutil.copy2`, which preserves the original
  mtime, so the restored file looked older than the objects built while the mutant was applied and `make` considered
  the build up to date. The next `make test` ran a binary compiled from mutated source and reported four failures
  that were not in the tree. Fixed with `copy()` plus a touch.

What settled it was the cheapest possible step, taken last instead of first: **disable `release_segments()`
entirely, re-run the suite, count segfaults.** Zero. Then markers inside the function. No reading and no inference,
and it took two minutes against the hour the readings took.

**What the counters said once they were on the line, and the rule that got three versions wrong.**

Putting the four by-reason counters into the node's own traffic log - the line every integration run already
prints - answered in one run what two runs of reasoning had not:

    tx_udp=2,446,989  tx_shm=2,475,524
    tx_udp_broadcast=141  tx_udp_oversize=0  tx_udp_unattached=2,446,848  shm_full_dropped=9,819,292

Two things at once. The ring overruns badly - 9.8 million datagrams dropped against a UDP arm that dropped
**none** - and 2.4 million took the *unattached* fallback on a run whose peer was alive from start to finish.
The second is the module abandoning a healthy peer and re-attaching, over and over, which puts one logical
stream on two paths all by itself: the thing drop-on-full exists to prevent, reintroduced by the heuristic
that was supposed to handle a dead reader.

**The dead-reader rule took three attempts, and the first two were each found by a mutant surviving rather
than by anyone noticing:**

1. *N consecutive refusals.* Measures the writer, not the reader. At same-host rates a healthy reader goes
   thousands of our sends between two of its own poll passes, so the threshold says more about our send rate
   than about whether anyone is listening.
2. *N consecutive refusals with `read_index` unmoved.* Added to fix (1), and **unreachable**: any movement by
   the reader frees a slot, so the very next write succeeds and resets the count anyway. The added condition
   could never be the thing that decided. Its mutant survived, which is the only reason that was noticed - and
   the first response to that was to add a *timed* version of the same unreachable idea, whose mutant also
   survived.
3. *Time since we last managed to put anything in the ring.* One line, and the discriminator was always there:
   a success resets the clock, a reader that has taken nothing for `tt_SEGMENT_DEAD_READER_NS` has stopped,
   and one that is merely behind frees a slot now and then. No count, no index.

The lesson is not about segments. **A condition that cannot decide the outcome reads exactly like one that
can**, and the only thing that told the difference here was asking whether removing it changed any test. Twice
the answer was no, and twice the code had already shipped in my head as "fixed".

**The third defect, and the one that made the first two look like they had made things worse.** With ordering
fixed and the flapping gone, the cell collapsed further rather than recovering: 73,081 records delivered against
UDP's 12,463,222, 12.2 Mbps, **241 ms** of latency, and 66 million dropped by a full ring. The ring had no
back-pressure and looked like a sizing problem.

It was not sizing. **The segment has no way to wake a reader that is blocked on its socket** - `node_poll()`
drains the ring at the top of the call and then waits on the socket, and nothing written into shared memory can
end that wait. As long as some traffic still went by UDP the reader kept being woken by accident; once the first
two fixes put *everything* on the segment, the socket went quiet and the reader ran only on its timers. 241 ms is
that timer. **The doorbell looked optional for exactly as long as a separate defect was doing its job for it**,
which is the clearest example this project has produced of a component whose absence is invisible while something
else accidentally covers for it.

The fix is a doorbell rather than a polling slice, which would have traded correctness back for the idle wakeups
the poll loop was rebuilt to remove. The owner sets `reader_waiting` in its own segment header immediately before
it blocks and clears it when it wakes; a writer reads that flag **after** it publishes a record - which is what
closes the window where the owner sets it between the writer's check and the writer's write - and only if it is
set sends a **zero-length UDP datagram** to the owner's data port, the one descriptor the reader is already
waiting on. The owner drains once more after setting the flag, closing the window the other way. Under load the
reader is never inside a wait, so the flag is never set and the doorbell costs a relaxed load per send. A
zero-length datagram is not a valid TickLE datagram under any circumstance, so the receive path drops it before
the magic check: no new parsable wire form and nothing another implementation can observe. `tt_SEGMENT_VERSION`
goes to 2, because a peer built against version 1 would read the ring correctly and never set the flag.

**Stage 1 now wins the cell it exists to win.** Paired arms in one run, same tree, differing only by
`-Dtt_SEGMENT_ENABLED=0`, library hashes `cd9b5f7f` and `d77f4882`:

    arm   perf RESULT                                                          discards
    on    recv 16,501,477  dropped 10   loss 0.0%  3,163.883 Mbps  0.007 ms     0
    off   recv 12,166,049  dropped  0   loss 0.0%  2,332.637 Mbps  0.008 ms     0

**+35.6% throughput at equal loss and equal latency**, against a starting point this evening of 147 Mbps and
97.6% loss. `tx_shm=19,213,752` against `tx_udp=102`, and those 102 are all broadcast - the one shape that can
never take a segment. `shm_full_dropped=10` in a sixty-second run, so the ring is not the constraint once the
reader is woken when there is something for it.

The three fixes only work together, and the order in which they were found is the wrong way round: each of the
first two made the measured cell *worse* on its own, because each removed traffic from the socket that had been
waking the reader by accident.

**CI, after `add73df8`.** `Test all` is green for the first time since `1c69658a` twelve hours earlier, and it
passes Plan's new perf tier on its own criterion rather than on the old `received >= 5`: loss 0.0% and
3,163.883 Mbps against a 2.0% / 400 Mbps floor. The rmw suite is green too - `test_service_roundtrip` and
`test_event_callbacks`, the two service tests that had been failing all day, both pass, and the summary is
**68 tests, 0 errors, 0 failures**. Those were the doorbell's doing: a service client blocked on a reply that
crossed the segment had been sleeping until some unrelated timer fired.

One failure remains, in `Check all` and nowhere else: the **rclcpp default-node publisher segfaults** in
`check_ros2_interfaces.sh -r`. The plain rmw-level path passes in the same run ("PASS - both types
round-tripped"), so it is specific to a node that also starts parameter services, `/rosout` and the type
description service. `Check all` was already red on every parent commit, so this is not a regression to revert
but the next thing to fix.

**A caution recorded because it nearly became a fourth wrong turn.** The failing run logs
`Segment ring of context 122 is full: 256 slots of 1472 bytes, first full after 9 datagrams`, and "full after
9" reads exactly like the mis-seeded ring this warning was written to describe. It is not. The count is *this
node's own* shm datagrams, and a segment is written by many peers - so a node can see the ring full after nine
of its own writes while other peers supplied the rest. The two builds' geometry was checked by printing it
rather than by reasoning about it, and both are 256 slots of 1472 bytes. **The warning's own wording is the
defect here**: "filling after one or two is a ring that was never seeded" is true only for a single writer, and
this ring is many-writer by construction. It should report the ring's occupancy, which is `write_index -
read_index` and belongs to nobody, instead of a number that belongs to whoever happened to be logging.

**Debugging notes for this module, put first because both of tonight's races cost an hour before anyone
reached them.**

1. **A timing-dependent defect cannot be observed by an instrument that changes timing.** Twice tonight, on
   two unrelated crashes, the process under gdb did not crash and the untraced one did - *in both directions*,
   so "which side crashes" was a fact about where gdb was attached and nothing else. This is not "gdb is
   unreliable": it is that for a suspected race the first move is to **remove the suspect and count**, not to
   watch it. Disabling `release_segments()` and re-running took two minutes and named the function; markers
   inside it named the line. Both times that was reached fourth.
2. **A sanitizer that was never linked is silent, and its silence reads exactly like a clean run.**
   `platform/linux/test.sh` re-invokes `make` without the caller's `CFLAGS`, so `-fsanitize=address` passed to
   `make test-linux` never reached the examples. Check with `ldd`, not with intent.
3. **Restore a source file with something that changes its mtime.** `shutil.copy2` preserves it, so `make`
   considers objects built from the mutant up to date and the next run reports failures that are not in the
   tree.
4. **Read the numbers inside a passing tier.** The same-host perf tier reported PASS through 97.6% loss and an
   8x throughput collapse for most of a day, because its criterion was `received >= 5` against a run that
   normally delivers 5.3 million.

**The defect behind every `Check all` failure, and it was never the ring: the drain held no lock.**

`drain_own_segment()` calls `process_datagram_locked()` - a function whose name states its contract, and whose
socket-side twin `drain_rx()` wraps the same call in `state_lock`/`state_unlock`. It did so **unlocked from the
commit the segment landed in**, `1c69658a`, which is exactly where `Check all` went red and stayed red.

**Nothing caught it because every test that drives the drain is single-threaded.** The core integration suite
runs one thread per process, so the race had no second party. `rmw_tickle` has an executor; there it corrupted
state until the publisher segfaulted. The first symptom was the service tests failing, the second was
`check_ros2_interfaces.sh -r` crashing, and both were the same unlocked write.

Fixed by taking the lock in chunks of `tt_RX_LOCK_CHUNK`, which is what `drain_rx()` already chose so a
publishing thread waits at most a chunk. The lock is re-entrant, so the call sites that already hold it are
unaffected.

**Reproduced and verified off CI, before and after, on the same machine**: `check_ros2_interfaces.sh -r`
segfaulted at 22:40 and returned `check_ros2_interfaces: PASS`, exit 0, at 22:44, with only this change between
(`rmw_tickle/scripts/night_repro_rclcpp_segv.sh`). Two earlier attempts to catch it under gdb failed the same
way an earlier crash had: **the traced process never crashed and the untraced one always did**, because tracing
moves the timing. What worked both times was the cheap thing - disable the suspect entirely, re-run, count
crashes - and it should have been first, not fourth.

**The gate that should have caught it, and now does.** `test_thread_safety` is the one binary in this project
that runs threads against the core, and its HAL returned NULL from every segment entry point - so it had been
passing while never creating, attaching or draining a segment at all. That is SHM_PLAN item 12's "a flag-gated
feature with no arm that enables it is an untested feature", in the tsan gate, costing exactly the defect it
existed to prevent.

Its stubs are now backed by real shared regions, and a second reporting bug had to be fixed first: the harness
told a node its own address was `0.0.0.0:0` while reporting that node's datagrams as coming from
`10.0.0.<id>:2000<id>`, so a context named its own segment from one address and its peers looked for it at
another. Every attach returned ABSENT. **A harness that states a node's identity two different ways makes a
module invisible without failing anything.**

With both fixed the gate reports `attach[ok=20 absent=0]` and `tx_shm=80333 rx_shm=80333`, and the run asserts
those are non-zero rather than trusting them - a run where the segment is inert proves nothing about it under
threads and would read exactly like a clean one. **Watched failing before being trusted**: with the lock
removed, `WARNING: ThreadSanitizer: data race` and exit 2; with it, the suite passes.

**A correction to what that harness change was said to expose (2026-09-29, same night).** It was described
here and in `f938461e`'s commit message as making the mixed-stream reordering visible. **It does not.**
Instrumenting each gap with its direction gives every one as FORWARD - a sample that did not arrive - and
**not one BACKWARDS**. Those publishers have no `reliable` set, so they are best-effort, and a bounded queue
that is full drops, which is what the ring does and what a real UDP socket does.

**The harness's own fake transport does not drop: `push()` blocks on a condition variable when its 256 slots
are full.** Same depth, opposite policy. So the assertion that stood there - no gaps at all - was only ever
satisfiable by the harness being lossless. It was testing the harness and not the module, which is how it
passed for the module's whole life while the module was never engaged at all.

A second error in the same reading, recorded because it is the kind that flatters: the loss was first reported
as "at most 0.19%" from counting gap *events*. A single gap skips many samples. Measured properly it is **29
to 2,281 of 20,000 per thread, 0.15% to 11.4%** - wrong by a factor of sixty, in the reassuring direction.

**A second assertion in the same harness had the same defect and was found the hard way (2026-09-30).**
`received[t] == SAMPLES_PER_THREAD` - "the last sample arrived" - is also a no-loss claim, about the tail
rather than the middle, and best-effort promises nothing about it either. It survived because `push()` blocks,
and it began failing **about one run in four** once the segment was engaged: 8,215 of 20,000 in one gate run,
15,088 in another. It went in with the harness change and came out a commit later, after a flaky gate caught
it. **A gate that fails one run in four is worse than no gate, because what it teaches is to run it again.**

**So no delivery floor is asserted there.** Any line inside a 0.15-11.4% range is a number nobody can derive,
which is this document's own objection to "tx_udp small and flat"; the figure is printed for a reader instead.
**The repair suggested here twice - make `push()` drop rather than block - was tried on 2026-09-30 and reverted
the same hour, because the measurement said so.** Both halves of the argument for it turned out to be wrong:

- **It does not make a floor derivable.** With both paths dropping at the same depth the loss is a function of
  thread scheduling rather than of either policy, so no threshold follows from the design. Exact accounting does
  not save it either: `push()` drops control datagrams as well as samples, and reliable retransmission means a
  dropped datagram is often a delivered sample. That claim was made here before it was checked, and is
  withdrawn.
- **And it makes the harness a worse instrument.** Blocking is what flow-controls the publisher threads.
  Without it they run unthrottled: across six runs five delivered all 20,000 samples per thread and one
  delivered 7,151, with the new drop counter at zero - so that run exercised a third of the traffic and the
  loss was not even from the new path. A harness that exercises less, and varies in how much, is worse than an
  unfaithful one whose unfaithfulness is written down.

The two assertions that were testing the harness were fixed on their own terms and needed no change here. What
the mismatch leaves behind is a comment at `push()` saying it was measured rather than missed, and the paired
comparison - a module-on arm against a module-off arm in the same harness - as the honest substitute for a
floor.

**And the ordering assertion there is not evidence, which was checked rather than assumed.** With the
pre-2026-09-29 reroute-past-a-full-ring behaviour restored - the thing that actually reordered the stream on
the rig - `backwards` stayed at zero across four runs. In that harness `push()` hands a datagram to the same
queue the peer reads while the ring is drained to empty first, so ring records always precede queued ones; on a
real socket the socket is drained to exhaustion while the ring lags, which is where the reordering came from.
The assertion stays because it costs nothing and is labelled in the source as not a guard.

**What that harness does falsify, both watched failing:** the data race (without the state lock,
ThreadSanitizer reports one and the run exits 2) and the engaged check (with the NULL stubs restored, the
`tx_shm > 0` assertion fails). Those two are its contribution, and they are the reason it lands.

**A tunable that could not be tuned (Plan, measured).** `tt_SEGMENT_ATTACH_RETRY_SENDS` and four other segment
constants were bare `#define`s with no `#ifndef`, so `-D` overrides were silently discarded by the header. Plan
found it trying to measure the retry's cost: the two arms came out byte-identical and the harness refused to
report rather than concluding the retry was free. All five are guarded now, and the override is verified by
building with one rather than by reading the header.

**Items 9 and 11 (2026-09-30), and a defect shipped in `f938461e` that only item 11 could find.**

**Item 9** - `t_samehost` now reads `rx_shm` from the listener's own traffic line and fails when it is zero,
**and fails when the line is absent at all**. The second half is the point: "the module was not engaged" and
"the instrument was not there" read identically, which is how the tsan gate stayed inert for the module's whole
life. CycloneDDS prints no such line, so the control is untouched.

**Item 11** - `examples/perf_hil/experiments/segment_reader_kill.sh`: two core processes in a private netns,
the reader killed with SIGKILL mid-run. It PASSES with the numbers agreeing for the first time:

    shm_gave_up=3                  the writer judged the reader dead and gave the segment up
    shm_full_dropped=3,380,492     the ring filled and stayed full, nobody draining it
    tx_udp_broadcast=2,615,288     no peers left, so it broadcast
    writer progress 8 -> 21 lines, still alive
    successor reader recv=2,268,247  a fresh reader took the name over

**And it found that the dead-reader rule was unreachable as shipped.** `peer_segment()`'s revalidation memsets
the cache entry, which cleared `last_progress_ns` - so the clock restarted every `tt_SEGMENT_REVALIDATE_SENDS`
sends, about seventy times a second on a writer sending three hundred thousand datagrams a second, and could
never reach one second. **This is the fourth condition in this module that could not decide anything**, and the
first to reach main. The clock is now carried across a revalidation when the address is unchanged, since a
revalidation is bookkeeping about the mapping and says nothing about whether the reader is consuming.

Necessity established by control rather than by argument: with the fix reverted, `shm_gave_up=0` and the test
FAILS; with it, `shm_gave_up=3` and it PASSES.

**Three defects in the test itself, all of which made it report PASS on nothing**, recorded because each is a
shape this project keeps meeting:

1. **`sudo -n kill -9` never killed anything.** The sudoers rule here covers `ip netns` and nothing else, the
   reader runs as this user anyway, and the failure went into `2>/dev/null`. Three runs reported on a reader
   that completed its full forty seconds - the reader's own log said `40.001 sec`. **An action that silently
   does not happen reads exactly like one that did.** The target's identity is now checked against
   `/proc/PID/exe` before the kill and its absence after, and either check failing makes the run VOID.
2. **The verdict started at PASS** and fell through to it whenever a comparison errored - which they did,
   because `grep -c` prints 0 *and exits 1*, so `|| echo 0` made every count the string `"0\n0"`. It now starts
   at VOID and is only lowered once every question has answered with a number.
3. **The assertion "the writer gave the dead peer up" was tested against `tx_udp_unattached`**, which rises for
   every ordinary reason a peer has no segment. A build whose dead-reader threshold was raised to an hour -
   which can never abandon anyone - produced 196,962 against the real build's 203,318. The counter that rises
   only in the abandoning branch is now on the traffic line as `shm_gave_up`, beside `shm_doorbells_sent` and
   `shm_doorbells_received`.

**And a finding from the same run, since fixed: `shm_doorbells_sent=2,853,609`.** A reader killed while
blocked leaves `reader_waiting` set in its own header and nobody clears it, so every writer rang a doorbell - a
real `sendto()` - for every datagram it sent to the corpse.

**The rule: a doorbell that was not answered is not rung again.** The writer remembers the peer's `read_index`
at its last ring and does not ring while it has not moved. A live reader drains before it blocks again, so the
index has always moved and the next record rings, which is the case the doorbell exists for; a dead one leaves
it where it was for ever and is rung exactly once. Under load the question never arises, because a reader that
is not blocked never sets the flag.

Measured on the same SIGKILL run: **2,853,609 -> 728,513**. The remainder is the doorbell working rather than
waste - it is the live phase, where each ring wakes a reader that had gone to sleep, and the alternative to
that is the 12.2 Mbps and 241 ms this module had before the doorbell existed. Read as phases, the ~2.1 million
removed are the twenty-one seconds after the kill, which is what the rule was for; that split is inferred from
the two totals rather than measured per phase.

**A bug the test caught on the way in, worth keeping because it is the same shape as the rest:** the first
doorbell did not ring. `doorbell_read_index` and a fresh ring's `read_index` both start at zero, so "we have
already rung at this index" was true before anything had been rung. An explicit `doorbell_rung` flag says
"never" rather than encoding it as a coincidence of zeroes. The control arm - a reader that drains, after
which the next record must ring again - is what stops the whole assertion passing for a doorbell that had
simply stopped working.

**Still owed:** the p1-p4 numbers against WIRE_PLAN 10.4's floors - **not measurable until Plan re-measures cell 1
with the fix in**, since anything measured against a build paying 87,000 failed syscalls a second would credit the
segment for removing an artefact - and items 9 and 11 of section 6a. Item 4 is done (`71e5ca86`).

**A cost to carry into the budget, measured by Plan in the same control:** peak RSS is 2,388 kB with the segment
and 2,004 kB without, so the own-segment mapping costs about 384 kB resident. That is not part of the regression
and is expected from the 512 KiB reservation, but it is a real cost of the module being on by default.

## 6c. What the module changes about behaviour under load, as a property rather than a caveat

Everything in 6b is a defect found and fixed. This section is not that. It is what the module **is**, and a
user deciding whether to turn it on needs it before they see any throughput figure.

**Under load, this module's failure mode is silent sample loss. UDP's is lower throughput.** Both are legal for
a best-effort reader and they are not the same proposition.

The ring is bounded - `tt_SEGMENT_SLOTS` slots of `tt_SEGMENT_SLOT_BYTES` - and a full ring **drops**, because
the alternative is worse in a way that was measured: rerouting the datagram to UDP puts it ahead of the records
already queued in the ring, the reader delivers the newer one and discards everything older behind it, and that
cost 97.6% of delivery on the same-host cell. So drop-on-full stands, and it is the right repair. What it means
is that a reader which falls behind loses samples where the same reader on UDP would simply have slowed the
sender down, because the kernel's socket buffer absorbs the same starvation.

**Measured, in the one place the two policies could be compared directly.** A CI run on 2026-09-30 produced
both arms one second apart on the same runner:

    Integration Linux            3,128.9 Mbps   7.2% loss   1,258,780 of 20,487,230 samples
    Integration Linux shm-off    1,902.7 Mbps   0.0% loss

The *slower* module-on arm is the one that lost samples, and the faster module-on run in the same pair lost
none - so this is contention, not a rate ceiling. One observation, an uncontrolled cause, no repetitions; it
does not establish a rate. What it establishes is that **the two paths answer a starved reader differently**,
and that is a design property rather than a run's bad luck.

**A second observation, on `7eaad571`, with the accounting closed** - which the run above did not have, because
the counters were not yet on the RESULT line:

    module ON   sent 53,405,295  tx_shm 48,387,684  tx_udp 78  shm_full_dropped 5,017,606
                recv 41,656,984  loss 8.6%   7,987.0 Mbps
    module OFF  sent 22,431,239  tx_shm 0   tx_udp 22,431,313  shm_full_dropped 0
                recv 19,195,623  loss 0.0%   3,680.4 Mbps

48,387,684 + 78 + 5,017,606 = 53,405,368 against `sent` 53,405,295 - 73 apart, in flight. **So every one of the
five million lost samples was refused by a full ring before it was ever sent, and that is the whole of the
8.6%.** Nothing was lost in transit and nothing is unaccounted for.

Two observations now, on different days and different commits, and they agree on the shape rather than on a
rate: **the module roughly doubles throughput and loses samples where the kernel path loses none.** The rate
differs with contention, as it must, and neither run establishes one.

**Three things follow that are worth stating plainly:**

- **It is visible.** `shm_full_dropped` counts every datagram a full ring refused, and it is on the node's
  traffic line and on the perf tier's RESULT line. A row that loses samples this way says so. The hole worth
  knowing about is that a *subscriber's* `loss_pct` cannot see it - a refused datagram is never sent, so it is
  not a gap in anything the reader receives. On 2026-09-30 a row of mine read `loss_pct=0.0` beside
  `shm_full_dropped=54,328`: 0.513% of that stream was never sent and the delivery check was silent about it.
  Both numbers were right; only together are they informative.
- **The fix is not a larger ring.** Raising `tt_SEGMENT_BYTES` moves the threshold rather than measuring it,
  and it would have made the failing run above pass. The counter is what lets the real threshold be found.
- **Whether a slow reader should get back-pressure instead is a design choice and not a defect to be fixed
  quietly.** Making a writer wait and letting a reader lose samples are both legitimate, and choosing between
  them is choosing what this module promises. It belongs with the user's decision 6, not in a commit.

## 6d. Lazy creation: the segment's lifetime is the span in which a peer could open it (Dev, 2026-09-30, `8cf8cdc9`)

Until this change a context created its segment at bind. That is the earliest moment its name can be built -
the address comes from the bind and the context id from just above it - and it was the wrong moment, because
it makes the segment's lifetime the context's lifetime rather than the span in which anything could use it.
A deployment whose peers are all on other hosts paid for a **379 kB** ring (256 slots of 1,472 bytes) that
nothing could ever attach to. That figure agrees with the off-arm/on-arm RSS pair measured at p2, ≈1,743 kB
against ≈2,143 kB, which is the same ≈400 kB from the other direction.

**Why the user chose this shape.** Discovery already reports both edges. It knows when a peer appears inside
the same host and it knows when one goes, so the lifecycle is symmetric and the segment can be built when
something can use it and given up when nothing can:

- **Appearing:** `note_same_host_peer()`, from `process_announce()`, keyed on the announcing peer's address
  being our own. A peer anywhere else can never open the file we would create - the name is built from
  (ip, port, context id) - so building one for it would be the eager behaviour under another name.
- **Departing:** `forget_same_host_peer()`, from `presume_node_dead()`, releasing when the **last** such peer
  has gone.

**Self-delivery is a separate claim on the segment, and missing it would have been silent.** A context
unicasts to itself and attaches to the file it created - which is why `release_segments()` must unmap that
region exactly once. Keying creation solely on *other* same-host peers would have moved self-delivery onto
UDP, and a context alone on its host has no other trigger at all. So `peer_segment()` builds the segment on
demand when asked for our own id, and release refuses while `segment_peers[own id].mapping` is non-NULL.

**Only the liveliness timeout releases, deliberately.** A graceful farewell arrives as an announce, and at
that point it cannot be told from the periodic refresh that also calls
`forget_peers_from_source(preserve_ack=true)` while the node is alive. Hooking that would release and rebuild
the segment under its peers once a second. The cost of the choice made is holding a segment until the silence
limit after a graceful departure; the cost of the other would be dropping a ring somebody is still writing to.

**Releasing is safe but not free, and the cost is worth stating rather than calling it none.** A peer judged
gone by timeout may still be alive and still hold a mapping, and unlinking does not invalidate a mapping that
already exists - so it goes on writing into a file that will never be read. Its own dead-reader rule notices
within `tt_SEGMENT_DEAD_READER_NS` and it falls back to UDP, and its revalidation re-attaches it to whatever
is built next. Being wrong here costs that peer a second on the slower path. It is not loss and it does not
wedge.

### The race this exposed, which was shipped and is fixed here

`create_own_segment()` writes every header field and seeds every slot's sequence, then publishes the lot with
`__ATOMIC_RELEASE` on the magic - the "magic last, magic checked first" handshake section 3 describes. But
`segment_header_check()` read that magic with a **plain load**. A release store pairs with an acquire load and
with nothing else, so a reader could see a valid magic beside a `slots` still zero: a header believed and a
ring that cannot be indexed.

**It was safe only by accident.** Creation at bind finished before any peer could attach, so creator and
attacher never overlapped. Deferred creation makes "a peer attaches while the owner is still building it" the
ordinary case, and ThreadSanitizer reported the race the first time the two could run at once. The load is now
`__ATOMIC_ACQUIRE`; tsan reports the race with the plain load and is clean with the acquire, from the same
instrument.

This is the second time this week a correct-looking release/acquire pair has turned out to be half a pair,
and both halves read as careful code. The general form: **a release store with no acquire load is a comment,
not a barrier.**

### Releasing while the context runs is new, and one thing had to move with it

`release_segments()` only ever ran at teardown. `release_own_segment()` is the first thing that can unmap a
segment **while the context is still going**, and that makes one existing assumption unsafe: `move_id()`
renumbers a live context when two hold one id, and the peer table does not move with it. A context that had
attached to its own segment under its old id still has that entry at the old index, so a release that looked
at `segment_peers[node->id]` would find an empty slot, unmap the region anyway, and leave the old entry
pointing into it - a use-after-munmap, the same class as the teardown segfault that `release_segments()`
was written to avoid.

So both the guard and the cleanup find our own attachment **by identity** - scanning for an entry whose
mapping is our own segment - rather than by index. The unlink path likewise takes the name from the header's
`owner_context_id` and not from `node->id`, because the file carries the id it was created under. A test
renumbers a context that holds its own segment and asserts the release refuses; with the index version it
fails on four assertions, one of them the dangling entry itself.

### What makes the claim testable

The claim is mostly a NEGATIVE one - that no segment is built until something can use it - and a test of an
absence passes just as well against the old eager code. So the controls carry it: a peer on another host
building nothing, a departure that is not the last one, a peer never counted leaving, and a context delivering
to itself keeping its segment. Seven new mutants in `tests/mutants_shm_stage1.py`, twenty in all, every one
dying with the control holding.

`shm_segments_created`, `shm_segments_released` and `shm_same_host_peers` are on the node traffic line for the
same reason: with creation deferred, `tx_shm=0` has two entirely different meanings - "no peer could have used
one" and "one could, and it broke" - and without a counter that rose they leave the same absence behind.

**But putting them there does not make them readable by the campaign, and that was an assumption rather than
a check.** Every `examples/perf_hil/*/run_scenario.sh` pipes both the client's and the server's output through
`grep '^RESULT:'`, so the `Node <id> traffic:` line these counters live on is discarded before anything sees
it. The examples do call `tt_Context_destroy()` and the line is printed - it is filtered out one step later.
So no campaign cell has ever carried a segment counter, and anything instrumented on that line is invisible to
the whole comparison table. Verification of lazy creation needs a harness that keeps the full log and drives
the examples itself (`examples/perf_hil/experiments/lazy_segment_lifecycle.sh`), which is how the rig run below
was done.

The general form is worth stating because it is the same one this document keeps finding: **an instrument is
not wired up until something has read it end to end.** A counter on a line nobody keeps reads exactly like a
counter that stayed at zero. The alternative - carrying these on the `RESULT:` line, where `tx_shm`, `tx_udp`
and `shm_full_dropped` already are - is the smaller change and has not been made yet; it needs a run to verify
rather than a reading.

`test_thread_safety`'s fake transport gave its two nodes 10.0.0.1 and 10.0.0.2: two nodes on two *hosts* that
nonetheless share a `/dev/shm`, which cannot happen. Once creation became conditional on a peer being on this
host, that addressing was what said they were not, and every attach came back ABSENT for a new reason. Both
contexts now have one address and differ by port. `tx_shm` went from 0 to 79,503 and `unattached` from 79,970
to 257 - and the segment was built through the real announce path, not by the test calling in.

### What is now measurable that was not

Section 6c's **pre-attach mixed-stream window** (S9 window B) becomes reachable: with the segment built on a
discovery edge, there is a real interval in which a same-host peer exists, has been announced, and has not yet
attached, during which its traffic takes UDP. Before this change that window existed only at process start.
It has not been measured yet, and this section does not claim it has.

## 6e. The send path: choose the transport before encoding (user decisions, 2026-10-02)

Everything above this section treats the segment as a **datagram** transport: a record carries exactly what a UDP
datagram would, and `tt_SEGMENT_SLOT_BYTES` is `tt_CONTROL_MAX_LENGTH`, which is `tt_ETHERNET_UDP_PAYLOAD`. That is
why the module was cheap to add and why nothing in the wire protocol had to move. S6 then measured what it costs:
at p4 (2800 B) a sample does not fit one slot, so it arrives as two datagrams, and CycloneDDS carries **92.6%** more
on the same cell (COMPARISON 2.2c). The segment inherits a limit the medium does not impose.

This section is the design that removes it. The decisions below are the user's, taken in conversation on
2026-10-02; the measurements they rest on are 2.2c's, and the criteria are written here **before** any of it is
built.

### The four decisions

1. **A sample bound only for shared memory is not fragmented.** There is no MTU in a segment. Fast DDS states the
   same property of its own transport - the only size limit is the machine's memory, and no network fragmentation
   is needed - and it is the reason its p4 cell does not have ours.

2. **The encoder writes into the slot, not into `tx_buffer`.** The goal is **fewer memory writes**, not shared
   memory for its own sake. Today an all-local BEST_EFFORT sample is written three times: encode into `tx_buffer`,
   copy into the slot, copy out to the reader. Encoding in place removes the middle one. The receive side's copy is
   section 4's lending and is staged separately, so a measurement can attribute the win to one or the other.

3. **Mixed destinations use both paths, and the fragmentation decision follows the network.** A publisher with a
   same-host and a remote subscriber cannot have one framing. It fragments as the network requires and the local
   peer receives those same fragments - today's behaviour, so **no regression and no gain in the mixed case**.

   This is not a compromise, it is what keeps `seq_no` correct. Our `seq_no` is **per datagram**, not per sample
   (the user's decision of 2026-09-26, so that a loss costs one datagram's retransmission rather than a sample's;
   `tickle.h` - "this datagram's own; the sample's is `seq_no - frag_index`"). DDS does the opposite: RTPS numbers
   samples and fragments carry a fragment number inside one sequence number. Because ours counts datagrams, a
   sample that went as one record locally and k fragments remotely would have to advance one publisher-wide
   counter by both 1 and k. Advance by 1 and the next sample collides with the remote fragments; advance by k and
   the local peer sees k-1 numbers it will never receive and NACKs for datagrams that do not exist. Matching the
   network removes the question instead of answering it.

   **So the p4 gain belongs to all-local publishers only.** The S6 cell is exactly that shape, so the measurement
   will show it in full; a deployment with both local and remote subscribers on one topic gets nothing. The
   COMPARISON row must say so rather than quote the number bare.

4. **The slot size is the user's, set at runtime.** It makes total memory predictable, which is what we sell against
   a 216 MB pre-allocated daemon, and it lets an application size the ring for its own messages.

### Why runtime costs nothing on the data path

`segment_slot()` already computes its stride from the header:

    size_t stride = sizeof(struct tt_SegmentSlot) + header->slot_bytes;
    return base + ((size_t)(index & (header->slots - 1U)) * stride);

The hot path never used the compile-time constant. `tt_SEGMENT_SLOT_BYTES` appears only in `segment_bytes()` at
attach, create and detach, and in the one store that seeds `header->slot_bytes`. Moving the value into the context's
configuration therefore changes four call sites outside the data path and nothing inside it.

**The attach becomes two steps, and that is the real cost.** Today an attacher maps
`segment_bytes(tt_SEGMENT_SLOTS, tt_SEGMENT_SLOT_BYTES)` computed from **its own** constants, then indexes with the
owner's `header->slot_bytes`. With the size configurable per context those can differ, so the attacher must map the
header first, read `slots` and `slot_bytes`, and then map the whole region. The header becomes the single authority,
which is what it should have been.

**This closes a hole that was here already - measured 2026-10-02 by Dev, and it is worse than the four readings
that preceded it.** `segment_header_check()` validated magic, version, owner and incarnation but **not `slots` or
`slot_bytes`**, while `tt_SEGMENT_SLOT_BYTES` has been `#ifndef`-guarded since 2026-09-29 precisely so it can be
overridden with `-D`. Two builds at `tt_SEGMENT_BYTES` 512K and 1M, geometry verified distinct by a compiled probe
rather than by trusting the `-D`, 2000 messages:

| arm | `tx_shm` | `shm_full_dropped` | exit | subscriber received |
|---|---:|---:|---:|---:|
| control, default against default | 1,842 | 0 | 0 | all |
| **owner LARGER than the attacher** | 259 | **1,535** | **0** | **536 of 2,000** |
| owner smaller | 0 | 0 | 0 | all, over UDP |

**There is no fault.** The writer fills the slots its short mapping can address, then reads a slot header past it -
inside the page-rounding slack, so nothing traps - finds a `sequence` that is not the index it claimed, and
`segment_write()` reports the ring full. `write_index` never advances past that slot, so **the ring is full for
ever**. `shm_full_dropped` is drop-on-full, so those 1,535 datagrams were discarded rather than rerouted, and the
publisher printed "sent 2,000 message(s)" and exited 0.

Four readings of this were wrong before the experiment: "a fault either way" (Plan), "silently the wrong slot"
(Dev), "the smaller direction works correctly" (Dev), and the sentence this paragraph replaces. What settled the
smaller direction was not in `tickle.c` at all - `tt_segment_attach()` in `hal_linux.c` already `fstat`s the file
and refuses a region shorter than the length asked for, with a comment giving exactly that reason. So the two
directions were never symmetric: one was guarded in the HAL and invisible to anyone reading the ring, and the other
was unguarded and silent.

`segment_header_check()` now compares both fields and returns `tt_SEGMENT_BAD_HEADER` on a mismatch. For the larger
direction that closes a hole; for the smaller it makes an existing refusal explicit rather than incidental.

**It also raises the stake on the two-step attach above.** A runtime slot size makes mismatched geometry ordinary
rather than exotic, and the failure it would meet is the wedged ring, not a crash. The header being the single
authority is what removes the class, and the geometry check is the guard until it is.

### What the API reports, and what it does not

A publisher whose type cannot fit a slot still works: its samples go over UDP and are counted in
`segment_oversized_to_udp`. So "cannot use the segment" is not a failure of `create_publisher`, and returning a new
non-zero code for it would break every existing caller - our own examples test `if (ret != 0)`
(`examples/linux/perf/perf_client.c:309`, `examples/linux/uint64/publisher.c:126`), and would abort on a working
publisher.

Nor does it get an output field. The rule is the division, not the mechanism:

| | goes in | because |
|---|---|---|
| the user set the slot size and a type does not fit | the **return value** of `tt_Context_create_publisher()` | they chose a value that does not match their data: the intended behaviour does not happen, so it is a **correctness** problem |
| the size is the default and a type does not fit | the existing `segment_oversized_to_udp` counter, plus one log line per topic | the data is delivered, over UDP: it is a **performance** fact, not a mistake |

Only a user who set the value is told through the return, and they are writing new code that handles it, so nothing
existing breaks. Everyone else sees no new API surface at all. **No new output field**: `segment_oversized_to_udp`
already reports the diagnostic half, and a field would duplicate it while adding a second place every caller has to
check. `pub->batch`, `pub->reliable_cache` and `pub->durable` are not precedent for this - they are inputs the user
writes, not outputs.

The check belongs at **publisher creation**, not at the first oversized sample. The type is known then, the
typesupport knows its maximum encoded size, it fires once per topic rather than per sample, and it fires even if a
sample that large never occurs in testing. A first-sample check reports hours into a run, on a target that may have
nobody reading its log.

**The guideline the user gets** (the deliverable section 4 of the conversation asked for):

    slot_bytes     >= the largest encoded message on any topic whose subscribers may be same-host
    slots           = segment_bytes / (sizeof(struct tt_SegmentSlot) + slot_bytes)
    segment_bytes  >= (sizeof(struct tt_SegmentSlot) + slot_bytes) x burst depth

At today's defaults that is 512 KiB / (16 + 1472) = 352 slots. A slot sized for p4 gives 185, and holding 352 at that
slot size needs about 1 MiB - still nothing beside 216 MB.

### The two halves are separable, and the larger gain is in the smaller one (2026-10-02, implementing)

Writing this section I treated "encode into the slot" and "do not fragment" as one change. They are not, and the
distinction decides what to build first:

| | what it buys | what it needs |
|---|---|---|
| **(a) do not fragment an all-local sample** | **p4's 92.6%** | the limit the decision compares against |
| (b) the encoder writes into the slot | one copy per publish | parameterising ~71 `tx_buffer`/`tx_tail` references |

Fragmenting is a decision about how many datagrams a sample becomes, not about where it is written, so (a) needs
nothing from (b). The pieces it does need are already in: a runtime slot size that can hold a whole sample
(`e94f6240`), a cache that takes an explicit length rather than measuring to `tx_buffer`'s end (`28347057`,
`bc7ed108`), and a two-step attach so a peer with a different geometry still interoperates (`4f5b40da`).

**And (a) has a consequence this section did not state: an unfragmented record cannot fall back to UDP.** It is
larger than the MTU by construction, so if the segment is not there, there is nowhere else for it to go. That
makes the rule conservative rather than optimistic:

- fragment as the network requires **unless** every destination is same-host **and already attached** **and** the
  record fits that peer's `slot_bytes`;
- a destination not yet attached, or a broadcast with no known peer list, fragments as today.

The *full* ring is not the hazard it first looks like, and that is Dev's measurement rather than an argument: a
full ring must never reroute to UDP anyway, because a datagram sent that way overtakes the records already in the
ring and the reader discards everything older behind it - 97.6% of CI's same-host traffic when it was tried. So
for an attached same-host peer the fallback was never available; a full ring drops, and an unfragmented record
changes only the size of what is dropped. What (a) must not do is produce a record for a peer whose segment it
has not confirmed.

### How this will be read, written before it is run

The cell is S6's `reliable_throughput` p4, all-local, against the figures in COMPARISON 2.2c (TickLE **2,745**,
CycloneDDS 5,287, FastDDS 1,983).

Unfragmenting halves the datagram count for a 2800 B sample, so the naive prediction is about **5,400**. Two known
costs pull against it and the arms below separate them:

- **a shallower ring**: a slot that holds p4 gives 128 slots where 1472 B gave 256. **Measured 2026-10-02,
  before the encoder work, because designing on an unknown cost is choosing blind**
  (`experiments/slot_depth_cost.sh`, 3 arms x 3 reps, `results/slot_depth_cost_e94f6240.txt`):

  | arm | slot / slots | samples, median | range |
  |---|---|---:|---|
  | small | 1472 / 256 | 752,687 | 747,987..848,547 |
  | big, p4 slot | 2816 / 128 | 734,713 | 693,199..817,109 |
  | deep, p4 slot, 1 MiB segment | 2816 / 256 | 700,092 | 633,475..836,465 |

  Taken at the 512 KiB default, so "small" is the 256-slot ring that was current when it ran; `5ee4b7e6` moved the
  default to 768 KiB and 512 slots hours later. The arms are named by their geometry rather than by "today" for
  that reason - what this measured is 256 against 128 slots, and it stays true whatever the default becomes.

  **All three ranges overlap, so depth is not separable here** and 6e's p4 prediction stands on batching
  alone. Stated as not-separable rather than free: this harness's own spread is 13-32% across repetitions
  of one arm, so a real cost below about 20% would not show. If the encoder work's measured gain falls
  short of its prediction, ring depth is still a candidate and wants a quieter instrument before it is
  ruled out.

  Two errors in the first version of that run are worth carrying. It judged on `tx_shm`, which counts
  **datagrams** - an event - where the question is about **samples**, the item; on the event counter the
  arms read "within 10%" while samples had fallen 17%. And it was n=1, against a quantity whose run-to-run
  spread is as large as the effect, so neither the 17% nor its absence meant anything.
- **lost batching**: `tx_buffer` coalesces several samples per flush and one sample per slot cannot. Visible as
  `segment_doorbells_sent` per sample rising, and in CPU per Msample rather than in throughput alone.

| outcome | reading |
|---|---|
| p4 separably above 2,745, near 5,400 | the fragmentation bound was the whole cost; record it and the all-local caveat |
| p4 separably above 2,745, well short of 5,400 | the bound was real but ring depth or batching eats part of it; the control arm says which, and **that number is what we report**, not the naive prediction |
| p4 not separably above 2,745 | the change did not deliver. It is not kept on throughput grounds, whatever else it tidies |
| p1, p2 or p3 separably **below** their 2.2c figures | a regression from the shallower ring. Report first, before any gain |

Ranges, not medians: p2 and p3 against CycloneDDS are draws on overlapping ranges today and must not become "wins"
on a median that moved inside its own spread.

**Latency is not this change's metric and must not be read as one.** 2.2c measured our segment path at 0.058 ms
against our own kernel path at 0.050 - the segment costs 16% on a p2 round trip, pure arm against pure arm - and
nothing here touches the doorbell that is the candidate cause. A p4 throughput win beside an unchanged latency loss
is the expected result, not a mixed one.

### What is not established

- **Whether the ring can serve as the RELIABLE retention cache for an all-local publisher: no, and the reason is
  not the one written here (settled by reading, 2026-10-03).** This asked it as a sizing question - "the ring is
  sized for flow rather than retention depth" - and sizing is the secondary objection. The primary one is
  ownership, and it is structural rather than a matter of degree:

  1. **The memory belongs to the receiver.** `own_segment` is one ring per *context*, which that node READS
     (`segment_read(node->own_segment, ...)`); a publisher sends by writing into the *subscriber's* inbox, through
     `segment_peers[]`. The sender cannot pin, reserve or re-read anything in it. Retention has to be memory the
     sender controls, and this is not.
  2. **Delivery is what frees the slot.** The reader releases a slot by setting its sequence one lap ahead, after
     which any writer may take it - and `segment_write()`'s own comment says many peers write into one context's
     segment, so the next occupant may belong to a different *node* entirely. Retention must outlive delivery;
     the ring's contract is that delivery ends the record's life. The two requirements are opposed, not merely
     mismatched in size.
  3. **The samples that need retransmitting are almost never the ones the ring still holds.** `segment_write()`
     either lands the record or returns false because the ring is full, and in the second case nothing was
     written at all - so a full-ring drop, which is this path's ordinary loss, leaves nothing to retain. There IS
     one written-but-lost case, and it does not help: a record larger than the reader's `rx_buffer` is counted as
     read and discarded, because `segment_read()` releases the slot either way so that a record a mapping cannot
     hold can never wedge the ring. The slot is gone in exactly the case a retransmission would want it back.

  TRANSIENT_LOCAL makes the same point from the other end: a late joiner's backlog has to survive an arbitrary
  amount of unrelated traffic through a ring shared with every other sender on the host.

  **What this changes about decision 2.** The plan counted an all-local RELIABLE sample as four writes going to
  two. It is four going to three: encoding into the slot removes the `tx_buffer` copy, and the retention copy
  stays. For BEST_EFFORT it is three going to two, as written. So 6e(b) is worth one write per sample on both
  paths rather than one on BEST_EFFORT and two on RELIABLE - still the larger of the two halves, but the
  RELIABLE case is not the extra prize this plan expected.
- **The header-validation hole above**, which needs the two-build test before it is called a defect.

### 6e(a) was built, and it cannot deliver before 6e(b) (2026-10-03)

Decision 1 - do not fragment a sample bound only for shared memory - was implemented in `838d659c` as
`whole_record_limit_for()`, a predicate that raises the record limit to the destination's slot when every
destination is a same-host peer whose segment is attached. Its unit tests pass in both directions. Across four
rig campaigns it never changed a number, and this section records why, because the reason is not the one that
was being inferred.

**Two instruments were wrong before any of the measurements were.**

- `s6_witness_check.sh` ran the client as `... 2>&1 | grep '^RESULT'`. The client is the publisher, the only
  side that can refuse a whole-record send, so every diagnostic it printed went into the pipe. Searching the
  *server* log for a publisher-only message returned nothing, and nothing is what "it did not refuse" looks
  like. The client's output is kept in a file now.
- `p4_whole_record.sh` decided INERT from `sample_path`. That is `-DBENCH_SAMPLE_PATH`, a compile-time string
  `build.sh` chooses from `TICKLE_P4_PATH`: in a frag build it reads `frag` whatever any sample did. The test
  could not fail. It said INERT on four campaigns without once inspecting behaviour, and was right every time,
  carried by the datagrams-per-sample figure beside it. That figure decides it now.

`experiments/whole_record_refusal.sh` asks the publisher instead. Every condition in the predicate is
publisher-local state, so none of it needs the rig: one private namespace, both roles, and core names the cause
in its own words. Its positive control - the same pair across two namespaces, where a segment is impossible -
prints `a destination has no attached segment`, so silence from the same-host run is the publisher's and not
the harness's. The control arm reproduces the rig cell to within 1.2% (2713 Mbps here, 2745 on the rig), which
is what makes the rest of this measurable without rig time.

**What the publisher said: nothing. It grants.** And when it grants, the cell collapses.

| arm (same binary pair, only the slot differs) | `tx_dropped_oversize` | samples sent in 5 s | send_mbps |
|---|---:|---:|---:|
| default slot (6e(a) cannot raise the limit) | 0 | 605,696 | **2713.5** |
| slot 4096 (6e(a) grants) | 277,500 | 421 | **1.9** |

**The first cause was the bound disagreeing with itself.** 6e(a) raised the limit for the datagram count and for
the retention cache, so the cache stored the sample as one whole record - but `end_encode_sample()` still tested
the constant `FRAG_WHOLE_DATA_LIMIT` and put two fragments on the wire. The subscriber then NACKed, and
`send_cached_record()` handed the cached *whole* record to `end_encode()`, whose `submessage_fits_datagram()`
measured it against `tt_MAX_BUFFER_LENGTH` and refused it. Forever. That is the 277,500. The cache and the wire
disagreed about one number, which is precisely what hoisting the destination decision above the datagram count
was supposed to prevent; the comment claiming so sits on the code that did it.

`record_size_limit(node, floor, peers, peer_count)` is the repair: one bound, asked at each send site with
**that send's** destinations rather than computed once and threaded down. It has to be per-site - a publish may
go to two peers and the retransmission of the same sample to the one node that asked, so a single value is
right for one and wrong for the other. `seam_send_to()` no longer falls back to UDP for a record only a segment
can carry, since such a record is larger than a datagram by construction.

**The second cause is ring economics, and it is why 6e(a) is held.** With the bound repaired the retransmit
refusals fall from 277,500 to 141, and the cell is still at 0.66-0.82 Mbps: `shm_full_dropped` 934, RELIABLE
never recovering, the subscriber logging `Still waiting on reliable seq_no 52 ... after 176 retries`. Restoring
the slot *count* to the default's 512 by widening the segment changed nothing (934 against 936), which rules out
sizing. A 2800-byte sample held as one 4096-byte slot costs about 2.7x the ring bytes of the same sample as two
datagram-sized ones, and the acknowledgements travel through the same rings, so samples and acks starve each
other.

So the order in this plan is wrong and the dependency runs the other way: **6e(b) is the prerequisite for
6e(a)**, not an optimisation on top of it. Encoding into the slot is what stops a whole record costing a whole
slot; until it exists, carrying a record whole buys one fewer datagram and pays several times its own ring
footprint. `valid_slot_bytes()` therefore caps the runtime slot at one datagram, so an application cannot select
the collapsing configuration, and a build may still set `tt_SEGMENT_SLOT_BYTES` higher to measure the path.

**What is still not established.** The rig has never been observed granting: its cells show 2.01 datagrams per
sample, so something there refuses that does not refuse here, and the instrumented build (`a5e9c553`) has not yet
been run on it. That answer does not change the hold - the collapse is reproducible locally and the cause is
structural - but it is the one remaining question about the predicate itself. Nothing from any of this has been
written into COMPARISON: there is still no p4 figure to write.

### 6e(b): the seam is three fields, not a hundred and eighty (inventory + design, 2026-10-03)

`src/tickle.c` has ~138 references to `tx_buffer`/`tx_tail`, which made decision 2 look like a file-wide
refactor. Counted per function it is not. (Two automatic attributions were built and thrown away first - an
awk tracker that carried its function name across boundaries and reported 3 and 15 where the truth was 1 and
1, and a brace-matcher that ran past the end of a function and credited it with 42 in a "4,153-line" body.
The table below was verified by hand.)

| function | refs | what it is |
|---|---:|---|
| `publisher_publish_locked` | 19 | the publish path decision 2 changes |
| `client_call_locked` | 13 | the same decision for a service call |
| `flush_tx` | 9 | hands the buffer to the transport |
| `build_and_send_update` | 7 | discovery - not sample data, no slot to go to |
| `send_acknack_range` | 6 | a control submessage, same |
| `start_encode` / `encode` / `encode_string` / `rollback` | 3/2/2/2 | the encoder primitives |

**The risk this plan named is mostly not there.** 6e lists RELIABLE retention, DURABILITY and ACKNACK as the
three things that assume `tx_buffer`. Measured: `cache_reliable_sample` 0, `make_depth_room` 0,
`process_acknack` 0, `cache_sample_fragments` 1. Retention already works on the cache arena. The one coupled
consumer is `send_cached_record()` (3), which copies a cached record back INTO `tx_buffer` to retransmit it -
a consumer of the encoder, not of retention, and open question 4 above says why it cannot simply point at a
slot instead.

**The seam.** Every encoder primitive reaches the buffer through exactly three fields - `node->tx_buffer`,
`node->tx_tail`, `node->tx_size` - and so does every consumer that measures what was just encoded:

    sample_cdr_length        node->tx_buffer + node->tx_tail - submessage_header
    submessage_fits_datagram node->tx_buffer + node->tx_tail - submessage_header
    end_encode               node->tx_buffer + node->tx_tail - submessage_header
    send_tail_as_fragments   (uint8_t*)submessage_header - node->tx_buffer

None of them assumes the buffer is the node's OWN. They assume base-plus-tail is where the encode ended. So
**redirecting those three fields at a slot carries the encoder and its length consumers unchanged**, and
decision 2 becomes a change to where a publish points them rather than a rewrite of how anything encodes.

**What must not be redirected, and why the 6e(a) predicate is the guard.** Three consumers assume the bytes
are going out as a datagram and must not run while the target is a slot: `send_tail_as_fragments()` (a slot
has no MTU, so there is nothing to fragment), `end_encode()`'s deferral branches (they move a submessage to
the front of the NEXT buffer, which a slot does not have), and `flush_tx()` (it hands `tx_buffer` to the
transport). All three are already excluded by the conditions `unicast_destinations_for()` tests - an
immediate flush, a small known peer set, and `old_tx_tail == sizeof(struct tt_Header)`, which is to say an
empty buffer with nothing batched ahead. The predicate built for 6e(a) is therefore also the precondition
for redirecting, which is the one piece of 6e(a) that survives its own hold.

**Not started.** The inventory and the seam are written down; the change is not made. Sequencing it behind a
night that already put one memory-safety defect on main is deliberate - the first slice is a redirect that
cannot be partially applied, and it wants a rested reading of `end_encode()`'s branches rather than a fast one.

## 7. Open questions


1. **Notification. Measured 2026-10-02, and the answer is a FIFO.** A reader must learn a record arrived. Today
   that is a zero-length UDP datagram per reader advance (`segment_doorbells_sent`), which in a ping/pong is one
   per sample because the reader sleeps every time. COMPARISON 2.2c measured the consequence with no mixture
   either side: **pure kernel 0.050 ms against pure segment 0.058 at p2, ranges separable, 16%**, while both
   vendors signal inside their own segments and gain 29-41% from theirs.

   `experiments/wake_cost.{c,sh}` measures the mechanisms themselves, with no transport around them, both roles on
   one rig Pi, 20,000 round trips, 3 reps (`results/wake_cost_4arm.txt`):

   | mechanism | RTT p50 us | saves vs today | keeps the single `ppoll` wait point |
   |---|---:|---:|---|
   | zero-length UDP (today) | 15.72 | - | yes |
   | unix-domain datagram | 17.94 | **-2.22, it is slower** | yes |
   | **FIFO** | **10.11** | **5.61** | **yes** |
   | futex | 9.52 | 6.20 | no |

   The file was then restructured to pass the repository's own clang-tidy - 53 findings, including a `main` at
   cognitive complexity 81 - and re-run, because a measurement belongs to the binary that produced it and
   "the refactor was mechanical" is an argument rather than evidence. The four arms moved by at most 0.4 us
   (`results/wake_cost_4arm.txt` is the pre-restructure run, `wake_cost_relint.txt` the published one), which is
   the control for the refactor.

   **The unix-domain arm was the one this plan argued for and it is worse than what we have.** The reasoning was
   that skipping the IP stack must be cheaper; Linux's UDP loopback path is well optimised and AF_UNIX datagram has
   its own costs. Written down because the reasoning was the error, not the arithmetic: the candidates had been
   chosen by argument rather than by listing what is both nameable and pollable, which is also why the FIFO - the
   one that won - was missing from the first run.

   **A FIFO takes 90% of futex's benefit and changes nothing structural.** It is nameable like the segment file, so
   no fd needs passing; it is pollable, so it joins the existing `ppoll` set; and it never enters the socket layer.
   futex buys 0.6 us per round trip more and costs a rewritten wait loop that also interacts with the poll and block
   wait modes, both of which are scored (`project_rmw_poll_and_block_cases`). That is not a good trade.

   **What this does not establish.** 5.61 us against the 8 us 2.2c's gap needs is **70%**, so a third of it is
   elsewhere - the slot write, the drain loop, cache behaviour - and a FIFO doorbell will not close the cell. The
   two figures also come from different runs on different days, so combining them is an estimate. The measurement
   that settles it is the p2 latency cell re-run against a FIFO doorbell, not this benchmark.

   **The gap re-measured on current code, 2026-10-03 (`a55d9cf9`), before building anything against it.** The 8 us
   above was taken on `fcc4ddb4`, and `record_size_limit()` has since entered the publish path of every build. It
   was measured inert on p4 THROUGHPUT, which says nothing about p2 latency - a different scenario, a different
   payload, and a metric a few hundred nanoseconds can move. 3 reps, 1,977 round trips each, both arms pure
   (`tx_shm` share 0.993 and 0.000, so neither is the mixture 2.2c's own ON arm carried):

   | arm | this run | COMPARISON 2.2c | |
   |---|---:|---:|---|
   | OFF, kernel | **0.046** | **0.046** | the control, and it reproduces exactly |
   | ON, segment | **0.051** | 0.050 | 2.2c's ON was 13.5% kernel; this one is not |

   The control matching to three decimals is what makes the rest readable: the box, the harness and the metric
   agree with the run being compared against, so a difference in the other arm is the code and not the rig.

   **The gap stands and is separable** - ranges 0.051..0.052 against 0.045..0.046 - at **+10.9%, which is 5.0 us**,
   not 8. So the premise survives: there is a real, repeatable, separable cost to our own segment path at p2.

   **The sizing argument does not survive, and it fails in the direction that should worry us.** 5.61 us against a
   5.0 us gap is 112%. A saving that exceeds the whole gap is not a better doorbell; it is proof that a figure from
   `wake_cost`'s own round trip cannot be subtracted from this cell's gap at all. The honest form of open question 1
   is therefore: a FIFO doorbell is worth building and measuring IN THIS CELL, and no number should be predicted
   for it beforehand. The 8 us it was sized against describes a build that no longer exists.
2. **Whether a same-host pair keeps its UDP socket at all,** for discovery only, or whether discovery also moves into
   the segment. Keeping discovery on UDP is the smaller change and keeps one discovery path; moving it is what would
   let two processes talk with no network stack at all, which is a real claim for an embedded target.
3. **The FreeRTOS form.** Two tasks in separate protection domains are the same case, but the segment is HAL-provided
   rather than `shm_open`. Whether stage 1 carries that or defers it changes the seam's shape, so it is decided before
   stage 0 lands, not after.
4. **Interaction with TickLE Security.** Section 1's "the same datagram through the same acceptance path" is what makes
   this safe, and it has to stay true when the security module's per-datagram transform exists. The transform sits
   inside the acceptance path, not inside the transport - stated here so stage 0's seam is not shaped in a way that
   makes it impossible later.

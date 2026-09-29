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

**Still owed:** the p1-p4 numbers against WIRE_PLAN 10.4's floors, and items 4, 6, 7, 8, 9 and 11 of section 6a.

## 7. Open questions

1. **Notification.** A reader must learn a record arrived. A futex or an eventfd per reader costs a syscall and gives
   back the wakeup latency the poll loop currently pays; a pure spin costs a core. The choice interacts with the poll
   and block wait modes that are both scored (`project_rmw_poll_and_block_cases`), so it is measured, not argued.
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

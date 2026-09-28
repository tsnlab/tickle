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

The transport choice belongs to core because core already knows what "same host" means: g8's host registry in /dev/shm
gives each context on a host an id of its own, and a peer discovered with the same host identity is a candidate for
this transport. **No new discovery mechanism.** If the module is not built, or the peer is not on this host, or attach
fails, the peer is reached over UDP exactly as today - that fallback path is the module's own failure mode, and it is
silent by design in the sense that correctness does not depend on it, but it is counted so a user can see it happened.

## 3. The segment

Decisions to make explicitly, because each one has a failure mode that only shows under load or after a crash:

- **One segment per writing context, not per topic or per pair.** A reader attaches to a writer's segment read-only.
  Per-pair segments multiply by peers; per-topic segments multiply by topics and make a late-created topic need a new
  segment mid-run. Per-writer is the smallest number that still lets a reader map only what it needs.
- **Fixed capacity, chosen at context creation and stated,** like every other core structure. A ring of records sized
  from the same budget idiom the KEEP_ALL publisher cache uses (`RMW_TICKLE_KEEP_ALL_BYTES`' sibling). Exhaustion is
  counted and warned about once - never silent, never unbounded.
- **Single writer, many readers.** The only writer is the owning context, which removes the general multi-writer
  concurrency problem. Readers coordinate with the writer through a per-record sequence and the release accounting
  below.
- **Naming and permissions.** A name derived from the context id (so a stale segment is identifiable), created with
  the owner's own user and no wider access. A segment a process cannot open is a fallback to UDP, not an error.
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
refused identically; and it beats loopback UDP on latency and CPU per sample at p1-p4 by more than 2xSE with nothing
worse by the same rule.

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

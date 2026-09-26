# DATA_FRAG in TickLE core: design, acceptance bar, and the two constraints that bind it

2026-09-26. The user's instruction: implement DATA_FRAG in TickLE core so that TickLE beats both
FastDDS and CycloneDDS at **all four payload shapes p1, p2, p3 and p4**, then look for further
optimisation, and only then move to `rmw_tickle`. This supersedes the 2026-09-25 decision to leave
OS-level IP fragmentation alone (COMPARISON.MD to-do 15).

Core is Dev's. This is the design, the measurement bar and the two numeric constraints Plan found
before any code exists, because both of them would be expensive to discover afterwards.

## 1. Where p1-p4 actually stand, including the gap in the evidence

| shape | size | who wins today | note |
|---|---|---|---|
| p1 | 76 B | **TickLE, every metric** | release-measured 2026-09-26 |
| p2 | 1292 B | **TickLE, every metric** | release-measured 2026-09-26 |
| p3 | 1424 B | TickLE, every metric | campaign only, `-O0` core - needs a release re-measure |
| p4 unshaped | 2800 B | TickLE on CPU, bandwidth, throughput; **CycloneDDS on memory** | campaign only, `-O0` |
| p4 under loss | 2800 B | **unknown - no valid measurement exists** | campaign cell c6 is VOID |

**The most important line is the last one.** c6 was voided for incomplete delivery, so this project
currently cannot say whether TickLE wins or loses p4 under loss. That is the condition DATA_FRAG
exists for, and there is no baseline to improve on. **Re-establishing c6 is the first task, before
any DATA_FRAG code**, or the change will be evaluated against nothing.

What is known about that condition is a mechanism, not a comparison: at p4 under 5% injected loss,
**97.4% of kernel IP-reassembly attempts fail** and roughly 1 datagram in 47 survives. A 2800 B
sample becomes two IP fragments, either loss destroys the sample, and TickLE then retransmits the
whole sample - which fragments again. Both DDS vendors avoid the path entirely by fragmenting at
the middleware layer, which is what DATA_FRAG is.

## 2. Constraint A: the per-fragment header must be <= 4 bytes

This is arithmetic on the measured framing model (COMPARISON.MD 2.5: base 70.6 B per datagram
including the 42 B of Ethernet+IP+UDP, plus ~42 B per extra IP fragment; the model predicted all
twelve observed packet counts with no mismatches).

TickLE today at p4 sends **one** datagram and lets the OS split it, so it pays the base once plus
a 42 B IP-fragment surcharge:

    2800 + 70.6 + 42.0 = 2912.6   (measured 2,914)

With DATA_FRAG each fragment is a **full datagram**, so that 42 B surcharge is replaced by a whole
second base header, and the fragment header is paid twice:

    2800 + 2*70.6 + 2*F = 2941.2 + 2F

CycloneDDS measures 2,950 B/sample at p4. So:

| per-fragment header | B/sample | vs CycloneDDS |
|---:|---:|---|
| 0 B | 2941.2 | win |
| **4 B** | **2949.2** | **win, by 0.8 B** |
| 8 B | 2957.2 | **lose** |
| 16 B | 2973.2 | **lose** |

**A DATA_FRAG header of 8 bytes or more converts a bandwidth row TickLE wins today into one it
loses.** The margin at 4 B is 0.8 B/sample, which is inside the noise of the model, so 4 B should
be read as "the ceiling, and even then the row is a coin toss" rather than as a safe target.

A 2-byte header is achievable and is what this recommends: `frag_index` (uint8) and `frag_count`
(uint8), padded to the submessage alignment. **The total sample size does not need a field** - it
is `(frag_count - 1) * frag_size + last_fragment_length`, and the last fragment's length is already
implied by `tt_SubmessageHeader.length`. `seq_no`, `endpoint_id`, `entity_id` and `timestamp` are
already in `tt_DataHeader` and are what tie the fragments together; they do not need repeating
beyond the per-datagram base already counted above.

The honest caveat: at p3 and below nothing fragments, so this constraint costs nothing there. It
binds only at p4, and only against CycloneDDS, and it binds hard.

## 3. Constraint B: DATA_FRAG alone cannot win p4, because the p4 loss is memory

The only metric TickLE loses at p4 unshaped is **peak RSS: 7,384 KB against CycloneDDS's 5,608**.
DATA_FRAG does not improve that and its reassembly buffers make it slightly worse. So "win p4"
requires a second, independent change, and it is already diagnosed (COMPARISON.MD 2.4):

TickLE's RSS is `C + reliable_depth * record_bytes` with C measured at 1,720-1,733 KB across all
four shapes - a 13 KB spread against a 5,464 KB range, so the retention window is the entire story.
The default `reliable_depth` is **2048 samples regardless of sample size**, so the arena grows
linearly with the payload: at p4 the touched arena is 5,651 KB of the 7,384 KB peak.

**Deriving depth from a byte budget instead of a sample count fixes it.** At a 512 KiB budget the
arena falls from ~5.7 MB to 512 KB and p4 RSS lands near 2.2 MB, comfortably under CycloneDDS's
5,608 - and p1/p2 are untouched, because at 76 B and 1292 B a 512 KiB budget still buys more depth
than 2048 samples does. `rmw_tickle` already has exactly this constant and this reasoning
(`RMW_TICKLE_KEEP_ALL_BYTES_DEFAULT`, 512 KiB, with its link-rate derivation written out); core
does not.

Reassembly storage, by contrast, is small: N concurrent reassemblies times the maximum sample size.
At p4 with N=8 that is ~22 KB, against the ~5 MB the depth change saves. It does not threaten the
memory row - provided N is bounded and static, per the no-malloc invariant.

## 4. Design, following the precedent already in the protocol

`tt_SUBMESSAGE_TYPE_UPDATE_PART` (type 7) already solves "one logical message across several
datagrams" for discovery announces, and its design notes record the user's own choice of **a new
submessage type over a protocol version bump**, so a node built before it skips the type as unknown
rather than misreading a part as a whole. DATA_FRAG should take type 8 and the same shape.

Where DATA_FRAG must differ from UPDATE_PART, and this is the substance of the work:

- **UPDATE_PART is idempotent; DATA is sequenced and reliable.** A lost announce part is recovered
  by the next periodic announce under the same `last_modified`. A lost DATA fragment has no such
  free retry. It needs either fragment-granular NACK, or sample-granular NACK that retransmits only
  the missing fragments.
- **The recommendation is sample-granular ACKNACK, fragment-granular retransmission.** The existing
  256-bit `tt_AckNackHeader.bitmap` stays exactly as it is - no wire change, no version bump on the
  ACKNACK path - and the Publisher, on being asked for sequence `n`, resends only the fragments of
  `n` the Subscriber has not acknowledged. That requires the Subscriber to report which, which is
  the one place a fragment-level field is genuinely needed. An alternative that avoids even that:
  resend **all** fragments of `n`. Simpler, and at p4 (2 fragments) it wastes at most one
  datagram per recovery - which is still 47x better than today's whole-sample-through-IP-reassembly
  path. **Start there.** The fragment-granular NACK is an optimisation to measure afterwards, not a
  precondition, and at 2 fragments it may never pay for itself.
- **Reassembly state is per `tt_WriterProxy`, bounded and static.** A fixed set of N in-flight
  reassembly slots, each with a fragment bitmap and a buffer of `tt_MAX_SAMPLE_LENGTH`. When all N
  are busy and a fragment for an N+1-th sample arrives, the oldest incomplete reassembly is
  abandoned and counted - the same shape as `gap_abandoned`, and it must be counted, because a
  silent drop here is indistinguishable from loss.
- **`tt_MAX_BUFFER_LENGTH` stays at 1472 and keeps meaning "one datagram".** A new, separate
  `tt_MAX_SAMPLE_LENGTH` bounds a reassembled sample. Conflating the two is what makes the current
  code refuse large samples, and it is also what `rmw_tickle` currently works around by raising the
  datagram limit and handing the problem to the OS - the workaround DATA_FRAG exists to retire.

## 5. Acceptance bar, and how each outcome reads

Written before implementation so it cannot be chosen afterwards. All four shapes, all three
frameworks, release builds, one session, interleaved by framework, identity fields asserted.

1. **c6 re-established first**, with no DATA_FRAG, so there is a baseline. Pre-registered
   expectation: TickLE loses or barely survives p4 under 5% loss, given the 97.4% reassembly
   failure. **If TickLE already wins it, the motivation for DATA_FRAG is weaker than stated and
   that must be said rather than absorbed.**
2. **p4 under loss, with DATA_FRAG**: the target is throughput retention comparable to p1's 90.6%,
   against today's 6.6%. This is the row the whole change is for.
3. **p4 unshaped bandwidth**: must stay under CycloneDDS's 2,950 B/sample. Constraint A says this
   is decided by the header width alone and the margin is under 1 B - so this row should be
   measured early, on a throwaway build if necessary, rather than at the end.
4. **p4 memory**: must fall under CycloneDDS's 5,608 KB, which needs the byte-budget change of
   section 3 and not DATA_FRAG.
5. **p1, p2, p3 must not regress.** Nothing fragments at those sizes, so the prediction is no
   change at all; any movement there means the fragmentation path is being entered when it should
   not be, and is a defect rather than a trade.

## 6. Revision (2026-09-26): unify UPDATE with DATA, and UPDATE_PART with DATA_FRAG

The user redirected the design: *"Unify UPDATE with DATA and UPDATE_PART with DATA_FRAG, and design
an effective DATA_FRAG in that direction. We have to win p4."* (translated). Sections 1-5 stand; this
section replaces section 4's wire shape.

### 6.1 UPDATE becomes DATA on a built-in discovery endpoint

This is the RTPS arrangement - discovery travels as DATA on built-in writers - and TickLE's fields map
onto it with nothing left over:

| UPDATE today | as DATA |
|---|---|
| `last_modified` (uint64 ns) | `timestamp` (uint64 ns) - one-for-one |
| (implicit: the node) | `endpoint_id` = reserved built-in id, `entity_id` = reserved per-node id |
| `entity_count` + entities | payload |
| - | `seq_no` = announce generation (see 6.4) |

Cost: `tt_UpdateHeader` (9 B) becomes `tt_DataHeader` (20 B) + `entity_count` (1 B), **+12 B per
announce**, at roughly one announce per second per node. Negligible.

### 6.2 UPDATE_PART and DATA_FRAG become one fragmentation mechanism

Once discovery is DATA, one reassembly pool serves both. `tt_SUBMESSAGE_TYPE_UPDATE_PART`, its
per-endpoint bookkeeping in `struct tt_Node` (`update_part_last_modified/received/count`, about
3.3 KB) and its separate completion rule are removed. Stated plainly so it is not over-read: **that
is a small memory saving and does not decide the p4 memory row** - section 3's byte budget does.

### 6.3 Why the unification is what wins p4: a short continuation header

`entity_id` is unique within a node (`entity_id_base + next_entity_id++`, `src/tickle.c:1042`) and
the source node is already in `tt_Header`, so **(source, `entity_id`, `seq_no`) identifies a
fragment's sample by itself**. `endpoint_id` and `timestamp` need to travel once per sample, not
once per fragment:

- `FRAG_FIRST`: `tt_DataHeader` (20 B) + `frag_count` (1 B); the index is implicitly 0
- `FRAG_CONT`: `entity_id` (4) + `seq_no` (4) + `frag_index` (1) + `frag_count` (1) = 10 B

Both are sent unpadded, since a fragment is always alone in its datagram (Dev's observation; the
receiver already accepts an exact length). From compiled struct sizes (`tt_Header` 4,
`tt_SubmessageHeader` 4, `tt_DataHeader` 20) plus 42 B of Ethernet/IPv4/UDP, at p4:

| p4 = 2800 B | B/sample | margin vs CycloneDDS (2,950) |
|---|---:|---:|
| today, OS IP fragmentation | 2912 (measured 2,914) | +38 |
| DATA_FRAG with the full header in every fragment | 2944 | **+6** |
| **unified, short continuation header** | **2931** | **+19** |

+6 B is inside the framing model's own noise; +19 B is not. This replaces section 2's "at most 4
bytes" target, which assumed a full `tt_DataHeader` in every fragment. `frag_count` stays in
`FRAG_CONT` so a continuation arriving before `FRAG_FIRST` can still be placed; the sample completes
only once `FRAG_FIRST` has supplied `endpoint_id` and `timestamp`.

### 6.4 The correctness risk: discovery liveliness versus DATA deduplication

Recommended: `seq_no` = announce **generation** - unchanged content keeps its seq. That preserves
today's UPDATE_PART behaviour, where a later periodic resend can complete a half-received large
announce, because its fragments share the slot key. The consequence is that every periodic resend
carries an already-seen seq, which an ordinary DATA path deduplicates. **The built-in endpoint must
refresh liveliness before deduplication** and re-apply the entity list only when seq changes. A test
must pin both halves, with a control in which liveliness is refreshed after deduplication and the
peer is wrongly declared dead.

### 6.5 Consequences to know

- **Wire version 6 -> 7.** UPDATE's type-1 encoding goes away. The 2026-09-24 choice of a new type
  over a version bump concerned UPDATE_PART alone; unifying UPDATE into DATA cannot be done without
  changing UPDATE itself.
- **p4 CPU is at risk, and is pre-registered here.** Today a p4 sample is one `sendto` that the kernel
  splits; under FRAG it is two `sendto` and, on the receiver, two receives. The campaign's p4 client
  CPU margin was 12.5 against CycloneDDS's 14.2 (at `-O0`). `sendmmsg` (section: CORE_HEADROOM item
  2) returns the sender to one syscall per sample and should land with FRAG or immediately after.
  p4 CPU is to be measured on the FRAG build both without and with `sendmmsg`, so each piece's cost
  is visible rather than bundled.

### 6.6 Amendment (Dev, 2026-09-26): one wire mechanism, two receive strategies

Section 6.2 as first written would have regressed something that works today. UPDATE_PART's parts are
each independently decodable - whole entities per part, processed on arrival - which is how a node
on core defaults, with no reassembly memory and a 1472 B datagram, discovers an rmw node whose
announce spans several datagrams. Byte-splitting discovery and reassembling it in the pool would
require every node to hold a pool as large as the largest possible announce
(`tt_MAX_ENDPOINT_COUNT` entities with names), which is far more than the 3.3 KB removed, and it
would fall on exactly the nodes that cannot afford it.

So the wire is one mechanism (`FRAG_FIRST`/`FRAG_CONT`) and the receive side has two strategies,
chosen by endpoint:

- **User data** is byte-split and reassembled in the node pool. The pool exists only when
  `tt_MAX_SAMPLE_LENGTH > tt_MAX_BUFFER_LENGTH`, so it costs nothing at the MCU default.
- **The built-in discovery endpoint** is split by the sender at entity boundaries, so each fragment
  carries whole entities, and the discovery handler processes each on arrival with no pool - today's
  UPDATE_PART semantics on the FRAG wire. A continuation is recognised as discovery by its reserved
  `entity_id`, so no extra field is needed.

This also settles 6.4: discovery does not pass through the subscriber delivery path, so there is no
DATA deduplication in front of it. Any announce or fragment refreshes liveliness first, and the entity
list is re-applied only when `timestamp` (`last_modified`) changes. The test for it is kept anyway,
with its control.

Order, each piece verified alone and the wire version bumped once: (1) FRAG for user data plus the
pool and one send-any-record routine for publish, retransmit and backlog - new types only, no bump;
(2) discovery onto DATA/FRAG, UPDATE/UPDATE_PART removed, `tt_VERSION` 7; (3) `sendmmsg`;
(4) the byte-budgeted depth in the harness. Step 1 is pushed alone so the p4 bandwidth row can be
measured on a real build early.

## 7. The c6 baseline: what the void cell already says, and the corrected pre-registration

Written before the release re-run of c6 is launched. Section 5 item 1's expectation - "TickLE loses
or barely survives" - was written without opening the void cell's own data, and that data says
something sharper. Campaign 2026-09-25, `-O0` TickLE core, p4 under 5% loss, client, median of 3:

| c6 (p4 + 5% loss) | TickLE | FastDDS | CycloneDDS |
|---|---:|---:|---:|
| `send_mbps` (5 s send window) | **61.9** | 13.8 | 12.2 |
| `wire_bytes_per_sample` | **153,732** | 3,136 | 3,275 |
| `wire_packets_per_sample` | **106.9** | 2.3 | 3.3 |
| `cpu_s_per_Msample` | **594.9** | 76.6 | 42.8 |
| `peak_rss_kb` | 7,376 | 28,636 | **4,940** |

**TickLE did not win c6. It won one metric and lost three by one to two orders of magnitude**: 47x
the bandwidth per sample, 14x CycloneDDS's CPU, and more memory. The throughput "win" is also not
what it looks like. `send_mbps` counts samples *accepted* during the 5 s window; TickLE's client
then used 8.1 s of CPU (utime 0.611 + stime 7.499) retransmitting them afterwards. At the time
TickLE's KEEP_ALL buffer was 2048 samples, ~5.7 MB at p4, against CycloneDDS's ~500 kB write-history
bound, so TickLE could accept roughly ten times as much before blocking. **That was a history-depth
mismatch - a QoS difference - measured as a throughput difference**, which is the thing the user's
standing rule (identical QoS for every framework) forbids.

**Correction to section 3 and to the implementation order in 6.6: the byte budget already exists.**
`9a230a1b` (2026-09-25 18:16) gave the TickLE harness a 512 KiB VOLATILE KEEP_ALL byte budget,
reached by blocking, mirroring `RMW_TICKLE_KEEP_ALL_BYTES_DEFAULT`. The campaign ran at 14:28 the same
day, before it. So step (4) of 6.6 is not work to do; it is a measurement not yet taken. It also
repairs the depth mismatch above, since 512 KiB is the same order as CycloneDDS's bound.

**Pre-registration for the release baseline (no DATA_FRAG, 512 KiB budget in force):**

- **Memory, p4: TickLE now wins.** Predicted RSS near 2.2 MB against CycloneDDS's ~5 MB. If it does
  not fall, the budget is not reaching the arena and that is a harness defect to find first.
- **Throughput, c6: TickLE's advantage shrinks sharply and probably reverses.** With matched buffers
  TickLE blocks after ~180 unacknowledged samples, and each of them still needs dozens of
  transmissions to survive kernel reassembly.
- **Bandwidth and CPU, c6: still lost by more than 10x.** The byte budget does not touch the
  mechanism, which is one lost IP fragment destroying the whole sample. **These two rows are what
  DATA_FRAG is for**: independently delivered fragments should take packets per sample from ~107
  to about 2.2-2.5, near CycloneDDS's 3.3.
- **If TickLE instead wins c6 throughput, bandwidth and CPU on this baseline**, the reassembly
  collapse was an `-O0` or buffer-depth artefact, DATA_FRAG's p4 case rests on wire efficiency
  alone, and that is reported as such.

Cells run: c1 and c5 (p1 unshaped and under loss, the 90.6% retention reference), c3 (p3, not yet
measured at release), c4 and c6 (p4 unshaped and under loss). `core_build=release` asserted on every
TickLE row, which the campaign script did not do until today.

## 8. The release baseline, read against section 7's pre-registration (2026-09-26)

`results/c6_baseline_2026-09-26.txt` and `_verdicts.txt`: cells 1, 5, 3, 4 and 6, all three
frameworks, 3 repetitions, release builds, `core_build=release` asserted on every TickLE row, verdicts
by `campaign_summary.py`. **Totals: WIN 36, DRAW/TIE 4, LOSE 0, VOID 10** - the ties are `loss_pct`
reading 0 for everyone, and all ten VOIDs are c6.

**p1, p3 and p4 unshaped are clean sweeps, p4 memory included.** At c4, TickLE's client RSS is
2,219 KB against CycloneDDS's 5,584 and FastDDS's 28,181. The pre-registration said about 2.2 MB,
and the byte budget accounts for the whole of it. The one p4 row TickLE lost in the campaign is now
a win, with no DATA_FRAG involved. The p4 unshaped bandwidth row holds at 2,914 against 2,950, the
same row DATA_FRAG puts at risk (section 6.3).

**c6 is VOID again, and this time the cause is visible in the data.**

| c6 (p4 + 5% loss) | sent | delivered | client `drained=` |
|---|---:|---:|---|
| TickLE | 15,363-15,911 | 98.8% | `timeout`, all 3 reps |
| FastDDS | 4,380-6,828 | 28.8% | `timeout`, all 3 reps, 45-47 `write_fail` |
| CycloneDDS | 386-1,889 | **100%** | `acked`, all 3 reps |

This is not the harness truncating the server, which is what voided the original c6. TickLE's client
reports `drained=timeout`: its reliable recovery at p4 could not finish within the harness's 3 s
drain cap (`default_drain_s`, `tickle/reliable_throughput/client.c:56`), and about 185 samples were
still unacknowledged when it gave up. FastDDS appears to stop delivering on its own: its server's
active span is ~3 s, and its writer records 45 refused writes. CycloneDDS delivered everything, but
only 386-1,889 samples, against TickLE's ~15,700.

Section 7's pre-registration, item by item:
- **p4 memory becomes a TickLE win**: confirmed, 2,219 against 5,584.
- **c6 throughput lead shrinks sharply and probably reverses**: **wrong**. TickLE accepted 70 Mbps
  against CycloneDDS's 3.9 and FastDDS's 16.4. The mechanism I predicted - matched buffers make
  TickLE block early - did not dominate. Recorded as a failed prediction, not reinterpreted.
- **c6 bandwidth and CPU still lost by more than 10x**: directionally right, but these are VOID
  figures and not verdicts. TickLE's client wire bytes per sample range 27,667-128,408 across three
  reps, against CycloneDDS's 3,372-6,584 and FastDDS's 3,161-3,245. A 5x spread within one
  framework's own repetitions is itself the finding: TickLE's p4 recovery under loss is unstable as
  well as expensive.

**What this adds to DATA_FRAG's acceptance bar for c6.** The metric the void hides is the one that
decides the cell: whether the reliable writer finishes. CycloneDDS does, and TickLE does not. So,
before any throughput, bandwidth or CPU comparison at c6:

1. TickLE must report `drained=acked` in every repetition, i.e. deliver 100% within the same drain
   cap. That is what makes c6 a comparable cell at all.
2. Then wire packets per sample should fall from today's unstable 20-90 into the 2.2-2.5 range, and
   stay there across repetitions. The spread matters as much as the median.

FastDDS's shortfall is FastDDS's. Its server active span and its refused writes look like its own
behaviour rather than the harness, but that is inferred from two fields and not verified. It
matters, because under the current rule any framework's incompleteness voids the cell for all
three. If it holds, c6 will stay VOID after DATA_FRAG even with TickLE at 100%. The rule will need
an explicit, pre-registered decision about whether one vendor's failure to deliver makes a cell
incomparable or makes it a loss for that vendor. That decision is not made here.

## 9. Step 1 on the rig: DATA_FRAG for user data, before sendmmsg (pre-registered 2026-09-26)

Build `e9be3434`: `FRAG_FIRST` (21 B) and `FRAG_CONT` (10 B), unpadded, an 8-slot pool, and one send
path for publish, retransmit and backlog. Discovery is unchanged and the wire version is still 6.
Off-rig, on a private netns with veth, Dev reproduced the c6 failure on the ipfrag build
(drained=timeout, 94.7 packets per sample). The frag build delivered everything under the same 5%
loss at 2.10 packets per sample, and measured 2931.5 B/sample at 0% loss, which matches section
6.3's prediction to the byte. Dev also measured the cost: the send rate at 0% loss fell 18%, because
there is one syscall per datagram until step 3 (`sendmmsg`).

**Order decided (Plan): `sendmmsg` before step 2.** Discovery traffic is about one announce per
second and cannot move a p4 number, whereas a p4 CPU row read without `sendmmsg` would have to be
read again. This run is the "FRAG without `sendmmsg`" arm that section 6.5 said to measure
separately. It is not the build p4 will be tabled on.

Cells 1, 2, 3, 4 and 6, all three frameworks, 3 repetitions, release. Each TickLE row is asserted
`core_build=release`, and `sample_path=datagram` at p1-p3 and `sample_path=frag` at p4.

Written before launch:

- **c1, c2, c3: no change** from the baseline in section 8, within each metric's spread across
  repetitions. Every row must report `sample_path=datagram`. Any other value makes the row VOID,
  and that would be section 5 item 5's defect: fragmentation entered where nothing needs it.
- **c4 bandwidth: about 2931 B/sample, under CycloneDDS's 2,950.** This row is decided by bytes on
  the wire and syscall batching cannot move it, so it is final on this build.
- **c4 CPU: expected to move against TickLE.** The baseline was 11.5 against CycloneDDS's 14.2, and
  one syscall per datagram costs roughly the 18% seen on veth, possibly enough to lose the client
  CPU row. **If it loses here, that is the known cost `sendmmsg` exists to remove, not a verdict on
  FRAG.** If it does not lose, `sendmmsg` is still worth doing but p4 no longer depends on it.
- **c6, criterion 1: TickLE `drained=acked` in all three repetitions, 100% delivered.** This is
  the criterion the baseline failed, and it is the one this build exists to meet.
- **c6, criterion 2: packets per sample in 2.1-2.5, with a narrow spread across repetitions**,
  against the baseline's unstable 20-90.
- **c6 may still be VOID** if FastDDS again delivers only part of its samples. That is the open
  rule question in section 8. TickLE's criteria 1 and 2 are read from TickLE's own rows either way.

## 10. Step 1 results, read against section 9

`results/frag_step1_2026-09-26.txt` and `_verdicts.txt`. The sweep reported no VOID of its own:
every TickLE row asserted `core_build=release`, and `sample_path=datagram` at p1-p3 and `frag` at
p4. Summary: **WIN 36, DRAW/TIE 4, LOSE 0, VOID 10.** The ties are `loss_pct` reading 0 for every
framework. All ten VOIDs are c6, and **the only incomplete framework is now FastDDS** (1,457 of
5,168 delivered).

| prediction (section 9) | result |
|---|---|
| c1-c3 unchanged, `sample_path=datagram` | **held.** c1 client CPU 5.28 / 115 Mbps, c3 6.62 / 938 Mbps, as at baseline |
| c4 bandwidth about 2931 < 2950 | **held: 2933 against 2,950** |
| c4 client CPU may lose until `sendmmsg` | **did not lose: 12.6 against CycloneDDS's 14.1.** Up from 11.5 (+9.6%), the syscall cost as foreseen, but it stays a win |
| c6 criterion 1: `drained=acked` in all 3 reps | **held**, 100% delivered in each (104,457-139,425 samples) |
| c6 criterion 2: 2.1-2.5 packets per sample, narrow | **held: 2.28-2.31** (baseline 20-90) |

c4 is a clean sweep on this build: every client and server metric is a WIN.

On TickLE's own c6 rows, which carry the label VOID only because FastDDS is incomplete, TickLE
leads on throughput (558 Mbps against 12.8 and 14.5), client and server CPU, client memory, and
wire bytes against CycloneDDS (3,184 against 3,227). Throughput retention under loss is now 59%
(558 of 940), up from 6.6% in the campaign. Section 5 item 2's target was "comparable to p1's
90.6%", so this **improved roughly ninefold and still falls short of that target**. That is stated
as not met rather than rounded up.

**One c6 row is still lost: server memory, 13,061 KB against CycloneDDS's 5,841.** It is unchanged
from the baseline (13,020), so DATA_FRAG did not cause it. The mechanism, checked in the code:
- The benchmark server declares `BENCH_REORDER_SLOTS = tt_RELIABLE_BITMAP_MAX_BITS` = 4,096 reorder
  slots of about 2.8 KB at p4, which is about 11.5 MB, statically.
- Its tracking window is 256 samples (`window_samples=256`).
- Core chooses a slot as `seq % reorder_slots` (`src/tickle.c:5858`). Unshaped, samples arrive in
  order and no slot is touched, which gives c4's 1,731 KB. Under loss, out-of-order samples land
  all around the ring as seq climbs into the hundreds of thousands, until every page is resident.
- 13,061 - 1,731 = 11,330 KB, which is the declared ring.

`rmw_tickle` does not do this: it sizes reorder slots to the tracking window
(`RMW_TICKLE_REORDER_SLOTS`, `rmw_subscription.c:276`, "defaulting to the window bound"). **The
benchmark server holds 16 times the slots its own window can use**, and rmw's rule is the product
behaviour. Mirroring it is the same correction as the KEEP_ALL byte budget: measure what a user of
the product gets. Dev's reorder-storm finding sets the floor. Fewer slots than the window storms,
while exactly the window measured 2.94 packets per sample. **Pre-registered: c6 server RSS about
1,731 + 256 x 2.8 KB = about 2.45 MB, under CycloneDDS's 5,841, with packets per sample unchanged
at 2.28-2.31.** If packets per sample rises, the slots are binding and the change is wrong.

**c6 still cannot be scored under the current rule while FastDDS is incomplete.** Whether it ever
can is the user's decision (section 8), and it has been put to them.

### 10.1 The c6 scoring rule, decided (2026-09-26)

The user chose **"compare the complete vendors with each other"** (translated). It was committed to
`campaign_summary.py` before the reorder-slot re-run's results were read. An incomplete vendor is
excluded from that cell's verdicts and shown on a DELIVERY FAILED line; an incomplete TickLE loses
every metric of the cell. Re-scored under it, step 1's c6 is TickLE against CycloneDDS: every metric
WIN or TIE except **server memory (LOSE)** and server wire bytes (DRAW, overlapping ranges). The
baseline's c6, where TickLE delivered 98.8%, becomes all LOSE, which is the control showing that the
rule bites TickLE too.

## 11. p4 is won on every scored metric (reorder slots = window, 2026-09-26)

`results/reorder_fix_2026-09-26.txt` and `_verdicts.txt`, build `e8be9fb3`, which is DATA_FRAG step 1
without `sendmmsg`. Cells 4 and 6, 3 repetitions, scored under the section 10.1 rule. Every TickLE
server row reported `window_samples=256 reorder_slots=256`, so the change was in effect. The sweep
reported no VOID.

**Totals: WIN 17, DRAW/TIE 3, LOSE 0.** FastDDS delivered 2,212 of 6,212 at c6 (64.4% missing), so it
is excluded from c6 and listed as a DELIVERY FAILED.

| pre-registered (section 10) | result |
|---|---|
| c6 server RSS about 2.45 MB, under CycloneDDS's 5,841 | **2,419 KB against CycloneDDS's 6,143: WIN** (was 13,061) |
| packets per sample unchanged, or the slots are binding | **2.29-2.31**, unchanged; `drained=acked` in all 3 reps |
| c4 server RSS unchanged (no loss, no slot touched) | 1,727 KB (was 1,731) |

c6 throughput rose to 663 Mbps (653-673), against step 1's 558 (468-625). The ranges do not overlap,
so the improvement is real, but it was not predicted and no mechanism has been checked. Retention
under loss is now 70.5% (663 of 940), still short of section 5 item 2's 90.6%.

The three non-wins:
- `server.loss_pct` at c4 and c6: every framework reads 0. That is not a target.
- **c4 `client.cpu_s_per_MB`: TIE at 0.005 against 0.005, which is a resolution artefact rather than
  a tie.** The RESULT line prints this metric as `%.3f`, and at p4 the values are about 0.005, which
  leaves one significant digit. The same quantity at usable resolution, `cpu_s_per_Msample`, is
  12.6 against 14.1 with no overlap, and the two differ only by the fixed sample size. In step 1 the
  same row read 0.004 against 0.005 and scored a WIN, so the verdict flips on rounding. The fix is
  more digits in all three harnesses' RESULT lines. Until then this row's verdict carries no
  information at p4.

Next: the p4 build that goes into COMPARISON.MD is `sendmmsg` (`c3d7a955`) or later. It is expected to
lower c4 client CPU toward the ipfrag build's 11.5 and leave bytes and packets unchanged.

## 12. FastDDS at c6: half the hypothesis confirmed, half not (2026-09-26)

`results/fastdds_c6_reassembly_2026-09-26.txt`, `experiments/fastdds_c6_reassembly.sh` (Dev's design,
pre-registration in its header). Server-side `/proc/net/snmp` deltas, FastDDS run through its own
`run_scenario.sh`:

| arm | delivered | ReasmReqds / sample | ReasmOKs | ReasmFails |
|---|---|---:|---:|---:|
| A control, p1 + 5% loss | 51,280 / 51,280 | 0.01 | 303 | 0 |
| B, p4, no loss | 184,691 / 184,691 | **2.98** | **184,691** | 36 |
| C, p4 + 5% loss | **2,895 / 5,192** | 4.32 | 4,711 | **0** |

- **A** is small but not zero: about 660 reassembly requests per arm from other traffic on that host.
  That is 0.1% of B and does not change B's or C's reading.
- **B confirms that FastDDS uses OS IP fragmentation at p4.** About three fragments per sample, and
  exactly one successful reassembly per sample sent. Its transport sets no `maxMessageSize` in
  `fastdds_eth0_only.xml`, so the 65,500 B default applies, and a 2,800 B sample leaves as one
  datagram for the kernel to split.
- **C does not confirm the collapse.** The pre-registration said a large ReasmFails would explain the
  shortfall. ReasmFails is **0**, so that explanation is **not confirmed**, and this is recorded
  rather than reinterpreted.

A possible instrument limitation, found **after** reading C and therefore unverified. The kernel
counts a reassembly failure when an incomplete datagram times out (`ipfrag_time`, 30 s by default) or
when fragment memory overflows. TickLE-ipfrag's failures were counted immediately, because at its rate
the fragment memory overflowed. FastDDS at c6 sends about 5,000 samples in 8 s and would not overflow
it, so its failures could fall due 30 s after the snapshot was taken. A re-read of the counters at
least 35 s after the arm ends would settle it.

What the run does establish:
- The shortfall is **not the harness stopping the server early**. Dev showed nothing arrives.
- FastDDS runs under the **same 3 s drain cap** as TickLE and CycloneDDS, and its client reports
  `drained=timeout` with 45 refused writes.
- The two suspects left are both FastDDS defaults that the harness leaves alone: IP fragmentation,
  confirmed by B, and heartbeat-paced recovery (`heartbeatPeriod`, 3 s by default).

So the DELIVERY FAILED label should read as **"did not complete within the drain cap every framework
gets"**, not as "lost". Whether it would finish given longer is not known. Under the section 10.1 rule,
TickLE's scoring does not depend on which mechanism it is. The tuned-FastDDS-arm question goes to the
user only once the mechanism is actually confirmed.

Follow-up, queued after the full campaign:
1. Re-read the server's counters 35 s after arm C ends.
2. Run arm C with a 30 s drain cap for FastDDS alone, as a diagnostic arm and not a scored one, to
   tell "slow" apart from "lost".

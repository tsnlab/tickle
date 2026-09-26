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

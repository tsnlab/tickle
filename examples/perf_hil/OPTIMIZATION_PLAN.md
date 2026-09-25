# Rig performance campaign: five metrics against both DDS vendors

**Status: DRAFT rev 2, awaiting the user's approval (step 2 of their own plan).** TickLE Plan,
2026-09-25. Rev 2 applies the user's four constraints (one hour, the existing network shaped only by
`tc`, four payload sizes, no QoS cross) and two findings from TickLE Dev's review.

Goal: compare memory, CPU, network bytes, latency and throughput against FastDDS and CycloneDDS
across QoS and `tc`-shaped network conditions, then optimise until TickLE wins each.

## 1. What already exists, and what does not

On the rig today (`COMPARISON.MD` §3, §3a, §3b): latency, throughput, and the nine QoS scenarios.
**Not measured anywhere: memory, CPU utilisation, network bytes.** `CpuPlace.h` is shared by all
three harnesses and reports `cpu_main` / `cpu_main_share` / `cpu_migrations` - *where* a process ran,
not how much CPU it used. No harness reads `VmHWM`, `getrusage()` or a packet count.

Where TickLE already leads: latency (0.20 ms against 0.25-0.29), throughput (118 Mbps against 72 and
48), memory under `rmw` on the dev box (16.8 MB against 21.7 and 35.0). **CPU and network bytes are
the genuine unknowns**, and are where this campaign can produce a surprise. TickLE's DATA framing is
28 B against RTPS's 60-76, which argues for it; the piggybacked Heartbeat every 64 samples and
broadcast discovery argue the other way.

## 2. The five metrics do not multiply the runs

One throughput run yields throughput, CPU per sample, peak RSS and wire bytes per sample at once.
Only latency needs its own shape. So the matrix is (run shape x payload x QoS x network), and the
metrics are a property of the instrumentation rather than of the cell count. This is what makes an
hour feasible at all.

| id | shape | yields |
|---|---|---|
| **T** | one-directional stream, `-d 5` | throughput (Mbps, msg/s), net loss, refused writes, CPU per delivered sample, peak RSS, wire bytes and packets per delivered sample |
| **L** | ping-pong at a low rate | RTT min/avg/max and a high percentile, CPU per sample, peak RSS, wire bytes per sample |

## 3. Payload sizes (the user's four cases)

Framing, both verified rather than assumed where possible:
- **TickLE DATA framing is 28 B** (`DESIGN.md`:569-571, `tt_Header` 4 + `tt_SubmessageHeader` 4 +
  `tt_DataHeader` 20). Single-datagram ceiling **1444 B**. *(README's "30" is BulkData's framing plus
  that message's own length prefix, which is payload encoding, not framing - Plan conflated the two
  in rev 1.)*
- **RTPS framing is 60-76 B** (header 20 + INFO_TS 12 + DATA submessage 24 + encapsulation 4, plus
  16 for an INFO_DST). Ceiling **1396-1412 B**. This is arithmetic from the spec, **not measured**,
  which is why §7's gate exists.

| id | sample (CDR) | `uint8[N]` | TickLE, 28 B framing | DDS, 60-76 B framing | the user's case |
|---|---|---|---|---|---|
| **P1** | 76 B | 64 | 104 B, 1 pkt | ~152 B, 1 pkt | small data |
| **P2** | 1388 B | 1376 | 1416 B, 1 pkt | 1448-1464 B, 1 pkt | fills one DDS packet |
| **P3** | 1440 B | 1428 | 1468 B, 1 pkt | 1500-1516 B, **2 pkt** | fills one TickLE packet, DDS splits |
| **P4** | 2800 B | 2788 | 2828 B, **2 pkt** (needs the 4096 build) | 2860-2876 B, 2 pkt | TickLE splits too |

Sizes are the **whole CDR sample**, not the payload array - rev 3 mixed the two in this table, which
is the one place the sizes get implemented from (TickLE Dev). All four are verified by generating
them: the generator emits `_Static_assert(sizeof(struct BenchData) == {76,1388,1440,2800})`.

**P2 is sized to the most constrained framing, not to TickLE's.** 1472 - 76 = 1396, less headroom,
gives 1388. TickLE then uses 1416 of its 1472, comfortably inside. Rev 1 used 1400, which was a
guess; this is TickLE Dev's arithmetic.

**P3 is deliberately asymmetric and must be read as such.** TickLE sends one datagram where the DDS
vendors send two, so under loss a two-fragment datagram is lost ~2x as often - at `loss 5%`, 9.75%
against 5%. **A "TickLE wins at large messages" claim must not be read off P3.** What P3 measures is
narrower and real: the cost, to each implementation, of the size where its own framing tips it over
the MTU. TickLE Dev flagged rev 1 for using 1442 B - tuned to TickLE's ceiling - as the *only* large
size, which would have been exactly that unearned claim. P2 is the like-for-like large size; P3 is
the boundary case the user asked for.

## 3a. What the rig measured, and what it says about the sizes above (2026-09-25)

The §3 framing figures above are spec arithmetic. The first campaign session measured them, and
§7's gate did the job it exists for: **P2 is mis-sized**. FastDDS sends 2.000 datagrams per sample
at P2, where the design says all three fit one, so every P2 cell in that session is VOID.

Per-sample overhead is *not* constant across sizes, so a one-point correction would have been
another guess. The structure is `overhead = base framing + ~42 B per extra IP fragment` - a second
fragment carries Ethernet+IP only, not UDP and the RTPS/TickLE headers again. Base framing, taken
from each framework's P1 cell, where packets per sample is 1.000:

| | TickLE | CycloneDDS | FastDDS |
|---|---|---|---|
| base framing, measured | **70.6 B** | **104.1 B** | **209.6 B** |
| §3's assumption | 28 B | 60-76 B | 60-76 B |
| largest sample in one frame | 1443.4 B | 1409.9 B | **1304.4 B** |

The per-fragment cost falls out independently in all three (TickLE P4 +42.4, CycloneDDS P3/P4
+44.1/+44.8, FastDDS P2/P3 +41.9, P4 at 2.979 pkt +88.8). The resulting rule
`sample + base framing <= 1514` - 1500 MTU + 14 B Ethernet, since the interface counters include L2
- then predicts **all twelve observed packet counts with zero mismatches**, which is why this is a
model rather than an extrapolation.

Note what the measured numbers are *not*: TickLE's 70.6 B is not a refutation of the 28 B DATA
framing, and FastDDS's 209.6 B is not RTPS framing. Both include Ethernet+IP+UDP (42 B) and each
implementation's own per-sample control traffic. 70.6 - 42 = 28.6 B is TickLE's DATA framing,
matching `DESIGN.md` exactly. FastDDS's 167.6 B above L4 is the figure that is 2-3x the RTPS
arithmetic in §3, and worth its own look - it is most of why FastDDS loses the wire-bytes metric.

**Consequences, both deferred to the next session rather than patched mid-campaign:**

- A fair P2 needs `sample <= 1304`, so `uint8[N]` with **N <= 1292** instead of 1376. N = 1280
  (sample 1292 B) leaves 12 B of margin. P2's cells are not to be carried into `COMPARISON.MD` from
  the session that used 1376.
- **P3 is correct but has 3.4 B of headroom.** At 1440 B TickLE's frame is 1510.6 against 1514. It
  works, and for exactly the intended reason - 1440 sits between CycloneDDS's 1409.9 and TickLE's
  1443.4 - but any future change to TickLE's framing flips that cell silently, and it would read as
  a TickLE regression in packet count rather than as a test that lost its premise. Either move P3
  to ~1400, or have `check_bench_shapes.sh` assert the computed frame size against these
  per-framework limits so the premise is gated rather than re-measured.

**One metric was withdrawn, after seeing data** (`3d6550f4`). `utime_s` and `stime_s` were verdict
metrics; they cannot be. Every throughput cell runs for a fixed *duration*, so a framework that
sends more samples necessarily burns more CPU seconds. At c1 that inverts the ranking outright -
FastDDS beats TickLE on absolute `stime_s` while doing a third of the work in the same five
seconds. They are still reported per cell, marked not-comparable; `cpu_s_per_Msample` and
`cpu_s_per_MB` are the normalised forms and remain the verdict metrics. The change is post-hoc and
removes a TickLE LOSE, which is the direction to be suspicious of - the reason to accept it is that
it does not depend on which way it fell: had TickLE been the slow one, the same metric would have
handed it a free WIN by the same mechanism.

**P4 needs a second TickLE build** with `-Dtt_MAX_BUFFER_LENGTH=4096`, so a 2828 B datagram leaves
the node and the kernel IP-fragments it. The DDS vendors need no such change: they fragment at the
protocol level (`DATA_FRAG`) automatically. **That is a genuine mechanism difference, not a
confound**, and it is the interesting part of P4: a lost IP fragment loses the whole TickLE datagram,
while RTPS can retransmit one fragment. Under loss, TickLE is expected to lose P4, and that
expectation is recorded here so the result is a finding either way. Two consequences to state: the
`tt_Node` tx/rx buffers are `2N` each, so the P4 build carries ~10 KB more node buffer than the
others - immaterial against a peak RSS in the megabytes, but the P4 memory figure is not strictly
comparable to P1-P3's for TickLE.

## 4. One IDL change, needed for the small-size metric to mean anything

`Bench.idl` is `unsigned long seq; unsigned long long send_ns; octet payload[N]`. TickLE's CDR-4 puts
`send_ns` at offset 4, giving 76 B - which the existing `_Static_assert` pins. **Standard CDR aligns
a 64-bit field to 8, so the DDS vendors insert 4 bytes of padding and put 80 B on the wire for the
same IDL** (TickLE Dev's finding). At P1 that is a 5% payload difference landing inside the one
metric the small size exists to measure, and it would be charged to framing.

**Fix: put `send_ns` first.** `unsigned long long send_ns; unsigned long seq; octet payload[N]` is
8 + 4 + N with no padding under either rule, so all three agree exactly at every size.

This does not invalidate any published figure. `COMPARISON.MD` §3's Mbps is computed as
`recv_count x 76 x 8 / elapsed` for all three alike, so it is application-payload throughput and is
unaffected. It is the *new* wire-bytes metric that the padding would have distorted.

## 4b. TickLE's harness codec moves to the generator

The user's decision, 2026-09-25: *"DDS가 Bench.idl을 사용하듯이 TickLE 또한 Bench.msg를 사용하는 것이
어떨까? codec 부분을 손으로 쓰는 부분이 있다면 일괄 삭제하고 xxx.msg 형태로 유지하는 것이 좋을 것 같아."*
Approved and in scope for step 2.

`tickle/common/Bench.h` is hand-written today, and its own header comment says it "mirrors
`idl/Bench.idl`'s exact field shape". Three reasons this has to change before the campaign runs, not
after:
- **The benchmark does not currently measure the product path.** A real TickLE user gets a
  `tools/typesupport`-generated codec. Measuring a hand-written one measures something users do not
  have - the same class of error as measuring a stale library, and worse for a published figure.
- **It is the only place in this comparison that can diverge silently.** Both DDS vendors generate
  from one `Bench.idl`, so `idlc` and `fastddsgen` cannot disagree by construction - the IDL's own
  comment calls that "a byte-identical source guarantee, not a hand-matched one". TickLE's side is
  precisely the hand-matched one, in a campaign whose headline metric is bytes on the wire.
- **Four payload sizes are about to multiply it.** One hand-written codec becoming four quadruples
  the places it can drift. Now is when it costs least.

Two checks that belong with the change, not after it:
- The generated struct must still be **76 B at P1**, which the existing `_Static_assert` already
  pins. If the generator lays `octet payload[64]`'s equivalent out differently, the comparison
  changes and that has to be seen, not discovered later.
- **The field reorder of §4 applies to both `Bench.msg` and `Bench.idl`.** Moving to the generator
  does not fix the padding by itself.

`examples/perf/Bulk.msg` already uses the generator, so `perf_hil`'s TickLE harness is the lone
exception. Removing it makes "every TickLE example uses the generator" a rule rather than a habit,
which is what stops the next hand-written codec.

## 5. QoS cells

Baseline **Q0**, one factor at a time - the user confirmed no QoS cross is wanted.

| id | reliability | history | durability | how each side selects it |
|---|---|---|---|---|
| **Q0** | RELIABLE | **KEEP_ALL** | VOLATILE | `-Q` for TickLE; the DDS harnesses' own default |
| Q1 | BEST_EFFORT | KEEP_LAST | VOLATILE | the `best_effort_throughput` scenario |
| Q2 | RELIABLE | **KEEP_LAST 64** | VOLATILE | TickLE's default; needs a matching option in both DDS harnesses |

**Q0 is KEEP_ALL, not KEEP_LAST, because KEEP_LAST is not runnable across all three as rev 3
specified it.** Both DDS `reliable_throughput` harnesses are hard-coded KEEP_ALL with
`resource_limits` and take only `-d -i -B`; TickLE defaults to KEEP_LAST and takes `-Q`. Rev 3's
baseline would therefore have run TickLE at KEEP_LAST against DDS at KEEP_ALL and reported it as
like-for-like - the exact defect §3a records and §3b exists to correct. KEEP_ALL is the only
configuration in which all three make the same promise. Found by reading the harnesses before
writing the sweep, not by running it.

**Q2 exists because KEEP_LAST is what users actually get** - TickLE's default and rclcpp's - so
without it the campaign would not measure the default configuration at all. Depth 64 specifically:
both DDS harnesses' comments record KEEP_LAST(8) as the bisected cause of a real 53% loss, so a
shallow depth would re-measure that finding instead of the default. If the DDS option is not cheap,
Q2 becomes a TickLE-only datapoint, labelled as such rather than as a comparison.

Depth 1024 and TRANSIENT_LOCAL are **cut for the hour budget**, and are the first things to add if a
second session is approved.

## 6. Network conditions

The user's constraint: the existing network, shaped only by `tc` for reorder, delay and loss. Rev
1's 10 Mbit rate cap is dropped accordingly.

| id | `tc` on eth0 | why |
|---|---|---|
| **N0** | none | baseline |
| N1 | `loss 5%` | moderate loss; the level §3a and §3b both use |
| N2 | `delay 10ms jitter 2ms` | makes the retransmit round trip the binding cost rather than the link. Every TickLE retry interval was tuned against a sub-millisecond RTT |
| N3 | `delay 1ms reorder 5%` | reordering, which **RELIABLE's strict in-order delivery now has to absorb** (`0b415269`, `d63860c7`). No existing sweep tests it, and it is the newest code in the reliable path |

N3 has never been run. `netem` needs a delay for `reorder` to have anything to reorder against,
hence the 1 ms.

## 7. The matrix: 12 combinations

| # | shape | payload | QoS | network | what it is for |
|---|---|---|---|---|---|
| 1-4 | T | P1, P2, P3, P4 | Q0 | N0 | the core comparison across all four sizes, KEEP_ALL so all three promise the same |
| 5 | T | P1 | Q0 | N1 | loss at the small size |
| 6 | T | P4 | Q0 | N1 | loss where TickLE is IP-fragmented - the §3 risk, measured |
| 7 | T | P1 | Q0 | N3 | reorder against strict ordering |
| 8 | T | P1 | Q1 | N0 | what reliability costs |
| 9 | T | P1 | Q2 | N0 | KEEP_LAST 64 - the default configuration users get |
| 10-11 | L | P1, P2 | Q0 | N0 | RTT at both comparable sizes |
| 12 | L | P1 | Q0 | N2 | RTT under a real network delay |

**12 x 3 frameworks x 3 repetitions = 108 runs.** The 2026-09-24 re-sweep ran 243 cells plus two
build phases in 77 minutes, i.e. ~19 s a cell. At 25 s that is **45 minutes**, plus ~8 minutes of
builds including the second TickLE build for P4: **~53 minutes**, inside the hour with margin.

Three repetitions, not two: every published figure uses three, and `COMPARISON.MD` reports the
spread rather than a mean alone. Frameworks are interleaved within each repetition so a drift lands
on all three.

## 8. Instrumentation, and the fairness rule

**Every counter goes into a header shared by all three harnesses, as `CpuPlace.h` already is, with
the same `RESULT:` field names and units.** Instrumenting TickLE alone, or better, turns a
comparison into a claim about the instrument. Precedent: the CPU-pinning fix (`9f70d9b4`) applied to
one harness would have handed TickLE 15% a quarter of the time.

| metric | how | reported as |
|---|---|---|
| CPU | `getrusage(RUSAGE_SELF)` utime+stime, both processes | `cpu_s_per_Msample`, `cpu_s_per_MB`, raw `utime_s`/`stime_s` |
| memory | `VmHWM` from `/proc/self/status` at exit | `peak_rss_kb` |
| network | `/proc/net/dev` eth0 byte and packet deltas, both hosts | `wire_bytes_per_sample`, `wire_packets_per_sample`, `wire_bytes_total` |

**CPU is normalised per delivered sample and per megabyte, never as a percentage.** The three run at
up to 3x different rates, so CPU% compares three different workloads. Both normalisations are needed:
per-sample favours large payloads, per-megabyte favours small ones.

**`/proc/net/dev` on eth0 is sound, and TickLE Dev measured it rather than assuming.** The rig's
control plane (SSH, orchestration, internet) is on wlan0 via the default route; eth0 is a direct
192.168.10.x pair with no gateway and no third host. Idle eth0 over 20 s showed a **byte- and
packet-identical** counter at both ends - not small background traffic, none. All three frameworks
are already pinned to eth0 (CycloneDDS `<NetworkInterface name="eth0"/>`, FastDDS
`fastdds_eth0_only.xml`, TickLE `_tt_CONFIG.broadcast` 192.168.10.255), and every `tc qdisc` in
`examples/perf_hil/` is on eth0, so the shaped, measured and data interfaces are the same one.

`tickle_pcap_count.py` is kept for the diagnostic question of *what* those bytes were (DATA against
ACKNACK against discovery), which step 6 will want - not something to run 108 times.

## 9. How the results will be read, written before running

- **Instrument gates, checked before any cell is believed.** Idle eth0 is measurably a zero delta, so
  a zero byte counter during a run is an instrument failure, not a quiet link. Same for a zero CPU
  counter. The harness must say so rather than report a zero.
- **The payload-boundary gate, free from a metric already planned**: at N0,
  `wire_packets_per_sample` must read **1.0 for all three at P1 and P2**, **1.0 for TickLE and 2.0
  for both vendors at P3**, and **2.0 or more for all three at P4**. If a size does not produce its
  intended split, that size was computed wrong and its cross-vendor comparison is **void** - the RTPS
  framing figure is spec arithmetic, not a measurement, and this is what checks it.
- **A cell is void** if its leftover guard fired, if it produced no `RESULT:` line, or if a framework
  did not end `drained=acked` where expected. Void is reported as void, never as zero.
- **Comparisons are within this session only.** The rig carries day-to-day offsets of ~13 us on
  latency across all implementations at once (`COMPARISON.MD` §5). Published figures are compared
  only as "moved / did not move".
- **TickLE wins a cell** when it beats *both* vendors on that metric, outside the spread of the three
  repetitions. Inside the spread is a draw, not a win.
- **Where TickLE does not win, that cell is an optimisation target and needs a named hypothesis
  before any code change** - the §3b lesson, where 108.5 to 84 Mbps was read as the price of ordering
  and was actually a linear scan.
- **Two results are expected rather than hoped for, and are recorded now so that neither is read as a
  surprise**: TickLE should lose P4 under N1, because a lost IP fragment costs it the whole datagram
  while RTPS retransmits one fragment; and TickLE may lose memory under Q2, because it preallocates
  its cache to the resource limit while CycloneDDS allocates as samples arrive. If either happens,
  the answer is the lazy growth that now exists (`81c8186c`) or documenting the trade - not calling
  the cell a draw.

## 9a. Optimisation targets from the 2026-09-25 campaign

108/108 runs, `instrument=ok` on all 189 RESULT lines. WIN 62, DRAW/TIE 9, LOSE 7, VOID 30. Raw
output and computed verdicts are in `results/campaign_2026-09-25_b9fad3c1*.txt`.

The LOSEs are not scattered. They fall into three targets, and each gets a named hypothesis and the
measurement that would **disprove** it, before any code changes - section 9's rule.

### A. The retransmission storm at P4 under loss - ANSWERED, and deferred by the user

**Resolved 2026-09-25: it is the kernel's IP reassembly, not TickLE.** At 5% loss with a 2800-byte
sample, 97.4% of the receiving kernel's reassembly attempts fail; the 681,461 datagrams that never
reassembled are 46.8 per delivered sample against 47.3 transmissions per sample from the interface
counters - the same number, so the wire amplification *is* the reassembly failure. Both controls
clean: no fragmentation at 76 bytes, and flawless fragmentation with nothing lost. A1, A2 and A3
were all ruled out inside core first, each with a control. Full record in `COMPARISON.MD` item 15;
harness `experiments/a4_reassembly_check.sh`, raw output
`results/a4_reassembly_2026-09-25.txt`.

**The user's decision is to leave OS IP fragmentation as it is and keep it on the to-do list**, so
this is no longer an optimisation target. Nothing below is to be actioned; it is kept because the
reasoning that got here is worth not repeating, and because the c6 cell still has to be re-run
before it can be compared at all.

#### Superseded working notes (kept for the record)

**c6 as a campaign cell cannot be compared, and `loss_pct` did not say so.** All three servers capped
themselves at `-d + 15` s absolute, and at c6 that truncated two of the three:

| | sent | recv | missing | reported loss_pct |
|---|---|---|---|---|
| TickLE | 14,002 | 13,802 | 1.4% | **0.0** |
| CycloneDDS | 2,807 | 2,807 | 0% | 0.0 |
| FastDDS | 4,987 | 1,641 | **67%** | **0.0** |

`loss_pct` measures gaps between the sequence numbers that *arrived*; samples that never arrived
at all leave no gap to count, so it reads a clean zero across a 67%-vs-1.4% difference and would
have been scored a TIE. The summariser now voids any RELIABLE cell with `sent != recv` (not the
BEST_EFFORT cell, where dropping is the promise). TickLE Dev fixed the cap in all three frameworks
at once (`0da3cad5`) - fixing only TickLE's would have handed it samples the other two were still
cut off from.

**The storm survives that, because it does not rest on c6.** It rests on arm B of
`experiments/c6_server_lifetime_check.sh`, where the server was given `-d 60`, `peer_acks_end=1`
throughout, and the client still spent **183 packets per 2800-byte sample**:

| arm | peer_acks_end | packets/sample | amplification |
|---|---|---|---|
| A control, no loss, `-d 60` | 1 | 2.01 | 1.0x |
| **B, 5% loss, `-d 60`** | **1** | **183.4** | **95.4x** |
| C, 5% loss, `-d 5` (campaign) | 0 | 149.3 | 77.6x |

Arm A reproduces c4 exactly (211,686 against 211,690 samples), so the longer lifetime changes
nothing by itself. A 2-fragment datagram at 5% per-packet loss is lost 1-(1-0.05)^2 = 9.75% of the time, so the ideal
cost is 1/(1-0.0975) = 1.11 transmissions, i.e. **~2.2 packets per sample**. TickLE spends 103.

**Two things were excluded before this became a target**, because each would have made it a
measurement artefact rather than a finding:

- *Does `/proc/net/dev` count packets netem drops?* If it did, every N1 cell would be inflated.
  `experiments/netem_counter_check.sh`, two arms: no netem `tx_packets +100` (the control - the
  counter can see packets), `loss 100%` `tx_packets +0`. The counter reflects the wire.
- *Did the server exit mid-drain, leaving the client retransmitting to a dead peer?* That is arm B
  above, and the answer is no - the peer stayed alive and the amplification got worse, not better.
  The truncation is real and separate: it is what makes c6 itself void.

**A1 and A2 are both ruled out at the unit level** (TickLE Dev, `38b8a503`), each with the control
that separates a true negative from a test that cannot fail.

- *A1, the retransmit is window-wide rather than gap-wide.* 1024 samples into a 1024-deep cache, one
  ACKNACK naming ten scattered positions (0, 1, 63, 64, 255, 256, 257, 511, 700, 1023): exactly
  those ten go out and no others, checked per slot via each slot's own `retry` counter so that the
  right *number* of wrong samples would still fail. Mutation-tested with a Publisher that also
  resends each named sample's neighbour.
- *A2, the ACKNACK bitmap cannot name gaps past bit 256.* `send_acknack_range()` sizes the bitmap to
  the subscriber's own window (up to `tt_RELIABLE_BITMAP_MAX_BITS`, 4096), not to
  `tt_RELIABLE_BITMAP_BITS`. A subscriber with a 1024-sample window names gaps at 5, 100, 255, 256,
  300 and 700 in one ACKNACK. Mutation-tested with a bitmap capped at 256 bits, which fails with
  exactly 256, 300 and 700 missing.

**Hypothesis A3 - the subscriber's tracking window overflows while the watermark is stalled.** The
campaign passed `-Q` and nothing else, and `run_scenario.sh` forwards the same arguments to both
sides, so c6's server ran with the *default* window - `tt_RELIABLE_BITMAP_BITS`, 256 samples - while
its reorder buffer is sized at 4096. A lost fragment stalls the watermark; samples arriving more
than 256 beyond it have nowhere to be recorded and must be asked for again once it moves.

**Two constraints A3 has to satisfy, both from data that already exists:**

1. **c5 is the control, and it shows nothing.** P1 under the same 5% loss, the same `-Q`, the same
   256-sample default window, the same 2048 depth - and 56x more samples in flight, so the publisher
   runs *further* ahead of a stalled watermark, not less:

   | | sample | fragments | sent | packets/sample | amplification |
   |---|---|---|---|---|---|
   | c5 | 76 B | 1 | 791,770 | **1.00** | 2.0x |
   | c6 | 2800 B | 2 | 14,002 | 103.27 | 53.6x |

   An identical window configuration produces no amplification at P1. So the window overflowing is
   not sufficient on its own; something about the two-fragment sample is required as well, and the
   loss rate does not cover it - 9.75% against 5% is a factor of 2 against an amplification of 50+.

2. **The magnitude.** Arm B's 183.4 packets per sample at 2 fragments is ~92 transmissions per
   sample against an ideal of 1.11. A3, modelled as "a sample d past the stalled watermark goes out
   about d/256 times, averaged over a 2048 depth", predicts ~4x - short by a factor of 23. Any
   proposed mechanism has to produce ~92, and that number is written here so a mechanism that
   explains 4x can be recognised as partial rather than accepted as the answer.

**Hypothesis A4 - the receive-side IP reassembly queue, not TickLE's retransmit logic at all.** At
P4 the kernel IP-fragments the datagram. A lost fragment leaves its partner in the reassembly queue
until `ipfrag_time` (30 s), and a queue reaching `ipfrag_high_thresh` makes the kernel drop *other*
datagrams too - a receive-side collapse whose effective loss is nothing like 9.75%. At P1 there is
no reassembly at all, which is exactly the axis c5 and c6 differ on. **Disproof:** `ReasmFails` and
`ReasmReqds` from `/proc/net/snmp` on the server across a c6 run. If reassembly failures account for
the bulk of it, the mechanism is not in TickLE's retransmit path and A3 is looking in the wrong
file.

**Queued for the next rig session**, all cheap: re-run c6 with `-w` matched to the client's depth
(A3's direct disproof - the amplification should collapse), with `/proc/net/snmp` sampled either
side (A4), and with `-Dtt_RELIABLE_STATS` so the retransmit counters are visible rather than
inferred from interface totals.

**What must not be read off this cell:** c6's throughput figures are over truncated runs for TickLE
and FastDDS, so the 5x sample lead there is not a result. What is one, from arm B: even with a live
peer and complete delivery, the cost of that throughput is 95x the wire bytes. On
10Base-T1S at 10 Mbps, 2.10 GB is 28 minutes of wire time. The throughput win and the bandwidth loss
are the same behaviour seen from two sides, and the target platform is the one where it is a loss.

### B. A fixed-rate poll in the latency path (c10, c11, c12, 6 of the 7 LOSEs)

| | stime_s per run | stime per sample | rtt_avg_ms |
|---|---|---|---|
| TickLE c10 (76 B) | 0.630 | 6.30 ms | **0.204** |
| TickLE c11 (1388 B) | 0.631 | 6.31 ms | **0.232** |
| TickLE c12 (76 B, +10 ms) | 0.622 | 6.22 ms | 10.032 |
| CycloneDDS, all three | 0.010-0.014 | ~0.13 ms | 0.358-10.438 |

**B1 CONFIRMED, with its control (2026-09-25).** `experiments/b1_syscall_count.sh`, raw output
`results/b1_syscalls_2026-09-25.txt`.

    TickLE latency client, strace -c:   ppoll  38,503 calls   99.69% of system time   11 us/call
                                        sendto     18
                                        recvfrom   60

    no shaping   77,398 syscalls, 5 round trips, rtt 0.344 ms
    +10 ms delay 77,396 syscalls, 5 round trips, rtt 10.076 ms      <- two calls' difference
    CONTROL, CycloneDDS, identical strace, same 5 round trips: hundreds of syscalls in total

38,503 ppolls in a ~5 s run is ~7,700/s under strace, consistent with a 100 us cadence. The count
tracks neither traffic nor samples. The control is what makes that mean something: strace is not
generating the calls.

**The mechanism, and it is narrower than "TickLE polls".** `tt_Node_poll()` already computes the
wait from the scheduler - `rest = min(timeout, next_due - now)` - so the scheduler's next entry can
make the wait *shorter*. What it cannot do is raise the ceiling, and four lines into the function a
negative timeout is normalised to `tt_RECEIVE_TIMEOUT`, 100 us. So a caller asking to block gets a
100 us ceiling, and with nothing due for 50 ms the loop still wakes 500 times.

**The user's instruction (2026-09-25): compute the poll timeout from the scheduler and reduce the
number of poll calls.** Core work, with TickLE Dev.

**What must not be traded away.** TickLE has the lowest RTT of the three (0.204 ms against 0.358 and
0.282) and the poll cadence is the plausible reason; CycloneDDS blocks and pays thread handoff
instead - 234 futex calls for 5 round trips in the control. So the controls are as important as the
target: **c10's RTT must not get worse**, and c1's throughput must not move, since the same loop
carries the publisher's send path and `tt_SCHEDULER_IO_INTERLEAVE` exists precisely because a
max-rate publisher can starve `tt_receive()`. A version that cuts CPU and loses the latency is a
loss, not a win. Pre-registered target: `ppoll` down at least 10x at c10.

### C. Memory at P4 (c4, the remaining LOSE) - already modelled

`peak_rss_kb` = C + reliable_depth x record_bytes, with C = 1720/1727/1727/1733 KB across the four
shapes - a 13 KB spread against a 5464 KB range, so the retention window is the whole story. At P4
the touched arena is 5651 KB of a 7384 KB peak.

**Hypothesis C1 - the default depth is a constant sample count where it should be a byte budget.**
`reliable_depth` is 2048 samples regardless of sample size, so the *unacked bytes* grow linearly
with the payload. **An earlier draft of this described it as an allocation-strategy problem, which
was wrong** (TickLE Dev): rmw_tickle's arena already grows lazily from 64 KiB, and peak RSS tracks
pages touched, so under KEEP_ALL with a full window the touched pages *are* the unacked samples.
Lazy allocation therefore cannot move the P4 figure. The only lever is fewer unacked bytes - a byte
budget works because it blocks the writer sooner, not because it reserves less. Predicted peak RSS at depth 1024 is 4584 KB against CycloneDDS's 5544 - a win - and at 512,
3184 KB.

**Why this is not simply "reduce the depth".** A smaller window is exactly where KEEP_ALL starts
dropping unacked samples on the byte bound, which is a proven path rather than a theoretical one. A
fixed byte budget with depth derived from it would win the large sizes without touching the small
ones. **Disproof:** if c4's memory advantage at depth 1024 comes with any increase in
`evicted_by_bytes` or in `sent != recv`, the budget is too small and the trade is not free.

### Not targets

- **c12's `rtt_avg_ms` DRAW** (10.2 / 10.5 / 10.3). A 10 ms delay dominates a 0.2 ms RTT; the cell
  measures netem. Correct behaviour, not a finding.
- **The 20 VOIDs**: c2 (FastDDS splits at the old P2 of 1388 B) and c9 (TickLE ran KEEP_LAST against
  two vendors hard-coded to KEEP_ALL). Both are fixed for the next session by the re-sized P2/P3
  (1957a4b1) and `-K` on both DDS harnesses (5fddc985), not by optimising anything.
- **The server's `-d + 15` cap**, now fixed in all three frameworks (`0da3cad5`): an absolute cap
  until the first sample arrives, then an idle cap measured from the last one. It is harness work,
  not a TickLE optimisation, and c6 has to be re-run before it can be compared at all.

## 10. Sequence

1. **This draft approved or changed** by the user.
2. **TickLE Dev**: (i) replace `tickle/common/Bench.h`'s hand-written codec with a
   `tools/typesupport`-generated one from a new `Bench.msg` (§4b), deleting the hand-written codec
   rather than keeping both; (ii) the field reorder of §4, in `Bench.msg` *and* `Bench.idl`; (iii) the
   four payload shapes - two lines of IDL for the DDS pair, and the generator for TickLE, so all
   three stay single-source; (iv) the shared instrumentation of §8, with the zero-counter failure
   built in.
3. **TickLE Plan**: the sweep script extending `comparison_resweep.sh` - the 12 combinations, the
   four `tc` conditions, the two TickLE builds, the leftover guard, timestamped output.
4. **Baseline pass on the rig**, holding the hil lock, ~53 minutes.
5. **`COMPARISON.MD` gains a section per metric**, raw output committed under
   `examples/perf_hil/results/`.
6. **Optimisation targets** from §9, each with a hypothesis and a pre-registered reading.
7. **Optimise, then re-run the affected cells and the unaffected ones as the control**, because a
   change that helps one cell and quietly costs another is the failure mode this document exists to
   avoid.

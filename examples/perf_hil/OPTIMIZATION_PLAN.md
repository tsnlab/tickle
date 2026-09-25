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

108/108 runs, `instrument=ok` on all 189 RESULT lines. WIN 65, DRAW/TIE 10, LOSE 13, VOID 20. Raw
output and computed verdicts are in `results/campaign_2026-09-25_b9fad3c1*.txt`.

The 13 LOSEs are not scattered. They fall into three targets, and each gets a named hypothesis and
the measurement that would **disprove** it, before any code changes - section 9's rule.

### A. The retransmission storm at P4 under loss (c6, 6 of the 13)

| | wire B/sample | amplification | packets/sample |
|---|---|---|---|
| **TickLE** | **150,065** | **53.6x** | **103.3** |
| CycloneDDS | 3,235 | 1.2x | 2.73 |
| FastDDS | 3,132 | 1.1x | 2.28 |

A 2-fragment datagram at 5% per-packet loss is lost 1-(1-0.05)^2 = 9.75% of the time, so the ideal
cost is 1/(1-0.0975) = 1.11 transmissions, i.e. **~2.2 packets per sample**. TickLE spends 103.

**Two things were excluded before this became a target**, because each would have made it a
measurement artefact rather than a finding:

- *Does `/proc/net/dev` count packets netem drops?* If it did, every N1 cell would be inflated.
  `experiments/netem_counter_check.sh`, two arms: no netem `tx_packets +100` (the control - the
  counter can see packets), `loss 100%` `tx_packets +0`. The counter reflects the wire.
- *Did the server exit mid-drain, leaving the client retransmitting to a dead peer?* c6 was the only
  cell of 108 with `sent != recv` and `peer_acks_end=0`, and `server.c` caps itself at `-d + 15` =
  20 s while c6 needs >= 16.8 s of wire time. `experiments/c6_server_lifetime_check.sh`, three arms:
  a no-loss control at `-d 60` reproduces c4 exactly (211,686 vs 211,690 samples, 2.01 pkt/sample),
  so the longer lifetime changes nothing by itself; and **at `-d 60` under loss the peer stays alive
  (`peer_acks_end=1`) and the amplification is 95.4x, 183 packets per sample.** The storm is not the
  dead peer.

**Hypothesis A1 - the retransmit is window-wide rather than gap-wide.** A NACK causes the whole
unacked window to be resent instead of only the missing sequence numbers. At depth 2048 and 2
fragments that is 4096 packets per event, and 1,446,030 / 4096 = 353 such events over the run.

**Hypothesis A2 - the ACKNACK bitmap cannot express the gaps.** `tt_RELIABLE_BITMAP_BITS` is 256
bits against a 2048-sample window, so losses spread beyond 256 sequence numbers cannot all be named
in one NACK, and the recovery degrades to resending from the oldest unacked sample.

A1 and A2 predict the same amplification and are told apart by *which* samples go out, not how many.
**Disproof for both:** build the P4 harness with `-Dtt_RELIABLE_STATS` (the counters already exist,
`include/tickle/reliable_stats.h`) and re-run c6. If retransmissions are gap-sized - within a small
multiple of the ~1400 samples that 9.75% loss over 14,002 samples implies - both are wrong and the
cost is somewhere else entirely. If A2 holds, raising `tt_RELIABLE_BITMAP_BITS` to cover the window
should cut the amplification; if A1 holds, it will not move.

**What must not be read off this cell yet:** TickLE still delivers 5x the samples at c6 (62.7 Mbps
against 12.0 and 13.8), and that win is real on a 1 Gbps link with bandwidth to waste. On
10Base-T1S at 10 Mbps, 2.10 GB is 28 minutes of wire time. The throughput win and the bandwidth loss
are the same behaviour seen from two sides, and the target platform is the one where it is a loss.

### B. A fixed-rate poll in the latency path (c10, c11, c12, 6 of the 13)

| | stime_s per run | stime per sample | rtt_avg_ms |
|---|---|---|---|
| TickLE c10 (76 B) | 0.630 | 6.30 ms | **0.204** |
| TickLE c11 (1388 B) | 0.631 | 6.31 ms | **0.232** |
| TickLE c12 (76 B, +10 ms) | 0.622 | 6.22 ms | 10.032 |
| CycloneDDS, all three | 0.010-0.014 | ~0.13 ms | 0.358-10.438 |

**Hypothesis B1 - the receive path polls at a fixed rate independent of traffic.** The evidence is
what does *not* move: TickLE's system time is 0.630, 0.631 and 0.622 s across an 18x payload
difference and a 50x RTT difference. It is neither per-byte work (c11 carries 18x the payload for
the same CPU) nor time spent waiting for the pong (c12 waits 50x longer for identical CPU). A
constant ~0.63 s over a ~5 s run is 12.6% of one core, spent whatever the traffic does.

**Disproof:** `strace -c -f` one latency client run, or `perf stat -e syscalls`. B1 predicts a syscall
count that is roughly constant across c10 and c12 and far larger than 100 - order 10^5, since 0.63 s
of kernel time at a few microseconds per call is ~10^5 calls. If the syscall count instead tracks
the 100 samples, B1 is wrong and the cost is per-sample work in the send/receive path.

**The trade this is not.** TickLE has the lowest RTT of the three in both ungated latency cells
(0.204 against 0.358 and 0.284). A tight poll buys that. The question for section 7 is whether the
same RTT survives a poll interval chosen for the target platform, not whether to keep the latency.

### C. Memory at P4 (c4, 1 of the 13) - already modelled

`peak_rss_kb` = C + reliable_depth x record_bytes, with C = 1720/1727/1727/1733 KB across the four
shapes - a 13 KB spread against a 5464 KB range, so the retention window is the whole story. At P4
the touched arena is 5651 KB of a 7384 KB peak.

**Hypothesis C1 - the default depth is a constant sample count where it should be a byte budget.**
`reliable_depth` is 2048 samples regardless of sample size, so the arena grows linearly with the
payload. Predicted peak RSS at depth 1024 is 4584 KB against CycloneDDS's 5544 - a win - and at 512,
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
- **The server's `-d + 15` cap**, which is a real harness defect - it explains c6's
  `peer_acks_end=0` and its 200 undelivered samples and nothing else - but it is harness work, not
  a TickLE optimisation.

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

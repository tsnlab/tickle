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

| id | payload | TickLE | DDS | the user's case |
|---|---|---|---|---|
| **P1** | 64 B | 1 pkt | 1 pkt | small data |
| **P2** | 1388 B | 1 pkt | 1 pkt | fills one DDS packet |
| **P3** | 1440 B | 1 pkt | **2 pkt** | fills one TickLE packet, DDS splits |
| **P4** | 2800 B | **2 pkt** | 2 pkt | TickLE splits too |

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

| id | reliability | history | durability |
|---|---|---|---|
| **Q0** | RELIABLE | KEEP_LAST 64 | VOLATILE |
| Q1 | BEST_EFFORT | KEEP_LAST 64 | VOLATILE |
| Q2 | RELIABLE | **KEEP_ALL** | VOLATILE |

Q3 (depth 1024) and Q4 (TRANSIENT_LOCAL) from rev 1 are **cut for the hour budget**. They are the
first thing to add if a second session is approved.

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
| 1-4 | T | P1, P2, P3, P4 | Q0 | N0 | the core comparison across all four sizes |
| 5 | T | P1 | Q0 | N1 | loss at the small size |
| 6 | T | P4 | Q0 | N1 | loss where TickLE is IP-fragmented - the §3 risk, measured |
| 7 | T | P1 | Q0 | N3 | reorder against strict ordering |
| 8 | T | P1 | Q1 | N0 | what reliability costs |
| 9 | T | P1 | Q2 | N0 | KEEP_ALL, where TickLE's preallocation may lose on memory |
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

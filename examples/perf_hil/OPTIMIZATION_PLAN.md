# Rig performance campaign: five metrics against both DDS vendors

**Status: DRAFT, awaiting the user's approval (step 2 of their own plan).** Written by TickLE Plan,
2026-09-25, at the user's request: compare memory, CPU, network, latency and throughput against
FastDDS and CycloneDDS across a range of QoS combinations and `tc`-shaped network conditions, then
optimise TickLE until it wins each one.

## 1. What already exists, and what does not

Measured today on the rig (`COMPARISON.MD` §3, §3a, §3b): latency, throughput, and the nine QoS
scenarios. **Not measured anywhere on the rig: memory, CPU utilisation, network bytes.**

`examples/perf_hil/CpuPlace.h` is shared by all three frameworks' harnesses and reports `cpu_main`,
`cpu_main_share` and `cpu_migrations` - *where* the process ran, not how much CPU it used.
`CpuFreq.h` is TickLE-only. No harness reads `VmHWM`, `getrusage()` or a packet count.

Where TickLE already leads, from figures we hold: latency (0.20 ms against 0.25-0.29), throughput
(118 Mbps against 72 and 48), and memory under `rmw` on the dev box (16.8 MB against 21.7 and 35.0).
**The two genuine unknowns are CPU and network bytes**, and they are where this campaign can produce
a surprise. TickLE's per-message framing is 30 B against RTPS's ~50-60, which argues for it; the
piggybacked Heartbeat every 64 samples and broadcast discovery argue the other way.

## 2. The design problem: the matrix does not fit

A full factorial is out of reach. Reliability x history x durability x payload x rate is 48 QoS
cells; times 8 network conditions, 3 frameworks and 3 repetitions is 3456 runs, and a run costs
25-40 s on the rig including orchestration. That is weeks.

Three decisions make it fit in one session:

**(a) The five metrics do not multiply the runs - they multiply the instrumentation.** One
throughput run can yield throughput, CPU per sample, peak RSS and wire bytes per sample at the same
time. Only latency needs its own run shape. So the matrix is (run shape x QoS x network), not
(metric x QoS x network).

**(b) Screening, not factorial.** One baseline plus one-factor-at-a-time. 48 QoS cells become 7.
This finds *where* TickLE loses, which is what step 6 needs; it does not measure interactions
between QoS policies, and it is not meant to. If a factor turns out to matter, its interactions get
their own follow-up sweep rather than being paid for up front.

**(c) Tiered network coverage.** The baseline gets every network condition; the QoS variations get
three. A variation that shows something interesting earns a full sweep afterwards.

## 3. Run shapes

| id | shape | yields |
|---|---|---|
| **T** | one-directional stream, fixed window | throughput (Mbps, msg/s), net loss, refused writes, CPU per delivered sample, peak RSS, wire bytes and packets per delivered sample |
| **L** | ping-pong at a low rate | RTT min/avg/max and a high percentile, CPU per sample, peak RSS, wire bytes per sample |

Both shapes exist already (`reliable_throughput` / `best_effort_throughput`, `reliable_latency` /
`best_effort_latency`). What they need is the instrumentation in §6.

## 4. QoS cells

Baseline **Q0**, then one factor changed at a time. Every cell runs in all three frameworks with the
same request.

| id | reliability | history | durability | payload | rate | what it isolates |
|---|---|---|---|---|---|---|
| **Q0** | RELIABLE | KEEP_LAST 64 | VOLATILE | 76 B | max | baseline |
| Q1 | BEST_EFFORT | KEEP_LAST 64 | VOLATILE | 76 B | max | the cost of reliability |
| Q2 | RELIABLE | **KEEP_ALL** | VOLATILE | 76 B | max | back-pressure instead of eviction |
| Q3 | RELIABLE | **KEEP_LAST 1024** | VOLATILE | 76 B | max | retention depth |
| Q4 | RELIABLE | KEEP_LAST 64 | **TRANSIENT_LOCAL** | 76 B | max | durability's retention cost |
| Q5 | RELIABLE | KEEP_LAST 64 | VOLATILE | **1442 B** | max | large messages |
| Q6 | RELIABLE | KEEP_LAST 64 | VOLATILE | 76 B | **paced** | behaviour below saturation |

Latency shape uses a smaller set, since history and durability do not bear on an unloaded RTT:
**L0** RELIABLE 76 B, **L1** BEST_EFFORT 76 B, **L2** RELIABLE 1442 B, **L3** BEST_EFFORT 1442 B.

**Both payload sizes are required, and this is the one non-trivial harness addition.** Network bytes
per sample is dominated by framing, which only shows at 76 B; throughput is dominated by payload,
which only shows at 1442 B. A single size would mislead on one metric or the other. The `Bench`
shape is fixed at 76 B in all three harnesses today, so a second shape has to be added to all three
identically.

## 5. Network conditions

Loss is the only dimension the current sweeps use. That is a blind spot: three of these conditions
exercise paths loss never reaches.

| id | `tc` | why it is here |
|---|---|---|
| **N0** | none | baseline |
| N1 | `loss 1%` | mild |
| N2 | `loss 5%` | moderate |
| N3 | `loss 20%` | severe |
| N4 | `loss 50%` | the published extreme; kept for continuity with §3b |
| N5 | `delay 10ms jitter 2ms` | makes the retransmit round trip the binding cost rather than the link. Exposes anything tuned against a sub-millisecond RTT - and every TickLE retry interval was |
| N6 | `delay 1ms reorder 5%` | reordering, which **RELIABLE's strict in-order delivery now has to absorb** (`0b415269`, `d63860c7`). No existing sweep tests it, and it is the newest code in the reliable path |
| N7 | `rate 10mbit burst 32kbit latency 50ms` | **the actual 10Base-T1S target rate.** Every figure published so far was taken on a ~100 Mbit link, i.e. ten times the speed of the hardware TickLE is for |

N6 and N7 are the two most likely to find something, and neither has ever been run.

## 6. Instrumentation to add, and the fairness rule

**Every metric below goes into a header shared by all three harnesses, as `CpuPlace.h` already is,
and is reported in the same `RESULT:` fields with the same units.** Instrumenting TickLE alone, or
better, is how a comparison quietly becomes a claim about the instrument. The CPU-pinning finding
(`9f70d9b4`) is the precedent: a fix applied to one harness would have handed TickLE a 15%
advantage a quarter of the time.

| metric | how | reported as |
|---|---|---|
| CPU | `getrusage(RUSAGE_SELF)` utime+stime at start and end, both processes | `cpu_s_per_Msample`, `cpu_s_per_MB`, and the raw `utime_s`/`stime_s` |
| memory | `VmHWM` from `/proc/self/status` at exit, plus `VmRSS` sampled | `peak_rss_kb` |
| network | `/proc/net/dev` byte and packet deltas on the test interface, both hosts | `wire_bytes_per_sample`, `wire_packets_per_sample`, `wire_bytes_total` |

**CPU must be normalised per delivered sample, not reported as a percentage.** The three frameworks
run at different rates by up to 3x, so CPU% compares three different workloads. Per-sample and
per-megabyte are the comparable forms, and both are needed: per-sample favours large payloads,
per-megabyte favours small ones.

`/proc/net/dev` rather than a capture, for the bulk figure: it costs nothing, needs no privileges,
and counts what actually left the interface. `tickle_pcap_count.py` stays for the one-off question
of *what* those bytes were (DATA against ACKNACK against discovery), which is the diagnostic step 6
will want but not something to run 387 times.

## 7. Coverage and the time budget

| tier | cells | network | combinations |
|---|---|---|---|
| 1 | T/Q0 and L/L0 | N0-N7, all eight | 16 |
| 2 | T/Q1-Q6 and L/L1-L3 | N0, N2, N7 | 27 |

**43 combinations x 3 frameworks x 3 repetitions = 387 runs.** At 25-40 s a run that is **2.7 to 4.3
hours** of rig time, holding the hil lock throughout. It splits cleanly at the tier boundary if one
session is too long.

Repetitions are 3 because that is what every published figure used, and because the spread across
three is what `COMPARISON.MD` reports rather than a mean alone. Frameworks are interleaved within
each repetition, so a drift during the session lands on all three rather than on whichever ran last.

## 8. How the results will be read, written before running

- **A cell is void** if its leftover guard fired, if it produced no `RESULT:` line, or if the
  framework did not end `drained=acked` where that is expected. Void cells are reported as void,
  never as a zero.
- **Comparisons are within one session only.** The rig carries day-to-day offsets of ~13 us on
  latency across all implementations at once (`COMPARISON.MD` §5), so a figure from this campaign is
  compared to another figure from this campaign, and to the published ones only as "moved / did not
  move".
- **TickLE wins a cell** when it is better than *both* vendors on that metric, outside the spread of
  the three repetitions. Inside the spread is a draw, not a win.
- **Where TickLE does not win, that cell is an optimisation target** and needs a named hypothesis
  before any code changes - the §3b lesson, where 108.5 to 84 Mbps was read as the price of ordering
  and was actually a linear scan.
- **The goal may not be reachable everywhere, and saying so is part of the result.** A known risk:
  under Q2 (KEEP_ALL) TickLE preallocates its cache to the resource limit while CycloneDDS allocates
  as samples arrive, so TickLE may legitimately lose on memory there. If it does, the answer is
  either the lazy growth that now exists (`81c8186c`) or documenting the trade - not pretending the
  cell was a draw.

## 9. Sequence and split

1. **This draft is approved or changed** by the user.
2. **Instrumentation first** (§6), by TickLE Dev, in shared headers across all three harnesses, with
   the second payload shape. Verified before any sweep: a run whose CPU or byte counters read zero
   is an instrument failure, and the harness must say so rather than report a zero.
3. **Sweep script** extending `comparison_resweep.sh`, by TickLE Plan: the tier structure, the `tc`
   conditions including the three new ones, the leftover guard, and timestamped output.
4. **Baseline pass on the rig** (§7), by TickLE Plan, holding the hil lock.
5. **COMPARISON.MD gains a section per metric**, with the raw output committed under
   `examples/perf_hil/results/` as the existing sweeps do.
6. **Optimisation targets** from §8, each with a hypothesis and a pre-registered reading.
7. **Optimise, then re-run the affected cells** - and the unaffected ones as the control, because a
   change that helps one cell and quietly costs another is the failure mode this whole document
   exists to avoid.

## 10. What the user needs to decide

1. **The time budget.** 387 runs is 2.7-4.3 hours of exclusive rig time. Approve as one session, or
   split at the tier boundary?
2. **The two new network conditions.** N6 (reorder) and N7 (10 Mbit rate cap) have never been run.
   N7 in particular may change the story, since every published figure was taken at roughly ten
   times the target link's speed.
3. **The second payload shape.** It is the one real harness addition, and both sizes are needed for
   the network and throughput metrics to mean anything. Approve, or accept measuring those two
   metrics at one size?
4. **Scope of the screening design.** One-factor-at-a-time finds where TickLE loses but does not
   measure QoS interactions. Accept, or is a specific combination worth the full cross?

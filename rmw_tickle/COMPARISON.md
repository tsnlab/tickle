# TickLE vs. FastDDS vs. CycloneDDS: performance comparison

**Read section 1 first.** It is one table holding every experiment in this document, with the best
value in each row marked ✅ and the worst ❌. Section 2 explains each row: what was measured, what
the number means, and where it is weaker than it looks. Section 3 is the standing to-do list.
Section 4 is the design philosophy and the measurement rules - deliberately last, because they
explain *why* the table is shaped the way it is, which is only interesting once the table has been
read.

Column order in every table is fixed: **TickLE, FastDDS, CycloneDDS**.

## 1. The one table

Every experiment in this document, all three frameworks side by side. **✅ = best in that row,
❌ = worst in that row.** Lower is better for latency, CPU, memory and bandwidth; higher is better
for throughput. Each row's explanation is in section 2 below, linked from its own name.

**⚪ means a row is deliberately not scored** (rows 5, 45, 46, 47). That is not
missing data. It is this document's central rule: a comparison that cannot be made fairly is worth
more unmade than made wrongly. §4.3 gives the conditions that void a cell and the real result each
one caught.

**Every framework here runs with the same QoS, set explicitly and checked on every row.** This follows
the user's instruction of 2026-09-26. How that is done, the one FastDDS transport parameter tuned for
a fair evaluation, and what was different before are all in §4.4. Read it before quoting a margin.

**Where the numbers come from (the `meas.` column).** Within a row all three frameworks come from the
same session, so a row is always comparable. Across rows of different letters it is not.
- **`A` rows** come from the aligned 12-cell campaign of 2026-09-26, TickLE core `43a6a52c`. It is
  the current core: release build, DATA_FRAG with a seq_no per datagram, `sendmmsg`, reorder slots
  equal to the window. All three frameworks got the same explicit QoS: the KEEP_ALL bound in samples,
  `max_blocking_time` 100 ms, and KEEP_LAST 64 where that is the cell. Every row asserted it.
  **Totals: WIN 98, DRAW/TIE 10, LOSE 0, VOID 0.** The ties are `loss_pct` reading 0 everywhere.
- **`T` rows** are P3/P4 cells re-run the same way (`fd586d3e`, identical core), with **FastDDS's
  transport `maxMessageSize` set to 1472**, the user's decision for a fair evaluation. This makes
  FastDDS fragment in RTPS instead of handing the kernel oversized datagrams. **Totals: WIN 27,
  DRAW/TIE 3, LOSE 0, and every framework delivered everything**, including at P4 under 5% loss.
- **`R` rows** are the cross-host `rmw` measurement of 2026-09-26 evening (`rmw_crosshost_rtt.sh`, build
  `75e39a61`, `results/rmw_scored_2026-09-26.txt`, 72 rows, 0 void). It is the current core and rmw_tickle,
  including the fix for a peer-registration race that had made rmw_tickle's ping broadcast. Each of
  `rmw_tickle`, `rmw_fastrtps_cpp` and `rmw_cyclonedds_cpp` is driven through `rclcpp` by the same ping/pong
  nodes, on the eth0 link only, 3 repetitions interleaved, 100 round trips each, median of 3. Every row's
  pong was checked to have mapped the expected `librmw_*.so`.
  - **Both ways an application can wait are test cases** (the user's decision of 2026-09-26). In *block*
    the ping waits in `spin_once()` and the rmw wakes it when the reply arrives: the `spin()` pattern of
    most ROS 2 applications, and the native harness's way. In *poll* it loops on `spin_some()` with a
    100 us sleep between spins.
  - Memory is measured from outside every process alike: the ping through `/usr/bin/time`, the pong
    through `/proc/PID/status` VmHWM.
  - CPU is the pong's thread run time summed from `schedstat` in nanoseconds over its whole life (4 s idle,
    then 100 round trips): a per-process total, not a cost per message.
  - These rows replace the earlier `M` and `H` rows, which were poll wait only, on builds before the race
    fix.
- **`S` rows** are the 2026-09-24/25 QoS-mechanics sweep. Neither campaign covered them.

Raw rows and verdicts: `examples/perf_hil/results/campaign_aligned_2026-09-26*` and
`campaign_tuned_fastdds_2026-09-26*`.

| # | Metric | Condition | TickLE | FastDDS | CycloneDDS | meas. |
|---|---|---|---|---|---|---|
| | **[Latency](#21-latency)** (ms) | | | | | |
| 1 | RTT mean | P1 76 B | ✅ **0.207** | ❌ 0.290 | 0.259 | A |
| 2 | RTT max (tail) | P1 76 B | ✅ **0.279** | 0.618 | ❌ 0.725 | A |
| 3 | RTT mean | P2 1292 B | ✅ **0.231** | 0.318 | ❌ 0.378 | A |
| 4 | RTT max (tail) | P2 1292 B | ✅ **0.332** | 0.645 | ❌ 10.8 | A |
| 5 | RTT mean | +10 ms netem | ⚪ 10.2 | ⚪ 10.3 | ⚪ 10.4 | A |
| 6 | RTT max (tail) | +10 ms netem | ✅ **12.2** | 12.3 | ❌ 22.4 | A |
| | **[Throughput](#22-throughput)** (Mbps) | | | | | |
| 7 | RELIABLE | P1 76 B | ✅ **116** | ❌ 39.2 | 92.9 | A |
| 8 | RELIABLE | P2 1292 B | ✅ **932** | ❌ 650 | 855 | A |
| 9 | RELIABLE | P3 1424 B | ✅ **938** | ❌ 436 | 803 | T |
| 10 | RELIABLE | P4 2800 B | ✅ **940** | ❌ 614 | 852 | T |
| 11 | BEST_EFFORT | P1, max rate | ✅ **121** | ❌ 47.6 | 71.2 | A |
| 12 | RELIABLE KEEP_LAST 64 | P1 | ✅ **118** | ❌ 42.2 | 89.7 | A |
| 13 | RELIABLE | P1, **5% loss** | ✅ **108** | 12.0 | ❌ 2.8 | A |
| 14 | RELIABLE | P4, **5% loss** | ✅ **859** | 213 | ❌ 14.0 | T |
| 15 | RELIABLE | P1, 5% reorder | ✅ **106** | ❌ 32.6 | 68.2 | A |
| 16 | retention under loss | P1: 5% loss / unshaped | ✅ **92.6%** | 30.6% | ❌ 3.1% | A |
| 17 | retention under loss | P4: 5% loss / unshaped | ✅ **91.4%** | 34.7% | ❌ 1.6% | T |
| | **[CPU](#23-cpu)** (cpu_s / Msample) | | | | | |
| 18 | throughput, client | P1 76 B | ✅ **5.2** | ❌ 19.6 | 6.9 | A |
| 19 | throughput, server | P1 76 B | ✅ **3.0** | ❌ 19.5 | 10.9 | A |
| 20 | throughput, client | P2 1292 B | ✅ **6.4** | ❌ 19.2 | 8.5 | A |
| 21 | throughput, server | P2 1292 B | ✅ **4.1** | ❌ 19.9 | 14.4 | A |
| 22 | throughput, client | P4 2800 B | ✅ **12.2** | ❌ 40.0 | 14.1 | T |
| 23 | throughput, server | P4 2800 B | ✅ **8.8** | ❌ 33.9 | 19.5 | T |
| 24 | latency | P1 76 B | ✅ **155** | ❌ 385 | 229 | A |
| 25 | latency | P2 1292 B | ✅ **154** | ❌ 389 | 236 | A |
| 26 | throughput, client | P1, 5% loss | ✅ **5.7** | ❌ 27.8 | 13.0 | A |
| 27 | throughput, client | P4, 5% loss | ✅ **14.4** | ❌ 64.6 | 41.1 | T |
| | **[Memory](#24-memory)** (peak RSS, KB) | | | | | |
| 28 | throughput, client | P1 76 B | ✅ **1,896** | ❌ 15,884 | 5,256 | A |
| 29 | throughput, client | P2 1292 B | ✅ **2,184** | ❌ 15,048 | 6,272 | A |
| 30 | throughput, client | P4 2800 B | ✅ **2,240** | ❌ 13,972 | 5,544 | T |
| 31 | latency, client | P1 76 B | ✅ **1,652** | ❌ 14,340 | 4,836 | A |
| 32 | throughput, server | P1 76 B | ✅ **1,772** | ❌ 14,344 | 4,884 | A |
| 33 | throughput, server | P4 2800 B | ✅ **2,792** | ❌ 13,996 | 4,912 | T |
| 34 | throughput, server | P4, 5% loss | ✅ **2,796** | ❌ 14,336 | 6,272 | T |
| | **[Bandwidth](#25-bandwidth)** (wire B / sample) | | | | | |
| 35 | wire bytes | P1 76 B | ✅ **146** | ❌ 286 | 180 | A |
| 36 | wire bytes | P2 1292 B | ✅ **1,363** | ❌ 1,502 | 1,397 | A |
| 37 | wire bytes | P3 1424 B | ✅ **1,495** | ❌ 1,865 | 1,585 | T |
| 38 | wire bytes | P4 2800 B | ✅ **2,934** | ❌ 3,461 | 2,950 | T |
| 39 | wire bytes | P1, 5% loss | ✅ **151** | ❌ 290 | 194 | A |
| 40 | wire bytes | P4, 5% loss | ✅ **3,066** | ❌ 4,356 | 3,203 | T |
| 41 | framing overhead | single datagram | ✅ **70.1** | ❌ 210.3 | 104.2 | A |
| | **[QoS mechanics](#26-qos-mechanics)** | | | | | |
| 42 | DURABILITY late join | durable / volatile | 20/20, 0/20 | 20/20 | 20/20 | S |
| 43 | HISTORY, within depth | burst | 160/160 | 160/160 | 160/160 | S |
| 44 | HISTORY, beyond depth | burst | ✅ **147.7/160** | ❌ 109/160 | ❌ 109/160 | S |
| 45 | DEADLINE detection | 50 ms | ⚪ works | ⚪ works | ⚪ works | S |
| 46 | LIVELINESS detection | lease 2.0 s | ⚪ not comparable | ⚪ 1999 ms | ⚪ 2000 ms | S |
| 47 | LIFESPAN expiry | 100 ms | ⚪ works | ⚪ works | ⚪ works | S |
| | **[rmw layer](#27-the-rmw-layer)** (ms, cross-host) | | | | | |
| 48 | RTT mean, block wait, BEST_EFFORT | Bench (64 B) | ✅ **0.252** | ❌ 0.316 | 0.270 | R |
| 49 | RTT mean, block wait, RELIABLE | Bench (64 B) | ✅ **0.250** | ❌ 0.331 | 0.269 | R |
| 50 | RTT mean, block wait, BEST_EFFORT | Array1k | ✅ **0.272** | ❌ 0.342 | 0.289 | R |
| 51 | RTT mean, block wait, RELIABLE | Array1k | ✅ **0.273** | ❌ 0.349 | 0.282 | R |
| 52 | RTT mean, poll wait (100 us sleep), BEST_EFFORT | Bench (64 B) | ❌ 0.505 | 0.469 | ✅ **0.428** | R |
| 53 | RTT mean, poll wait (100 us sleep), RELIABLE | Bench (64 B) | ❌ 0.504 | 0.498 | ✅ **0.433** | R |
| 54 | RTT mean, poll wait (100 us sleep), BEST_EFFORT | Array1k | ❌ 0.509 | ❌ 0.509 | ✅ **0.449** | R |
| 55 | RTT mean, poll wait (100 us sleep), RELIABLE | Array1k | 0.510 | ❌ 0.517 | ✅ **0.437** | R |
| 56 | peak RSS, ping process (KB) | Bench, block, BEST_EFFORT | ✅ **11,264** | ❌ 23,884 | 14,844 | R |
| 57 | peak RSS, pong process (KB) | Bench, block, BEST_EFFORT | ✅ **11,760** | ❌ 23,624 | 14,532 | R |
| 58 | pong CPU, whole run (ms) | Bench, block, BEST_EFFORT | ✅ **35.3** | ❌ 56.6 | 42.9 | R |
| 59 | pong CPU, whole run (ms) | Bench, block, RELIABLE | ✅ **34.9** | ❌ 60.8 | 48.3 | R |

**FastDDS as shipped (`maxMessageSize` 65,500), at the same cells, for reference.** Aligned campaign,
same QoS:
- P3: 533 Mbps and 1,677 B/sample.
- P4: 786 Mbps, 3,101 B/sample and
  14,864 KB.
- P4 under 5% loss: 9.9 Mbps, and it **did not deliver everything within
  the drain cap** (3,252 of 3,439), because the kernel's IP reassembly fails under loss (§4.4 item 4).

The tuned profile trades some of FastDDS's lossless P3/P4 throughput and bytes for completing under
loss. Both are shown so neither configuration's weakness is hidden.

### Where TickLE does not come first, stated up front

- **Rows 52-55, the `rmw` round trip with a polling wait: CycloneDDS is faster, by 0.06-0.08 ms.** TickLE is
  fastest in the block rows (48-51), by 0.009-0.019 ms. The poll result is a grid effect, measured and not
  argued (`RMW_PERF_PLAN.md` 8.1). `rmw_tickle`'s `spin_some()` costs 6.4 us against CycloneDDS's 46.5,
  so its loop cycles faster, and against a 100 us sleep its reply is caught on the third iteration instead
  of the second (3.01 against 2.03 per round trip). A single sleep value is arbitrary, so a sweep of
  0/50/100/200 us is being measured; these rows will then show the whole sweep rather than one point of it.
  At the kernel boundary, rmw_tickle is fastest in every user-space segment. It is behind by more than
  5 us only in the pong's kernel receive (ppoll then recvfrom, against the vendors' blocking recvmsg),
  which is the next thing being worked on.
- **Row 5** is a draw by construction, not a win. A 10 ms injected delay swamps a 0.2 ms round
  trip, so that cell measures `netem`. The next row, the tail, still separates the three.

Everything else is a TickLE win: every scored metric at P1, P2, P3 and P4, with and without loss, with
identical QoS, against FastDDS both as shipped and tuned. **Throughput retention under 5% loss is
92.6% at P1 and 91.4% at P4 for TickLE.** It was 6.6% at P4 in the 2026-09-25 campaign; what
closed that gap is DATA_FRAG with a seq_no per datagram (`DATAFRAG_PLAN.md` section 14).

## 2. Each experiment explained

One subsection per group of rows in the table above. **The figures quoted inside these subsections are
the ones measured when each finding was made, and many predate the current build. Section 1 has
the current figures.** What these subsections still carry is the explanation: what was measured,
why it came out that way, and where a number is weaker than it looks.

### 2.1 Latency

Table rows 1-6. **The release-build re-measurement (2026-09-26, rows 1-4 of the master table)
supersedes the P1/P2 figures below**: TickLE reads 0.209 / 0.231 ms mean and 0.324 / 0.357 ms tail
against CycloneDDS's 0.360 / 0.277 and FastDDS's 0.287 / 0.319. The campaign figures are kept
because the `+10 ms netem` cell has not been re-run at release, and because the analysis of the
tail and the governor below applies unchanged.

`rtt_avg_ms` / `rtt_max_ms`, median of 3, 100 round trips each.

| cell | condition | TickLE | CycloneDDS | FastDDS |
|---|---|---|---|---|
| c10 | P1, no shaping, mean | **0.204** | 0.358 | 0.282 |
| c10 | P1, no shaping, **max** | **0.440** | 10.9 | 0.605 |
| c11 | P2, no shaping, mean | **0.233** | 0.296 | 0.334 |
| c11 | P2, no shaping, max | **0.659** | 1.29 | 0.711 |
| c12 | P1, +10 ms delay, mean | 10.3 | 10.4 | **10.3** |
| c12 | P1, +10 ms delay, max | **12.2** | 21.1 | 12.3 |

**Every absolute latency figure in this section was measured with the `ondemand` CPU governor, and
that costs about 10% (2026-09-25).** Holding the package clock at its 2,400 MHz ceiling instead of
letting it fall to 1,500 between messages makes a round trip **19-21 us faster on this rig, about
10% of a 207 us round trip** - measured on two builds of TickLE, each 15 runs, by pinning a
`nice 19` spinner to a core the harness does not use (`cpufreq/policy0 related_cpus = "0 1 2 3"`, so
one core lifts the package). Both Raspberry Pi 5s run `ondemand` with a 1.5-2.4 GHz range and report
`cpuidle current_driver=none`, so this is frequency scaling and not C-states.

**The comparison stays fair; the absolute numbers move.** All three frameworks were measured on the
same two hosts under the same governor in the same sessions, so nothing about the ordering or the
margins between them depends on this. What it means is that a reader should not take 0.204 ms as
"TickLE's RTT" - it is TickLE's RTT on a host that lets an idle core down-clock, and the same code
on a pinned-clock host would read roughly a tenth lower. The same applies to both vendors' figures.
That is a property of every latency measurement on a DVFS host, not a flaw in this one, but leaving
it implicit would let the numbers read as protocol properties.

c12's mean is a three-way draw by construction: a 10 ms injected delay dominates a 0.2 ms RTT, so
that cell measures `netem`. The tail still separates - CycloneDDS's worst round trip is 21.1 ms
against TickLE's 12.2, and at c10 CycloneDDS shows a 10.9 ms spike against TickLE's 0.44.

**The cost of that latency is the campaign's other real loss.** TickLE's `stime_s` in the latency
cells is 0.630, 0.631 and 0.622 s - across an 18x payload difference *and* a 50x RTT difference. It
is neither per-byte work nor time spent waiting for the pong; ~0.63 s over a ~5 s run is 12.6% of
one core burned whatever the traffic does, which is what a fixed-rate poll looks like. A tight poll
is presumably what buys the RTT, so the open question is whether the same RTT survives a poll
interval chosen for the target platform, not whether to give up the latency.


**The 30x CPU cost in the heading this section used to carry is gone, and the mechanism named above
is why.** "A tight poll is presumably what buys the RTT" was the hypothesis; it was wrong in a
useful direction. The poll was not buying anything - it woke on a fixed 100 us timer whether or not
there was work, so `ppoll` was called 38,505 times per latency run. Waiting for the next scheduler
entry instead took that to **38 calls** with the RTT unchanged (`experiments/poll_fix_verify.sh`).
Combined with the `-O0` build fix, latency-cell CPU went from 33x CycloneDDS's to 34% below it
(master table rows 24-25). The full argument is in §2.3.
### 2.2 Throughput

Table rows 7-17. The two deep dives that follow (§2.2a, §2.2b) are where the RELIABLE-under-loss numbers come from.

`send_mbps`, client, median of 3.

| cell | condition | TickLE | CycloneDDS | FastDDS |
|---|---|---|---|---|
| c1 | P1, no shaping | **106** | 93.3 | 36.6 |
| c3 | P3 | **938** | 812 | 502 |
| c4 | P4 | **948** | 861 | 831 |
| c5 | P1, **5% loss** | **95.8** | 3.39 | 9.31 |
| c7 | P1, reorder 5% | **73.9** | 67.9 | 31.4 |
| c8 | P1, BEST_EFFORT | **119** | 71.7 | 47.4 |

c5 is the largest single result in the campaign. Under 5% packet loss TickLE keeps **90.6%** of its
unshaped throughput; CycloneDDS keeps 3.5% and FastDDS 26.4%. This is RELIABLE + KEEP_ALL for all
three, verified from the harnesses rather than assumed, with every sample delivered on all three
sides (`sent == recv`).

### 2.2a Scenario 4 in detail: RELIABLE under injected loss (re-measured 2026-09-23)

Scenario 4's own single-number rows above were replaced by this section. Two reasons, both real
bugs found on 2026-09-22/23 (see §3 items 12-13): every earlier TickLE number was measured while
RELIABLE's own retry path was partly inert or flooding, and **every earlier FastDDS number was
measured over a path `tc` never touched at all** - FastDDS was sending each sample on both the
`192.168.10.0/24` test link *and* the `10.1.1.0/24` management WiFi.

The three frameworks are also not answering the same question at their default settings, so a
single "loss %" cell hides the real difference:

- **TickLE** (today, KEEP_LAST-equivalent, `publish()` never blocks): loss shows up as **lost
  samples**.
- **FastDDS/CycloneDDS** (KEEP_ALL + `resource_limits(4000)`): loss shows up as **a slower sender**,
  and - once `max_blocking_time` is short - as **refused writes** (`write_fail`, added 2026-09-23).
  Neither ever loses an accepted sample.

So each cell reports **loss % / refused % / Mbps**. `-B` is `max_blocking_time` (DDS only).
3 reps for TickLE, 2 for the DDS pair.

**Max send rate** - re-measured 2026-09-24/25 in one session at `659013e9`, 3 reps per cell and all six
columns interleaved within each loss level (`comparison_resweep.sh`, part B). Each cell reads
**net loss / refused / Mbps**. Net loss counts samples the middleware accepted and failed to deliver;
refused counts writes the DDS pair declined with `-B 1` (1 ms `max_blocking_time`). A range is
min-max over the reps. Where a column did not end `drained=acked` in every rep, it is marked, and
there `Mbps` counts acceptance, not delivery.

| `tc` loss | TickLE, depth=64 | TickLE, depth=1024 | FastDDS (default `-B`) | FastDDS (`-B 1ms`) | CycloneDDS (default `-B`) | CycloneDDS (`-B 1ms`) |
|---|---|---|---|---|---|---|
| 0% | 0% / - / 113 | 0% / - / 113 | 0% / 0% / 34.6-37.9 | 0% / 0% / 35.1-37.6 | 0% / 0% / 89.2 | 0% / 0-0.00025% / 92.9 |
| 1% | 0.0099% / - / 111 | 0-0.00014% / - / 110 | 0% / 0% / 10.6 | 0% / 2.8% / 10.5 | 0% / 0% / 11.6-12.8 | 0% / 0.67-0.76% / 18.4 |
| 5% | 0.24-0.27% / - / 106 | 0.028-0.044% / - / 107 | 0% / 0-0.042% / 5.29-10.3 | 0% / 3.4-18% / 2.39-9.59 | 0-0.045% / 0% / 2.81-2.98 | 0% / 13-21% / 2.12-3.33 |
| 20% | 3.8-4.4% / - / 96.3 | 2.7% / - / 95.2 (one rep timed out draining) | 0% / 0.09-0.4% / 0.93-3.03 (acked/timeout) | 0% / 11-18% / 1.77-2.88 (timeout) | 0% / 0% / 0.32-0.44 | 0% / 58-69% / 0.26-0.43 |
| 50% | 24% / - / 91.8 (timeout) | 19% / - / 87.8 (timeout) | 0% / 1.4-1.9% / 0.23-0.30 (timeout) | 0% / 0.57-58% / 0.31-53.1 (timeout) | 0% / 0% / 0.08-0.12 | 0% / 57-82% / 0.13-0.32 (acked/timeout) |

Against the previous table (2026-09-23), TickLE's column moved in the expected direction and by
little. depth=1024 at 1% is 0-0.00014% (was 0%), and at 5% it is 0.028-0.044% (was 0.016%). The
20% and 50% rows are new. They show where TickLE's KEEP_LAST default stops keeping up with loss: it
keeps its rate (88-96 Mbps) and pays in loss (2.7-24%). The DDS pair keeps 0 net loss by slowing to
0.08-3 Mbps. §2.2b is the like-for-like comparison under KEEP_ALL.

**Paced** - re-measured 2026-09-25 in one session at `9235ee21` (`paced_resweep.sh`, 3 reps per cell,
interleaved). All three clients get `-i 0.000075`, which was meant as ~8 Mbps. The rig's timers
deliver about 7-8k msg/s, so the actual rate is 4.0-4.7 Mbps at 0% for all three. That is lower
than the label, but the same for everyone, which is what makes the table comparable. Cells read
**net loss / refused / Mbps**, with TickLE at its default (KEEP_LAST, depth 64) and the DDS pair at
their default `max_blocking_time`:

| `tc` loss | TickLE (depth=64) | FastDDS | CycloneDDS |
|---|---|---|---|
| 0% | 0% / - / 4.72 | 0% / 0% / 4.06 | 0% / 0% / 4.41 |
| 1% | 0% / - / 4.73 | 0% / 0% / 4.04 | 0% / 0% / 4.09-4.32 |
| 5% | 0% / - / 4.73 | 0% / 0% / 4.03 | 0% / 0% / 1.55-1.89 |
| 20% | 0.14-0.17% / - / 4.93 | 0% / 0% / 4.07 | 0% / 0% / 0.21-0.25 |
| 50% | 6.2% / - / 5.07 (one rep timed out draining) | 0% / 0.32-0.54% / 0.62-0.93 (timeout) | 0% / 0% / 0.08-0.27 |

At a rate all three can sustain, TickLE and FastDDS lose nothing through 5% and keep their rate.
CycloneDDS already slows to about 40% of its rate at 5% injected loss. At 20% only FastDDS holds
both zero loss and its rate. TickLE keeps its rate and loses 0.14-0.17%, because it is on KEEP_LAST;
under KEEP_ALL (§2.2b) it loses nothing.

**Reading**:

- **At a rate all three sustain (4-5 Mbps paced), TickLE loses nothing through 5% injected loss**,
  the same as FastDDS and at a slightly higher rate. At 20% it loses 0.14-0.17% on its KEEP_LAST
  default.
- **At max rate TickLE is 10-100x faster than either DDS** (113-121 Mbps vs. 0.1-17 Mbps under
  loss) and pays for it with a small residual loss (0.04% at depth=1024, 5% injected). That is the
  real trade-off today: TickLE keeps sending, DDS slows down or refuses.
- **DDS's "0% loss" is a different guarantee, not a better recovery rate.** With a 1ms
  `max_blocking_time` the same conditions produce 3-79% refused writes, i.e. the data never
  entered the middleware at all.
- **Updated 2026-09-23 (Phase 2, `1b15e15`)**: with the Subscriber's tracking window widened to
  1024 samples (and per-Subscriber ack identity on the wire, `tt_VERSION` 6), TickLE's own
  depth=1024 column above is now **0% loss at 1% injected loss (6/6 runs)** and 0.016% at 5% -
  i.e. at ~113-117 Mbps, two orders of magnitude above either DDS vendor's rate under the same
  injected loss. The window must stay ≤ the Publisher's cache depth; beyond that it measurably
  hurts (PLAN.md's own Phase 2 entry).
- Closing TickLE's remaining gap is `rmw_tickle/PLAN.md`'s Phase 3 (RELIABLE + KEEP_ALL: block the
  writer, fail the write on timeout), which gives the same guarantee DDS offers, plus Phase 2 if
  KEEP_LAST at max rate must also reach zero.

### 2.2b The same guarantee, compared: RELIABLE + KEEP_ALL (2026-09-23, final)

§2.2a compared TickLE's default (KEEP_LAST-equivalent, `publish()` never blocks) against the DDS
pair's KEEP_ALL, which is not the same promise. TickLE now implements the DDS promise too
(`rmw_tickle/PLAN.md` Phase 3): a KEEP_ALL Publisher refuses a write rather than evicting an
unacknowledged sample, and `rmw_publish()` blocks up to `max_blocking_time` before returning
`RMW_RET_TIMEOUT`. This section compares all three **under that same contract**, measured the same
way: `tc netem` on the sender's own egress, 3 reps, and every writer waits for acknowledgements at
teardown (`dds_wait_for_acks()` / `wait_for_acknowledgments()` / the TickLE harness's own
drain-until-acked), so no framework's tail loss depends on drain luck.

**Net loss** = lost - refused writes, i.e. samples the middleware accepted and then failed to
deliver. All three are **0 in every cell**, so the only thing that differs is what it costs:

| `tc` loss | TickLE (KEEP_ALL, strict order) ‡ | FastDDS | CycloneDDS |
|---|---:|---:|---:|
| 0% | **108.2 Mbps** | 36.2 (median) | 93.3 |
| 1% | **103.8** | 10.9 (median) | 12.1 |
| 5% | **97.1** | 9.6 (median) | 2.4 |
| 20% | **54.7** | 1.1 (median) † | 0.42 |
| 50% | **21.8** | 0.48 (median) † | 0.14 |

**Re-measured 2026-09-24/25 at `659013e9`** (`keepall_three_way.sh`, run as part D of
`comparison_resweep.sh`), 3 reps per cell, `-d 5`, interleaved by framework. That build is later than
the `d63860c7` measurement below by the watermark drains, the piggybacked heartbeat, the oversize-flush
fix, multi-part discovery and the create-default fix. TickLE's column did not move: 108.2 / 103.8 /
97.1 / 54.7 / 21.8 against 108.8 / 104.4 / 98.1 / 55.0 / 22.0, within 1.3% everywhere. Every TickLE rep
ended `drained=acked` with `write_fail=0`. **The ◊ outlier below did not recur.** All three 50% reps
read 21.6-21.9. FastDDS again timed out draining at 20% and 50% in most reps, including one 50% rep
that read 70.7 Mbps of accepted but undelivered writes, so its medians are marked as before.

The previous measurement, kept because the ordering-cost discussion below refers to it:

| `tc` loss (2026-09-24, `d63860c7`) | TickLE (KEEP_ALL, strict order) ‡ | FastDDS | CycloneDDS |
|---|---:|---:|---:|
| 0% | **108.8 Mbps** | 35.9 | 93.2 |
| 1% | **104.4** | 10.6 (median) | 13.0 |
| 5% | **98.1** | 6.8 (median) | 2.5 |
| 20% | **55.0** | 2.9 (median) † | 0.42 |
| 50% | **22.0** ◊ | 0.68 (median) † | 0.13 |

**Measured in one session on 2026-09-24 at `d63860c7`, both Pis confirmed at that commit by reporting
their own HEAD** - 3 reps per cell, `-d 5`, interleaved by framework within each loss level. This is
the build that ships: RELIABLE delivers each writer's samples in strictly increasing order, through a
reorder buffer that costs O(1) per sample. TickLE's cells are the mean of three reps except where
marked; every TickLE rep `drained=acked` with `write_fail=0` except the one noted under ◊.

**◊** One of three 50% reps read 8.4 Mbps with 28 refused writes (still `drained=acked`, so not a
stall). It did not recur: a follow-up of **8 consecutive 50% reps at the same build all read
21.8-22.2**, with `write_fail=0`, `out_of_order_discarded=0`, `timestamp_not_newer=0` and
`reorder_held_peak=255` in every one. The cell reports 22.0, the median of the sweep's three and
consistent with all eight; the outlier is unexplained and is recorded rather than averaged in.

**† and the FastDDS medians.** FastDDS varied far more than either other column in this session - at
20% its three reps read 2.85, 1.41 and 38.77, at 50% 38.33, 0.68 and 0.56, and every rep that read
above ~15 at those levels ended `drained=timeout`. `send_mbps` counts writes the middleware
*accepted*, so a writer that never drained reports work it did not deliver; a mean over those reps
would describe acceptance, not throughput. Medians are reported where the reps disagree, and the
20% and 50% FastDDS cells should be read as "did not keep up" rather than as a rate.

### Ordering costs nothing, and the number that said otherwise was an implementation defect

TickLE's column is within 0.7% of its own pre-ordering figures (108.5 / 104.1 / 97.5 / 54.7 / 21.9,
same rig, same method, earlier the same day) at every loss level. It took five builds to get there,
and the sequence is the useful part:

| build | what changed | 0% | 1% | 5% | 20% | 50% |
|---|---|---:|---:|---:|---:|---:|
| `5816aab3` | no ordering | 108.5 | 104.1 | 97.5 | 54.7 | 21.9 |
| `e461e7f3` | ordering, reorder buffer scanned linearly, 512 slots | 84.6 | 58.2 | 44.8 | 32.8 | 18.4 |
| `a5b99b1b` | same scan, buffer sized to the widest window, 4096 slots | 10.0 | 8.1 | 7.1 | 6.8 | 6.1 |
| `0b415269` | buffer indexed by sequence number, O(1) per sample | 108.6 | 104.2 | 97.9 | 54.6 | 21.8 |
| `d63860c7` | + strict order (discard below the delivered high-water mark) | 108.8 | 104.4 | 98.1 | 55.0 | 22.0 |

Every hold and every drain used to walk every slot, and the drain re-walked them for each sample it
released - so per-sample cost scaled with the buffer's *capacity*, not with what it held. On a clean
link nothing is ever held, yet every sample paid for a full scan of an empty buffer: that is the whole
0% drop. Growing the buffer 8x to make overflow impossible cost 8.5x in throughput, which is how the
cause was found. **The 108.5 -> 84 drop was nearly published here as the price of the ordering
guarantee.** It was the price of a loop.

The same scan also manufactured loss. A subscriber spending its time scanning fell behind its socket
and the kernel dropped datagrams the network had delivered - so `lost=` and `out_of_order` counts
from `a5b99b1b` and earlier describe the subscriber's own backlog as much as the transport, and none
of them are used here.

**One earlier figure is still unexplained and is now superseded rather than resolved.** TickLE's 20%
and 50% cells previously read 89.3 and 56.5 against today's 54.7 and 21.9. Four candidate causes
were each excluded by measurement rather than by argument: the same-host socket work (ABBA A/B, 4
blocks of 5 reps, arms agreeing at 55.0); `835e8b44`'s added logging ("Publisher peer registered"
fires **once** in a 20%-loss run); run duration (`-d 5` gives 54.5 and `-d 10` gives 54.9); and the
code across the whole window, with the harness held constant and only `src/`+`include/` moved -
`acaa8049` (the KEEP_ALL-solicitation fix the published matrix was taken immediately after),
`b332dc1b`, `7bb87702`, `0be8c33f` and HEAD all measure 54.5-56.1. **The old 89.3 does not reproduce
at its own commit.** What remains is the original measurement's own conditions, which were not
recorded - `PLAN.md` cited that fix as `a437859`, a sha rebased away before it was ever pushed, so <!-- doc-shas-ignore: this line names the broken sha as its subject, so the checker must not read it as a citation -->
not even the commit was pinned down. The conclusion recorded here is that the cause was not found,
not that it was ruled out; the table above supersedes those cells because it was taken under a
controlled, internally comparable method, which they were not.

**Reading, recomputed from the table above rather than carried over** (every figure below is
arithmetic on the `d63860c7` table, redone when that table replaced the one before it - the previous
version of this paragraph was once left computing from a superseded table, see below). On a clean
link TickLE's advantage is modest - **3.0x** FastDDS and only **1.2x** CycloneDDS at 0% - and the
whole story is what happens under loss. At 1% it is 9.8x and 8.0x; at 5% 14.4x and **39x**; at 20%
19x and **131x**; at 50% 32x and **169x**. The FastDDS ratios at 20% and 50% are against a writer
that did not keep up (see †), so they overstate the comparison with a draining writer and are best
read as "FastDDS could not sustain this at all". TickLE keeps **20%** of its lossless rate at 50%
injected loss; CycloneDDS keeps 0.14%. Every figure comes from one interleaved session at one build.

*(The previous version of this paragraph said 10-45x, 160x, 510x and "~51% of its lossless rate".
Those were arithmetic on the superseded rows and were carried over when the table was replaced,
because a derived number does not look stale the way a citation does - the doc-sha gate checks that
every commit id resolves and nothing checks that a ratio still follows from the table above it.
Found on review, not by any tool.)*

**What it costs TickLE**, re-measured against its own KEEP_LAST mode in one interleaved session
(2026-09-24, `5816aab3`, 3 reps per cell, modes alternated within each loss level):

*(Measured before in-order delivery existed. It is still the cost of the KEEP_ALL guarantee: the
five-build table above shows the KEEP_ALL column at `d63860c7` within 0.7% of its `5816aab3` values
at every loss level, so ordering moved neither side of this comparison. Re-measure both columns
together before the next change to either mode.)*

| `tc` loss | KEEP_ALL | KEEP_LAST | cost of the guarantee |
|---|---:|---:|---:|
| 0% | 108.6 | 115.1 | **-5.7%** |
| 1% | 103.8 | 110.9 | **-6.3%** |
| 5% | 97.0 | 106.6 | **-9.0%** |
| 20% | 55.0 | 96.4 | **-43.0%** |
| 50% | 21.8 | 92.6 | **-76.4%** |

**This paragraph previously read -11% at 20% and -33% at 50%, and those figures were not merely
stale - they were a comparison between measurement days.** The KEEP_ALL side came from the
2026-09-24 re-measurement and the KEEP_LAST side (99.8 and 84.4) from the earlier session, in the
one paragraph that quantifies the headline trade-off, and in a document whose §2.7 exists to say that
cross-day absolutes cannot be compared. Recomputing the percentages from the new KEEP_ALL numbers
alone would have left that intact while making it look corrected, so KEEP_LAST was re-measured
instead. **The guarantee costs several times what this document used to claim**: two to three times
more at 20% and 50% loss.

**`write_fail` was 0 in all 15 KEEP_ALL runs**, so none of that cost is refused writes - every
sample was accepted and the loss of throughput is back-pressure alone, the Publisher waiting for
acknowledgements rather than turning work away. And as a check on reproducibility rather than an
extra claim: this session's KEEP_ALL column (108.6 / 103.8 / 97.0 / 55.0 / 21.8) reproduces the
three-way session's (108.5 / 104.1 / 97.5 / 54.7 / 21.9) to within 0.6% at every level, measured
hours apart on the same rig.

Paced to ~8 Mbps the two modes are identical (4.90 vs. 4.93, 5.04 vs. 5.04): with that much
headroom back-pressure never binds. *(Those two pairs are from the earlier session and are a
within-session comparison there, so they stand; they are marked here rather than silently mixed
with the table above.)*

**Two honest caveats, both measured rather than asserted:**

1. **TickLE's pre-match window.** A Subscriber starts tracking at the first DATA that *arrives*, so
   samples published before that are unknown to it rather than lost - nothing requests them and
   every counter on both sides correctly reads zero. Measured size: **0 on a healthy link and at
   20% injected loss (6/6 reps), 0-5 per run at 50%**. Counted from seq 1 those show up as loss;
   counted from the Subscriber's first observed seq they do not, and this document reports both
   rather than picking the flattering one. In every KEEP_ALL run the raw count equalled the
   pre-match window *exactly*, rep by rep. DDS has no equivalent window because its matching is
   symmetric and its writer waits for it; closing TickLE's would mean establishing the baseline at
   match time, a core change the user deliberately deferred (PLAN.md). **Update 2026-09-23**: this
   applies to a VOLATILE Subscriber, which is not owed pre-match history. With **TRANSIENT_LOCAL**
   the window is gone entirely - measured 15/15 reps at tc 0/1/5/20/50%, `first_seq=1` every time,
   after `261f39b8` taught the DATA-first contact path to respect requested durability the same way
   the Heartbeat path already did.
   *(This is what the `‡` on the TickLE column marks: post-match loss is 0; the pre-match window is
   reported separately rather than folded into it.)*
   Note for anyone re-running this: FastDDS has **no local compile path on the dev box** (no
   fastcdr headers), unlike CycloneDDS - its harness first compiles on the Pis, which is how a
   `ReturnCode_t` API difference in FastDDS 2.x got past a clean local check once. A clean local
   build says nothing about the FastDDS side.

2. **FastDDS at 20-50% did not always drain**: `drained=timeout` in 1/3 reps at 20% and 3/3 at 50%,
   i.e. its 3s teardown wait expired with samples still unacknowledged. Its net loss is still 0 (a
   refused write is counted, not silently dropped), but its numbers at those levels describe a
   writer that never caught up rather than one that finished cleanly. CycloneDDS and TickLE
   reported `acked` in every cell.

### 2.3 CPU

`cpu_s_per_Msample`, median of 3.

| cell | condition | TickLE client | CycloneDDS | FastDDS | TickLE server | CycloneDDS | FastDDS |
|---|---|---|---|---|---|---|---|
| c1 | P1, no shaping | **5.75** | 6.85 | 20.8 | **3.75** | 10.9 | 20.3 |
| c3 | P3 | **7.43** | 12.3 | 26.3 | **5.26** | 16.2 | 21.7 |
| c4 | P4 | **12.5** | 14.2 | 30.4 | **8.35** | 19.5 | 23.3 |
| c5 | P1, 5% loss | **6.17** | 12.9 | 37.3 | **4.36** | 15.1 | 48.9 |
| c7 | P1, reorder 5% | **6.91** | 9.23 | 24.0 | **3.73** | 12.3 | 20.5 |
| c8 | P1, BEST_EFFORT | **5.24** | 14.1 | 13.5 | **3.40** | 12.8 | 21.4 |

The latency cells invert it completely: TickLE 6,509-6,523 against CycloneDDS's 195-213. See the
latency section - it is the same finding from the other side.


**Both halves of that heading have since changed, and the second one was a defect rather than a
trade.** Two causes, found separately overnight on 2026-09-25/26:

1. **A fixed-rate poll.** `tt_Node_poll()` woke every `tt_RECEIVE_TIMEOUT` (100 us) whether or not
   anything was due, so a latency run that exchanges 100 messages made 38,505 `ppoll` calls. It now
   sleeps until the next scheduler entry: **38 calls**, same RTT. `node_flush` was rescheduling
   itself every 1 ms unconditionally for the same reason and is now demand-driven on the same grid,
   which preserves the 0.5 ms mean batching latency.
2. **The core was built `-O0`.** `platform/linux/Makefile` defaults to `BUILD_TYPE=debug`, and the
   harness had never overridden it, so every TickLE figure published before 2026-09-26 measured an
   unoptimised build. The harness now defaults to `release` and every RESULT line carries
   `core_build=` so this cannot recur silently.

Neither is a tuning choice and neither trades latency for CPU, which is what the old heading
implied. Master table rows 18-27 are the current figures: TickLE now wins the latency CPU cells it
used to lose 33x, by 34%.
### 2.4 Memory

`peak_rss_kb`, median of 3 repetitions.

| cell | condition | TickLE | CycloneDDS | FastDDS |
|---|---|---|---|---|
| c1 | P1, no shaping | **1,924** | 5,244 | 17,668 |
| c3 | P3 (1440 B) | **4,656** | 6,216 | 23,008 |
| c4 | P4 (2800 B) | 7,384 | **5,608** | 28,412 |
| c5 | P1, 5% loss | **1,924** | 5,024 | 17,756 |
| c7 | P1, reorder 5% | **1,924** | 5,308 | 16,864 |
| c8 | P1, BEST_EFFORT | **1,668** | 4,976 | 14,212 |

Server side, TickLE wins every cell including P4: 1,720-2,248 KB against CycloneDDS's 4,880-7,400
and FastDDS's 14,340-16,428.

**c4 is the one real memory loss, and it is understood.** TickLE's resident memory is
`C + reliable_depth x record_bytes`, with C measured at 1,720/1,727/1,727/1,733 KB across the four
payload shapes - a 13 KB spread against a 5,464 KB range, so the retention window is the whole
story. At P4 the touched arena is 5,651 KB of a 7,384 KB peak. The default `reliable_depth` is 2048
*samples* regardless of sample size, so the arena grows linearly with the payload; a fixed byte
budget with depth derived from it would win the large sizes without touching the small ones. The
trade is not free, since a narrower window is where KEEP_ALL begins dropping unacked samples on the
byte bound.

**A qualifier that has to travel with every memory number here.** These are *resident* pages on a
demand-paged Linux host. TickLE's harness declares far more static storage than it touches: at P4,
22.6 MB on the client and 11.4 MB on the server, against 7.4 and 1.7 measured. The server figure is
the clean demonstration - its declared reorder window varies 24x across the payload shapes while its
measured RSS goes 1,792 / 1,752 / 1,720 KB, flat and slightly *down*. On a target without demand
paging the declared figure is the real one. This is the harness's `MAX_RELIABLE_DEPTH` choice rather
than a libtickle property, so it does not invalidate the comparison, but "TickLE uses the least
memory" means "resident, on Linux, at these depths".

### 2.5 Bandwidth

`wire_bytes_per_sample`, client, median of 3. Measured from `/proc/net/dev` rather than from any
middleware's own statistics, so it counts retransmissions and control traffic.

| cell | condition | sample | TickLE | CycloneDDS | FastDDS |
|---|---|---|---|---|---|
| c1 | P1, no shaping | 76 B | **147** | 180 | 286 |
| c3 | P3 | 1440 B | **1,511** | 1,588 | 1,692 |
| c4 | P4 | 2800 B | **2,914** | 2,950 | 3,099 |
| c5 | P1, 5% loss | 76 B | **150** | 193 | 327 |
| c7 | P1, reorder 5% | 76 B | **147** | 181 | 286 |
| c8 | P1, BEST_EFFORT | 76 B | **146** | 178 | 254 |

The per-framework framing this implies, measured rather than assumed: **TickLE 70.6 B, CycloneDDS
104.1 B, FastDDS 209.6 B** per single-datagram sample, each including the 42 B of Ethernet+IP+UDP.
TickLE's 70.6 - 42 = 28.6 B matches `DESIGN.md`'s stated 28 B DATA framing. FastDDS's 167.6 B above
L4 is 2-3x the RTPS specification arithmetic and is most of why it loses this metric. Adding ~42 B
per extra IP fragment, `sample + base framing <= 1514` predicts **all twelve observed packet counts
with zero mismatches**, which is what makes this a model rather than an extrapolation.

### 2.6 QoS mechanics

Table rows 42-47, from the nine-scenario matrix.

**Notation** (unified 2026-09-21, the user's own explicit request - every cell in this section
uses one of these fixed templates so results are directly, visually comparable at a glance,
replacing this document's own earlier mix of ranges/min-max/single-value styles):

- **Latency**: `avg±stdev ms, X% loss`
- **Throughput**: `sent avg±stdev msgs, recv avg±stdev msgs, X% loss, sent Y Mbps, recv Z Mbps, loss (Y-Z) Mbps`
- **Count-based** (DURABILITY/HISTORY/LIFESPAN - no sustained rate to report a Mbps for): the same
  `msgs`/`loss%` half of the Throughput template, without the Mbps fields.
- `avg±stdev` is computed from every individually-recorded run for that cell (sample stdev, `n-1`;
  for exactly 2 runs this reduces to `stdev = |a-b|/√2`). **`†` marks a value with no computable
  stdev** - either a single, unrepeated observation, or (scenarios 1/2/8's own DDS-vendor cells,
  §2.7's own table) a run where only the tool's own already-summarized min/avg/max was recorded at
  the time, not the raw per-sample data a real stdev needs - shown as `avg` alone, not invented.
  `loss %`/`Mbps` fields are plain means (not `±stdev`) even where the underlying msgs count has
  one, matching this section's own literal template; `loss Mbps` is the derived `sent - recv`
  aggregate, not an independently re-measured quantity.

**★ = re-measured 2026-09-24/25 in one session, all three frameworks interleaved by rep, 3 reps each**
(`examples/perf_hil/experiments/comparison_resweep.sh` at `659013e9`, which is the head of main at the
time; the latency rows come from `latency_recheck.sh` at `5bed1a89`, after the harness fix below). These
cells are comparable to each other and not to the older cells in this table, because the rig carries
day-to-day offsets (§2.7). Rows without ★ were re-run in the same session and gave the same result as
published: durability (TickLE durable 20/20 in 3/3, volatile 0 in 3/3, and both DDS the same), deadline
(TickLE writer_misses 3, reader_misses 29-30), and TickLE liveliness (lease 1.0/2.0/4.0 s gave 1152/2103/3270
ms mean). Their older cells stand.

Two corrections came out of this pass. **The latency rows had never been measured at the same rate for
TickLE and the DDS pair.** The DDS harness defaults to `-i 0.1` and TickLE's to `-i 1`, and the old cells
used each harness's own default. All three now get explicit `-i 0.1 -d 10`. **TickLE's two latency
servers also lacked the +15 s lifetime buffer** that every other scenario's server has
(`0f536579`), so at 10 Hz they exited halfway through the client's pings. The first ★ run read "53%
loss", and that figure was the harness, not TickLE. **The same happened to lifespan.** `lifespan_expiry`'s server sleeps its
`-p` pause *before* creating its Subscriber, so it needs the client started at the same moment
(`PRE_CLIENT_SLEEP=0`), and both callers passed 3 s. With 3 s, the Subscriber existed before the first
sample, and "lost 0" was the correct answer to a different question. Plan first published this as a
LIFESPAN regression bisected to `261f39b8`. TickLE Dev showed it was the stagger: on loopback, each
commit with stagger 0 loses 48 at pause 1.0, and with stagger 3 loses 0, at HEAD, at `fa3265f0` and
before. The bisect's "good" point was an artifact too. Fixed in `a5f41b46`. Re-measured after the fix: TickLE lost 21.3 / 54 / 77 at
pause 1.0 / 1.5 / 2.0 (`lifespan_recheck.sh`). The pre-registered expectation was ~45 / 70 / 95, as
CycloneDDS loses. The difference is explained by the harness, not by LIFESPAN. The slope is exactly
the 50 samples/s the client publishes at, so every sample older than its 0.1 s lifespan is dropped.
The offset of ~18 samples is the ~0.36 s the TickLE client starts after its server, one SSH
connection later. The DDS twins start their writers from a match wait, so they have no such
offset. The spread at pause 1.0 (8-29) is that SSH latency varying. The 2026-09-20 figures
(28 / 43 / 79) agree with these, so they were valid LIFESPAN measurements taken before the 3 s
stagger was introduced.

| # | Scenario | Condition | TickLE (native) | FastDDS | CycloneDDS |
|---|---|---|---|---|---|
| 1 | `best_effort_latency` | `-i 0.1 -d 10` (100 pings) ★ | 0.20±0.00ms, 0% loss | 0.27±0.00ms, 0% loss | 0.25±0.00ms, 1.1% loss |
| 2 | `reliable_latency` | `-i 0.1 -d 10` (100 pings) ★ | 0.20±0.00ms, 0% loss | 0.29±0.00ms, 0% loss | 0.29±0.06ms, 0% loss (one rep's max RTT 10.8ms) |
| 3 | `best_effort_throughput` | max rate, 8s, no `tc` loss ★ | sent 1549.1K±7.9K msgs, recv 1549.1K±7.9K msgs, 0% loss, sent 117.7Mbps, recv 117.7Mbps, loss 0Mbps | sent 605.0K±10.1K msgs, recv 470.8K±80.5K msgs, 22.2% loss, sent 48.4Mbps, recv 35.8Mbps, loss 12.6Mbps | sent 895.9K±4.9K msgs, recv 894.1K±4.9K msgs, 0.2% loss, sent 71.7Mbps, recv 68.0Mbps, loss 3.7Mbps |
| 4 | `reliable_throughput` | see §2.2a | **superseded - see §2.2a below** | **superseded (measured over a WiFi path, §3 item 12)** | **superseded - see §2.2a** |
| 5 | `durability_late_join` | - | durable: recv 20.0±0/20 msgs, 0% loss; volatile: recv 0.0±0/20 msgs, 100% loss (correctly excludes backlog, matches DDS exactly - see §3) | recv 20/20 msgs, 0% loss | recv 20/20 msgs, 0% loss |
| 6 | `history_depth_burst_loss` | within depth | recv 160/160 msgs, 0% loss | recv 160/160 msgs, 0% loss | recv 160/160 msgs, 0% loss |
| 6 | `history_depth_burst_loss` | beyond depth ★ | recv 147.7±0.6/160 msgs, 7.7% loss (n=3; the earlier n=3 read 150.0±5.2) | recv 109/160 msgs, 31.9% not delivered (deterministic, 3/3) | recv 109/160 msgs, 31.9% not delivered (deterministic, 3/3) |
| 7 | `deadline_miss_detection` | - | sent 198±0 msgs, recv 198±0 msgs, 0% loss, writer_misses=3† (own math), reader_misses=30† (harmless, unexplained) | writer_misses=7†, reader_misses=19†, detect -0.90ms† | writer_misses=7†, reader_misses=14†, detect 0.05ms† |
| 8 | `liveliness_loss_detection` | - | **provisional, and NOT comparable to the DDS columns as measured - see §3 item 14** - lease=1.0s: 1375.7±169.1ms; lease=2.0s: 2060.9±465.7ms; lease=4.0s: 3232.0±235.7ms (real ceiling past ~3s - see §3 item 10). Those ± bands measure **mode-mixing, not precision**: at n=20 the distribution is bimodal, a dominant mode holding to ~15ms plus a minority mode ~460-500ms away | 1999.08ms† (lease=2000ms) | 2000.07ms† (lease=2000ms) |
| 9 | `lifespan_expiry` | within lifespan | 0 lost | 0 lost | 0 lost |
| 9 | `lifespan_expiry` | beyond lifespan, `pause=1.0/1.5/2.0s` ★ | lost 21.3 (8-29) / 54 / 77 of 250 (n=3 each, at `6daf5b98` with `PRE_CLIENT_SLEEP=0`) - LIFESPAN enforced; see the note below for why these run ~18 below the DDS columns | lost 39/64.7/88.7 of 250 (n=3 each) | lost 45/70/95 of 250 (n=3 each, identical) |

Scenarios 3-4's own "recv Mbps" is computed uniformly for all three from `recv_count × 76 bytes × 8 / elapsed_s` (the shared `Bench` wire shape, §4.2) - only CycloneDDS/FastDDS's own `server.c`/`.cpp` print it directly; TickLE's doesn't, so it's derived the same way for all three rather than left blank for one, to keep offered-vs-actually-delivered throughput directly comparable in every cell. "sent Mbps" is each framework's own printed client-side rate. Every condition was reproduced 2/2 (scenario 6 beyond-depth: 3 runs; scenario 9 beyond-lifespan pause=1.5s: 4 runs - see §3 items 6/8) - see the Notation block above for how each cell's `avg±stdev` was computed from those runs.

**Reading**:

- **Latency (1-2)**: TickLE is the fastest of the three, ~0.20-0.22ms average RTT vs. FastDDS's
  ~0.30ms and CycloneDDS's ~0.24ms - and RELIABLE costs nothing extra over BEST_EFFORT for any of
  the three at this rate.
- **Best-effort throughput (3)** (★, 2026-09-25): TickLE sends ~1.55M msg/s (118 Mbps) with 0% loss in all
  three reps, against ~896K (72 Mbps, 0.2% loss) for CycloneDDS and ~605K (48 Mbps, 7-36% loss) for
  FastDDS. That is 1.7x CycloneDDS and 2.6x FastDDS, not the "two orders of magnitude" this bullet used
  to claim, which never matched its own table. TickLE's own figure is 25% above the old 94.5 Mbps,
  mostly because every harness is now pinned away from the NIC interrupt core (`9f70d9b4`). That fix
  moves all three frameworks, which is why the columns are compared only within one session.
- **Reliable throughput under controlled loss (4)**: at identical, real, `tc`/`netem`-injected
  loss, **CycloneDDS and FastDDS both fully recover 100% of it, at both 1% and 5%, every time
  (2/2 each)** - `recv` loss stays at exactly 0% regardless of the injected rate for either DDS
  vendor. FastDDS's own initial result here looked like a real cross-vendor difference (loss
  tracking the injected rate almost exactly, not recovered at all) - turned out to be a real
  harness bug instead, not a vendor difference: `examples/perf_hil/fastdds/reliable_throughput`
  had never received the same `KEEP_ALL`+`resource_limits(4000)` fix the CycloneDDS twin got early
  in this exercise, left at a shallow `depth=8` that couldn't retain a lost sample long enough for
  its own retry to land. Fixed and re-verified 4/4 clean - see §3, item 7 for the full story.
  **TickLE's own RELIABLE, unlike either DDS vendor, still mostly doesn't recover it**, and is
  noisier about it (2.1% avg observed at 1% injected; a consistent ~5.25% avg at 5% injected) -
  plausibly explained by TickLE's own extremely high throughput at this rate: `depth=64`
  (`tt_MAX_RELIABLE_HISTORY`, TickLE's own hard cap at the time of this measurement) represents on
  the order of tens of microseconds of retention at ~1.2M msg/s, almost certainly shorter than one
  real ACKNACK-retry round trip on this link - a lost sample is likely evicted from the cache long
  before a retransmit request for it could ever be serviced. This part **is** a genuine TickLE
  characteristic worth its own re-measurement once the depth ceiling becomes configurable
  (PLAN.md's own DDS semantic-parity backlog, row 2, assigned to TickLE Dev) - though TickLE Dev's
  own design note there flags a separate, deeper structural limit (`tt_WriterProxy.received_bitmap`
  is a fixed 64 bits) that a deeper Publisher-side cache alone may not fix.
- **QoS mechanics (5-9)**: HISTORY depth, LIFESPAN expiry, and DEADLINE/LIVELINESS detection are
  all confirmed working in TickLE, each with the expected numbers where the mechanism is directly
  comparable to DDS's own. Two real, TickLE-specific findings surfaced along the way - see §3.

### 2.7 The rmw layer

Table rows 48-59. `rmw_tickle` vs. `rmw_fastrtps_cpp` vs. `rmw_cyclonedds_cpp`.

**Current status (2026-09-26):**
- With a block wait, TickLE is first on every `rmw` row: RTT, memory and CPU.
- With a poll wait at a 100 us sleep, it is not first (rows 52-55). Why is measured: the loop's grid.
- The earlier ~0.08 ms "deficit" was the ping's polling loop, found when the round trip was split with
  packet captures, a syscall timestamp layer and application stamps on both hosts.

[`RMW_PERF_PLAN.md`](RMW_PERF_PLAN.md) sections 7-8 hold the whole investigation: what was ruled out,
the vendor source comparison, and the per-segment split. The text below is the history of how the
`rmw` comparison was set up, and its figures predate that plan.

**Rows 58-59, CPU:** `rmw_tickle`'s pong uses the least CPU of the three in both reliability modes: 35 ms
against CycloneDDS's 43-48 and FastDDS's 57-61. That agrees with the per-segment split, where every
user-space segment of rmw_tickle is the shortest.

Two separate measurement passes, both reached through the real ROS 2 `rmw` layer
(`RMW_IMPLEMENTATION` + `rclcpp`), not either framework's own native API:

- **Same-host**: `.github/scripts/compare_rmw_perf.sh` (run by hand, deliberately not CI - see §3)
  drives `buildfarm_perf_tests`' own two-process `rmw` benchmark across all three
  `RMW_IMPLEMENTATION`s on one box. FastDDS and CycloneDDS are both forced onto real UDP/IP with
  every same-host shared-memory shortcut explicitly disabled (FastDDS's `useBuiltinTransports=
  false` *and* its separate `data_sharing kind=OFF`; CycloneDDS's `SharedMemory/Enable=false`) -
  without this, both get an unfair, TickLE-incomparable advantage neither has a same-host
  fast-path equivalent for.
- **Cross-host**: `rmw_tickle/rmw_perf_pingpong` (a small purpose-built `rclcpp` ping/pong pair,
  message-shape-compatible with `buildfarm_perf_tests`' own `Array1k`/`Struct16`) run for real on
  the `tickle-hil` rig's own two Pis - the same real physical link §4.2 uses, not same-host loopback.

### Results

One table, both topologies together (`Array1k`=1024-byte array, `Struct16`=16-byte struct, `Bench`
QoS-tagged=the cross-host tool's own ping/pong shape at 50 samples/run, 3 runs/combination). Same
notation as §2.6 (`avg±stdev`, `†` = no computable stdev) - every cell here is `†`: the underlying
tools (`buildfarm_perf_tests`, `rmw_perf_pingpong`) only ever printed their own already-averaged
figure, not the raw per-run samples a real stdev needs, the same limitation as §2.6's DDS-vendor
min/avg/max cells:

| Topology | Message | Mode | TickLE (`rmw_tickle`) | FastDDS | CycloneDDS |
|---|---|---|---:|---:|---:|
| Cross-host | Bench, QoS=best_effort | - | 0.524ms† avg RTT, 0% loss | 0.516ms† avg RTT, 0% loss | 0.440ms† avg RTT, 0% loss |
| Cross-host | Bench, QoS=reliable | - | 0.523ms† avg RTT, 0% loss | 0.523ms† avg RTT, 0% loss | 0.449ms† avg RTT, 0% loss |

**The same-host rows are gone, and this is a policy rather than a gap** (the user's decision,
2026-09-24, translated): *"The PC has all sorts of work running on it, so a performance measurement
taken on the PC is meaningless. The RPi is independent hardware, so only performance results from
there mean anything. Use the PC only to verify functional correctness, not to measure
performance."* The dev box runs whatever else it is running -
two Claude sessions, two CI runners, a Windows VM, an Android emulator - so a latency figure taken
on it measures that machine's mood as much as the middleware. **This document therefore publishes no
performance number measured on the dev box.** The rig's two Raspberry Pis are dedicated hardware and
are where every performance figure in §4.2, §2.6, §2.2a, §2.2b and the rows above comes from.

**This was not an abstract concern; it had already produced a wrong result here.** The same-host
rows were re-measured on 2026-09-24 and read 12-15us slower than the pass they replaced - which
looked like a regression from `82a6a02d`'s socket work. It was not: *every* implementation had moved
by about the same amount between the two days (FastDDS +12.4 to +19.5us, CycloneDDS +11.8 and
+13.3us, `rmw_tickle` +5.7 to +15.1us, mean about **+13us** across all ten comparable cells), and two
of the three contain no TickLE code at all. The offset was the box, and it was roughly a quarter of
the whole figure. Chasing it cost most of a day. The ratio *appeared* to improve at the same time,
from ~1.3-1.6x to ~1.12-1.30x, which is the same artifact read the flattering way - a constant
additive term compresses every ratio toward 1 on its own.

**What is lost, stated plainly:** `buildfarm_perf_tests` only has a same-host shape - its
"two-process" tests launch both sides with `launch_ros.actions.Node`, which forks local children and
has no remote-host path at all (Milestone 14) - so the `rmw`-layer comparison against the two DDS
vendors has no valid venue today. `rmw_perf_pingpong` on the rig is the cross-host replacement and
is what the rows above are, but it covers RTT rather than `buildfarm_perf_tests`' full matrix. The
honest position is that the `rmw`-layer cross-vendor comparison is **narrower than it looks
elsewhere in this document**, not that it is unavailable.

**Reading**: cross-host, `rmw_tickle` and `rmw_fastrtps_cpp` land within noise of each other
(~0.52ms) while `rmw_cyclonedds_cpp` is genuinely faster (~0.44ms) on the real link. RELIABLE costs
nothing measurable over BEST_EFFORT for any of the three. 18/18 runs clean, zero loss every time.

### 2.8 The five-metric campaign (2026-09-25)

The 2026-09-25 campaign, which the F rows of section 1 have since replaced; kept for its method and its findings.

The user's five questions asked together rather than one at a time, across payload sizes, QoS
combinations and `tc`-shaped network conditions. 12 cells x 3 frameworks x 3 repetitions = 108 runs
in one rig session, `instrument=ok` on all 189 RESULT lines. Design in
`examples/perf_hil/OPTIMIZATION_PLAN.md`, harness `experiments/campaign_sweep.sh`, raw output and
computed verdicts in `examples/perf_hil/results/campaign_2026-09-25_b9fad3c1*.txt`.

**Totals: WIN 62, DRAW/TIE 9, LOSE 7, VOID 30.** A WIN means TickLE beat *both* vendors with no
overlap between the three repetitions' ranges; inside the spread is a draw. The verdicts are
computed by `experiments/campaign_summary.py`, not read off by eye.

### What the 2026-09-25 campaign handed forward

- **P2 and P3 are re-sized from the measured framing**: P2 1292 B (`uint8[1280]`), P3 1424 B
  (`uint8[1412]`). P3's usable window is only 33.5 B wide - above CycloneDDS's 1409.9 B limit,
  below TickLE's 1443.4 - so `check_bench_shapes.sh` now gates every shape against every
  framework's measured limit, in both directions, rather than the size being re-derived by eye.
- **Both DDS `reliable_throughput` harnesses take `-K <depth>`**, so the KEEP_LAST cell becomes a
  real cross-vendor comparison instead of a TickLE-only one, and all three now report the history
  they actually ran.
- **All three servers use an idle-based lifetime** instead of an absolute `-d + 15` cap, which is
  what truncated c6.
- **c6 has to be re-run before it can be compared at all.** When it is, `tt_Publisher.retransmitted`
  and `tt_Subscriber.gap_abandoned`/`gap_evicted` give a free cross-check of the reassembly finding
  from inside TickLE: the prediction, written before the run, is `retransmitted` at about 46x `sent`
  and `gap_abandoned = 0`.

## 3. To-do, from TickLE's own perspective

1. **[TickLE core, real correctness bug - fixed]** A liveliness false-positive was found causing
   real duplicate delivery under load: `check_liveliness()`'s own `forget_peers_from_source()`
   wipes a still-alive peer's bookkeeping on a false "presumed dead" timeout, so the next UPDATE
   from that same peer looks like a fresh discovery and re-triggers redelivery. TickLE Dev's
   PLAN.md Milestone 59 (2026-09-21) fixed the **DURABLE push path**
   (`deliver_durability_backlog()`) - scenario 5's own durable case re-verified clean,
   `received=20/20` exactly every time (was 140=7×20 in one run before the fix).
2. **[TickLE core, design decision made - fixed and verified, closes scenarios 5 *and* 6]** TickLE's
   own `RELIABLE + VOLATILE` didn't isolate a late joiner from history the way DDS's own
   RELIABLE+VOLATILE does (scenario 5's volatile control case delivered 57 samples, not the 0 DDS
   gives; scenario 6's own `history_depth_burst_loss` showed the same `recv > sent` shape via a
   late-joining volatile Subscriber against a shallow `depth=8` Publisher - the identical
   structural setup, it turned out). **The user's own explicit decision (2026-09-21): fix it to
   match DDS semantics.**

   Two attempts were needed, both documented honestly in PLAN.md Milestone 60 rather than
   silently replaced: **attempt 1** (keyed on `update_reliable_ack()`'s own DATA-path
   first-contact sync) left scenario 5 completely unchanged - `received=57`, decimal-identical to
   before the fix. Root cause: this scenario's server publishes its whole backlog *before any
   Subscriber exists*, so a fresh Subscriber's first contact is always resolved by the
   discovery-triggered Heartbeat (`inform_subscriber_of_heartbeat()`), not DATA - a separate,
   older first-contact branch attempt 1 never touched. Scenario 6's own `recv > sent` was, at this
   point, *mis*-attributed to a believed-separate receive-side dedup gap (`deliver_data_to_
   subscriber()` had no `seq_no`-based dedup at all, also fixed this same milestone, a real and
   independently-verified-correct feature - just not the actual cause of either open bug).

   **Attempt 2, after real DDS/RTPS analysis at the user's own request** (DURABILITY is a
   Requested-vs-Offered QoS like RELIABILITY/DEADLINE/LIVELINESS - the fix belongs on the
   requesting Subscriber side, keyed on `sub->durable`, mirroring real RTPS's own
   newly-matched-VOLATILE-reader baseline): fixed `inform_subscriber_of_heartbeat()`'s own
   first-contact branch to sync `ack_seq_no` to `first_available_seq_no` only when `sub->durable`,
   otherwise to `last_seq_no + 1`. **Re-verified (2026-09-21): both scenarios fixed.** Scenario 5
   volatile: 3/3, `received=0` exactly, matching DDS precisely; durable case re-confirmed still
   20/20 (2/2), no regression. Scenario 6: re-verified clean too once this landed - `recv > sent`
   gone entirely, replaced by ordinary `recv < sent` (real loss only, 147/156/147 of `sent=160`,
   confirming it was the *same* Heartbeat-driven leak all along, not the dedup gap first guessed).
   One honest open observation, left as-is rather than guessed at further: scenario 6's own real
   loss count varies run to run (4/13/13 lost, vs. CycloneDDS's own deterministic 52) - plausibly
   ordinary timing variance in exactly when late-join matching completes relative to the
   Publisher's own continuous send rate and the `depth=8` ring's own eviction, not yet
   instrumented directly.
3. **[`rmw_tickle`, performance - item 3 now has a real number]** Close the remaining same-host
   `rmw_tickle` latency gap (PLAN.md Milestone 45): item (1), pooling the scratch conversion
   buffers, is done but was a verified *negative result* (no measurable improvement - likely
   already below glibc `tcache` noise at this message size). Item (2), the unconditional
   `tt_Node_interrupt()`+lock pair on every `rmw_publish()` call, is confirmed real but small
   (~3%). **Item (3), the cross-thread wake-up cost between the poll thread and the application's
   executor thread**: `perf sched`/`ftrace` itself needs kernel tracepoint access this box doesn't
   have without an interactive `sudo` password (no TTY available) - measured the underlying
   `pthread_cond_wait`/`broadcast` primitive directly instead, via a standalone benchmark
   reproducing `rmw_tickle.h`'s exact `wait_mutex`/`wait_cond` shape. **Real result, reproduced
   4/4**: avg ≈ 9.4-9.6μs, p50 ≈ 8.6-8.7μs, p99 ≈ 13-16.5μs - closely matching the observed
   same-host gap itself (§2.7's own numbers put the gap at ~9-22μs). Real, converging evidence that
   this mechanism's own inherent cost is the right order of magnitude to explain the whole gap -
   not proof no DDS vendor pays a comparable cost for their own equivalent hand-off (not measured
   this pass), so suggestive rather than closed. Full detail and the suggested next step (a real
   `perf sched latency` capture inside the actual two-process run, once `perf_event_paranoid` is
   lowered) in PLAN.md's own Milestone 45 addendum.
4. **[`rmw_tickle`, re-measurement]** Done (2026-09-21, the user's own go-ahead) - §2.7's own numbers
   above are this fresh baseline, run after Milestones 46-58 landed. No regression from the
   Milestone 44 baseline; the ~1.3-1.6x same-host gap and the cross-host FastDDS-parity/
   CycloneDDS-ahead pattern both persist unchanged.
5. **[`rmw_tickle`, methodology gap - hardware-blocked]** §2.7's own cross-host numbers
   (`rmw_perf_pingpong` on the `tickle-hil` rig) already cover real cross-host `rmw_tickle` vs.
   FastDDS/CycloneDDS, so this item is narrower than it once read: that run is still over the rig's
   own regular Ethernet link, not the real target network medium (10Base-T1S) §4.2-3's own raw-core
   HIL comparison is *also* still measured over (a pre-existing, separately-tracked gap, not new
   here). Genuinely blocked on real 10Base-T1S hardware being available on the rig at all - not
   something reachable by re-running existing tools differently.
6. **[Methodology, found and fixed 2026-09-21]** The original scenario 3/4 numbers (and, it turned
   out, every CycloneDDS/FastDDS scenario where the server is the authoritative side) were measured
   under a real, confirmed bug: `run_scenario.sh` forwards the same `-d` to both the client and the
   server, but it means "the client's own send duration" on one side and "this side's own don't-
   hang-forever cap" on the other - taken literally, the server could stop counting before the
   client had even finished sending. Worse, the CycloneDDS/FastDDS twins' own `run_scenario.sh`
   never actually printed the server's own `RESULT` line at all (only `pkill`ed it) - the original
   "9312 sent, 9311 recv, 0% loss" numbers for scenario 3 predate this discovery and cannot be
   fully trusted as apples-to-apples against a fixed condition. Both bugs are now fixed (`+15s`
   safety-cap buffer on every affected `server.c`/`.cpp`; the server's log is now actually read
   after `pkill`, matching the TickLE-native script's own already-fixed pattern) and scenarios 3-4
   re-measured clean, reproduced 2/2, under the redesigned methodology in §4.2-3 above. Scenario 9's
   own `pause_s` value is a **known**, not-yet-resolved condition mismatch between TickLE (1.0s)
   and the DDS twins (0.3s) - TickLE's own discovery is fast enough on this rig that 0.3s doesn't
   reliably create a stale-enough gap to observe expiry (see the git history for the full
   investigation) - left as a flagged, honest inconsistency rather than silently normalized.
7. **[Harness bug, found and fixed 2026-09-21 - closed]** §2.6's own original reliable-throughput-
   under-loss result looked like a real cross-vendor difference (CycloneDDS's RELIABLE fully
   recovering 5% injected loss, FastDDS's recovering none of it) - it wasn't. Root cause, found
   independently of TickLE Dev's own concurrent HISTORY-depth work (no file overlap):
   `examples/perf_hil/fastdds/reliable_throughput/{client,server}.cpp` had never received the same
   `KEEP_ALL`+`resource_limits(4000)` fix the CycloneDDS twin got earlier in this exercise (see §2.6's
   own methodology note) - left at a shallow `KEEP_LAST(8)` (plus a stale doc comment that still
   said "BEST_EFFORT", a copy-paste leftover never updated for this scenario's own real RELIABLE
   design) that couldn't retain a lost sample long enough for a NACK-driven retry to land, the exact
   same failure mode CycloneDDS's own original client.c doc comment already named. Fixed to match;
   re-verified real HIL, `tc`/`netem` 1%/5% loss, **0% observed loss both times, reproduced 4/4** -
   FastDDS and CycloneDDS now behave identically here, both fully recovering real injected loss.
   No inherent vendor difference exists for this QoS combination after all.
8. **[TickLE core, verified - LIFESPAN follows the expected formula]** PLAN.md's own DDS
   semantic-parity backlog flagged one open gap for LIFESPAN, the policy already closest to real
   DDS: TickLE's own beyond-lifespan loss count had never been checked against a hand-derived
   expected-loss formula the way CycloneDDS's own exact `(pause-lifespan)/interval` match was
   (§2.6, scenario 9). TickLE's own `lifespan_expiry` design can't use that same formula directly -
   `pause_s` is measured from the *server's own process start*, not from a completed match the way
   the DDS twins' own design does (§3, item 6's own "not the same condition" note) - so the exact
   offset from real discovery/match completion time is an unknown constant, not a controlled zero.
   **Worked around by testing the *slope*, not the absolute count**: real HIL, three `pause_s`
   values (1.0/1.5/2.0s, same `lifespan=0.1s`/`interval=0.02s`, 250-sample runs), 2-4 reps each.
   Results: `pause=1.0s` → 27/29 lost (tight); `pause=1.5s` → 54/31/54/33 lost (real, unexplained
   bimodal variance - two runs cluster near 54, two near 32); `pause=2.0s` → 79/79 lost (exactly
   reproducible). **The full-range slope from 1.0s to 2.0s is 51 samples lost per second of
   additional pause** (`(79-28)/1.0`, using each endpoint's own average) - matching the formula's
   own predicted `1/interval_s = 1/0.02 = 50/s` almost exactly (2% off), confirming TickLE's own
   age-based expiry genuinely follows the same linear, interval-driven relationship DDS's own
   exact match demonstrated, independent of not knowing the constant discovery-timing offset. The
   `pause=1.5s` midpoint's own real bimodal variance is left unexplained - plausibly some real,
   discrete timing-alignment effect in discovery/match completion, not investigated further this
   pass (both the 1.0s and 2.0s endpoints were tight/reproducible, only the middle value showed
   this).

9. **[TickLE core, re-measured - confirms Milestone 61's own prediction, and finds one more thing]**
   PLAN.md's own DDS semantic-parity backlog, row 2, predicted that raising a Publisher's own
   `reliable_cache` depth past `tt_RELIABLE_BITMAP_BITS` (64, the Subscriber's own fixed
   `received_bitmap` width) would *not* fix `reliable_throughput`'s own poor tc-loss recovery,
   since the bottleneck is Subscriber-side, not Publisher-side. Re-measured on real HIL with the
   new `-K` flag (`examples/perf_hil/tickle/reliable_throughput/client.c`, Milestone 61), same
   `tc netem` matrix as §2.6/scenario 4's own original depth=64 numbers:
   - `-K 512` (8x the old depth), `tc` loss=1%: sent 1,214,479/1,220,570, recv 1,183,005/1,183,049,
     **2.6%/3.1% loss** (2/2) - inside the original depth=64 range (1.1-3.1%), no improvement.
   - `-K 512`, `tc` loss=5%: sent 1,240,256/1,247,211, recv 1,177,152/1,180,082, **5.1%/5.4% loss**
     (2/2) - matches the original depth=64 range (5.2-5.3%) almost exactly, no improvement.
   - `-K 8192` (the flag's own max, 128x), `tc` loss=5%: sent 1,229,164/1,230,201, recv
     1,136,834/1,136,837, **7.5%/7.6% loss** (2/2, reproducible) - *worse* than both the depth=64
     baseline and `-K 512`, not just flat.

   **Confirms the prediction**: a deeper Publisher cache alone does not improve RELIABLE recovery
   under real loss - the server's own log makes the reason directly visible, not just inferred:
   `[WARNING] Reliable gap too large to track (64 ahead of N) - jumping ahead instead of getting
   stuck` fires regardless of `-K` (seen at both 512 and 8192, "64" never changes), confirming the
   Subscriber-side `received_bitmap`'s fixed 64-bit window - not the Publisher's own cache depth -
   is what actually caps how large a gap RELIABLE can recover from.

   **One more thing, not predicted going in**: pushing depth all the way to 8192 made loss
   measurably *worse* (7.5-7.6% vs. 5.1-5.4% at the same 512 depth and the same 5% injected loss),
   reproduced 2/2. **Root-caused by TickLE Dev directly from the code** (not re-measured/profiled,
   but a high-confidence structural explanation): `process_acknack()` calls
   `find_resendable_cache_entry()` once per set ACKNACK bit, and that function does a *linear scan*
   over `cache->entries[0..depth)` comparing `seq_no` - unlike the write side
   (`cache_reliable_sample()`), which indexes directly via `next % depth`. At depth=64 the scan is
   cheap; at depth=8192, `struct tt_ReliableCacheEntry` carries a 1472-byte buffer, so the full
   `entries[]` array is ~12MB - past any real L2/L3 cache - and an ACKNACK with several bits set
   re-scans that large an array repeatedly, taking a real cache-miss hit nearly every time. Fixable
   (make `find_resendable_cache_entry()` direct-index like the write side, O(1) instead of O(depth))
   but a separate optimization, not done this pass - flagged as a PLAN.md follow-up candidate,
   pending the user's own go-ahead before any core code changes. A very deep Publisher cache is
   still not a sensible default either way - it does not help recovery (per the bottleneck above)
   and, with today's linear-scan read path, actively costs more the deeper it goes. `tc qdisc`
   cleared back to default (`fq_codel`) after all runs.

10. **[TickLE core, re-measured - row 3 (LIVELINESS) fix verified, one honest limit found]**
    PLAN.md's own DDS semantic-parity backlog, row 3, flagged that TickLE's `check_liveliness()`
    only ever ran one fixed, node-level ~3s sweep, ignoring each entity's own announced
    `liveliness_lease_duration_ns` entirely - the user's own explicit go-ahead (2026-09-21) had
    this fixed (Milestone 62: new `tt_Node_entity_alive()`, computed fresh per-entity from its own
    lease; Milestone 63: wired into `check_liveliness()`'s own discovery-departed path too, via
    `tombstone_entities_past_own_lease()`, so the TickLE-native HIL scenario below - which doesn't
    go through `rmw_tickle` at all - actually exercises the new code, not just `rmw_tickle`'s own
    consumer of it). Re-measured on real HIL, `liveliness_loss_detection`, `kill -9` mid-stream,
    2 reps per lease:
    - `-T 1.0` (1.0s lease): detect 1495.3ms / 1256.1ms
    - `-T 2.0` (2.0s lease): detect 2390.2ms / 1731.6ms
    - `-T 4.0` (4.0s lease): detect 3065.3ms / 3398.7ms

    **Confirms the fix for short leases**: at 1.0s/2.0s (both shorter than the old fixed ~3s
    window), detection now tracks the announced lease directly - a dramatic improvement from the
    old fixed ~3080-3620ms regardless of lease, and much closer to CycloneDDS/FastDDS's own
    lease-proportional behavior (§2.6, scenario 8: both DDS vendors detect within ~0.07-1ms of their
    own 2000ms lease).

    **One honest limit, not glossed over**: at 4.0s (a lease *longer* than the old fixed ~3s
    window), detection did **not** scale up to ~4s - it landed at 3065-3399ms instead, right in the
    old fixed-window's own range. Root cause is structural, not a bug: `check_liveliness()` still
    runs its own original node-level sweep (fixed `tt_LIVELINESS_MISS_THRESHOLD *
    tt_NODE_UPDATE_INTERVAL` ≈ 3s) in the same pass as the new per-entity lease check, and whichever
    of the two fires first wins (the discovery-departed callback only fires once, guarded by
    `!departed`). For a lease shorter than ~3s, the new per-entity check fires first, so detection
    tracks the lease. For a lease longer than ~3s, the *old* node-level sweep fires first instead,
    capping real-world detection at ~3-3.6s regardless of how long the announced lease actually is
    - the opposite of true DDS per-entity semantics, where a longer lease means TickLE would keep
    treating the peer as alive for the entity's own full requested duration. In TickLE's favor, this
    means a long-lease Publisher's real death is *never* detected slower than ~3.6s even if it asked
    for a much longer grace period - but it does mean TickLE cannot yet honor a genuinely long lease
    the way real DDS does. Not investigated further this pass (a real behavior, not a bug, and
    likely a reasonable trade-off for embedded targets that want a hard upper bound on detection
    latency) - noted here rather than left undocumented. `COMPARISON.md` §2.6, scenario 8's own TickLE
    column updated with these numbers.

11. **[TickLE core, real HIL - a real methodology correction, and further throughput research]**
    PLAN.md's own Milestone 65 reported the widened (256-bit) ACKNACK bitmap fully recovering
    real `tc`/`netem`-injected loss (0.0% at both 1% and 5%) - a real, correctly-measured result,
    but **from a different tool than this section's own scenario 4** (`perf_client`/`perf_server
    -R` via `run_perf.sh`, the automated `Performance Test` CI workflow's own benchmark - "a
    modest (~500 message) sample at `LOSS_TEST_INTERVAL_SEC`'s own pacing" per that workflow's
    own comment, a real but much lower, paced rate). **Re-measured with the exact scenario 4
    methodology this section uses** (`examples/perf_hil/tickle/reliable_throughput`, real max
    send rate, no pacing) once the bitmap fix landed on `main`: real, reproducible residual loss
    remains at TickLE's own true max throughput - 1.9-4.1% at 1% injected (2 reps), 5.8-8.1% at 5%
    injected (2 reps) - a real improvement over the old 64-bit baseline (was 1.1-3.1%/5.2-5.3%,
    roughly comparable, not dramatically better) but **not** the full recovery the low-rate CI
    benchmark's own 0.0% figure might suggest at a glance. Both numbers are real and correctly
    measured for what each actually tests; the honest summary is that a 256-bit window is enough
    to fully recover loss at a modest, paced rate, but not yet enough at TickLE's own real
    ~150-200K msg/s max throughput - `COMPARISON.md` §2.6's own TickLE column updated with both the
    new post-fix numbers and the old pre-fix ones side by side, not silently replaced.

    **Further hypothesis-driven research, at the user's own explicit instruction to keep
    investigating latency/throughput levers**: a real, previously-undiscovered `reliable_
    throughput/server.c` bug was found and fixed along the way (commit `474e755`) -
    `stream_callback()` had no `seq_no`-based dedup of its own, and core's own `update_reliable_
    ack()` explicitly documents it doesn't catch every duplicate once `jump_ack_baseline()` has
    fired for a writer ("an accepted, narrow miss", that function's own doc comment) - a genuine
    `recv > sent` was reproduced for real at low throughput/high retry ratio before this fix.

    A second, complementary hypothesis (PLAN.md's own "Further latency research") - bounding how
    many scheduler tasks `tt_Node_poll()`'s own inner loop may run consecutively before forcing a
    non-blocking I/O check, so a continuously-rescheduling max-rate Publisher can't starve
    `tt_receive()` for a whole call's own timeout budget - was implemented on a separate branch
    (`experiment/poll-loop-io-interleave-v2`, rebased onto the bitmap-widened `main`, not merged)
    and re-measured with the now-fixed harness, same `tc`/`netem` matrix, 2 reps each: **1% loss**
    - sent 1531776/1304978, recv 1489346/1292070, **2.8%/1.0% loss** (avg 1.9%, vs. the plain-
    `main` baseline's own 3.0% avg above), send_mbps 116.4/99.2; **5% loss** - sent
    1306406/1577933, recv 1240526/1470818, **5.0%/6.8% loss** (avg 5.9%, vs. `main`'s own 6.95%
    avg), send_mbps 99.3/119.9. **A real, now twice-reproduced effect** (this and the original
    pre-bitmap-fix experiment, `experiment/poll-loop-io-interleave`, both showed it independently):
    the poll-loop fix achieves substantially higher real throughput every single run (99-120 Mbps
    vs. plain `main`'s own consistent 92-95 Mbps here) - a robust ~15-25% increase across 8 total
    runs now, cause not yet instrumented/confirmed. Loss% this time was directionally *better*
    with the fix at both conditions (not worse, unlike the first, pre-bitmap-fix experiment, which
    was confounded differently) - a favorable signal, but still only n=2 per condition, not a
    strong statistical claim. **Not merged, not recommended for `main` yet** - a real, reproducible
    throughput effect worth pursuing further (with more reps, and ideally direct instrumentation
    of real `tt_receive()` call frequency to confirm the actual mechanism), but not yet proven
    enough to commit to. See PLAN.md's own experiment write-up for the full numbers and reasoning.

12. **[Methodology, found and fixed 2026-09-23 - every earlier FastDDS under-loss number is void]**
    FastDDS was sending every sample on *both* the `192.168.10.0/24` test link (where `tc netem`
    injects loss) and the `10.1.1.0/24` management WiFi: `tx_bytes` rose ~30.7MB on each interface
    in a 4s run, while CycloneDDS used the test link only. The WiFi copy always arrived, so FastDDS
    reported 0% loss even at **50%** injected loss, and its throughput was partly WiFi-bound. Fixed
    by pinning FastDDS to the test link (`examples/perf_hil/fastdds/fastdds_eth0_only.xml`, a UDPv4
    `interfaceWhiteList` + `useBuiltinTransports=false`, applied via `FASTRTPS_DEFAULT_PROFILES_FILE`
    in that framework's own `run_scenario.sh`, commit `f9cd4c0`) - verified after the fix: eth0
    +43MB, wlan0 +12KB in the same 4s run, and throughput rose 17.0 -> 21.2 Mbps. §2.2a's own FastDDS
    column is the re-measured, eth0-only result; §2.6's own scenario 4 FastDDS cells are superseded.

13. **[TickLE core, three real RELIABLE bugs found and fixed 2026-09-22/23 - §2.6's own scenario 4
    TickLE cells are superseded too]** In order: (a) `struct tt_Publisher.seq_no` was `uint16_t`, so
    the wire counter wrapped every 65536 sends and `update_reliable_ack()`'s early-return path left
    retransmission effectively disabled for most of every run; (b) once that was fixed,
    `maybe_arm_acknack_retry()` was found sending an ACKNACK per *DATA packet* while a gap was open
    (~70K/s, commit `b393764`); (c) `reliable_throughput/server.c` counted loss assuming in-order
    arrival, so genuinely recovered retransmits were still counted lost (`84ef9a6`). Phase 1 of
    PLAN.md's own recovery plan then landed three improvements: 1-a (immediate narrow NACK for a gap
    that opens while a retry is armed), 1-b (RELIABLE-only 1ms retry interval), 1-c (an eviction
    Heartbeat carrying `first_available_seq_no`, replacing a compile-time depth guess). Net effect at
    max rate, 5% injected loss: **4.6% -> 0.24% at depth=64, 0.04% at depth=1024**; at ~8 Mbps, loss
    is now 0. Full per-phase numbers in `rmw_tickle/PLAN.md`.

14. **[Methodology, found 2026-09-23 - two measured distributions are bimodal, so several published
    bands measure mode-mixing rather than precision]** Found by Dev while A/B-ing an unrelated
    liveliness change, and it lands on this document rather than on the code.
    - **Scenario 8 (`liveliness_loss_detection`)**: at n=20 per lease the residual
      (`detect_latency - lease`) is not a spread around a mean, it is two clusters - a dominant one
      holding to about **15ms**, and a minority one (3-4 reps in 20) sitting **~460-500ms** away.
      **Mechanism, found 2026-09-23 after two wrong hypotheses were killed by the same data**: it is
      a *reference-point* artifact in our own harness, not a property of detection. Instrumenting
      `traffic_last_seen - update_last_seen` at the moment the departure fires shows that gap is
      itself bimodal and takes exactly two values, **0 or ~500ms**, nothing between - and every
      minority-mode rep has gap ~500 while every majority-mode rep has gap 0. Our liveliness client
      publishes every 0.5s while the node UPDATE goes out every 1.0s, so the last packet before the
      kill is either the announce itself (gap 0) or a data sample 500ms later (gap 500);
      `detect_latency_ms` is measured from the last *data* sample, so the measurement's own
      reference point is stale by exactly that amount. The ~500ms mode separation IS that staleness.
      Consequence for this table: at n=3 there is roughly a 45% chance of catching a minority-mode
      sample, and one moves the mean ~160ms, so §2.6 scenario 8's ±169/±466/±236 measure mode-mixing,
      not precision - the dominant mode holds to ~15ms.

      **And the cross-vendor comparison in scenario 8 is apples-to-oranges, which more reps cannot
      fix.** Both DDS harnesses also measure from the last received sample - but in DDS the sample
      *is* what refreshes the liveliness lease, so measuring from it and detecting at
      last_sample + lease are anchored to the same event and cancel; that is why FastDDS and
      CycloneDDS land within ~1ms of their lease. In TickLE the lease is refreshed by the node-level
      UPDATE while the measurement is anchored to data - two different clocks - so the phase between
      them appears as spread the DDS figures structurally cannot have. So scenario 8's TickLE column
      is not measuring the same quantity as its DDS columns, and the standing reading that "TickLE's
      liveliness timing is imprecise vs. DDS" is substantially an artifact of that mismatch. Being
      fixed by reporting `announce_age_at_detect_ms` (anchored to the UPDATE that actually governs
      TickLE's lease, i.e. the DDS-comparable number) alongside the existing end-to-end field, and
      by dropping our client to the DDS twins' own 0.1s interval.

      **Answered (2026-09-23, n=90 clean, both fixes in place)**: with the announce-anchored field,
      `detection = min(lease, 3s) + one check interval`, the three leases' offsets agreeing within
      4ms. Two gaps fall out, and they are different in kind: **granularity** (+610ms on this rig, a
      phase between the 1s announce period and the 1s check tick, anywhere in 0-1000ms on another
      startup) is a trade anyone can reason about; the **3s cap** - `tt_LIVELINESS_MISS_THRESHOLD *
      tt_NODE_UPDATE_INTERVAL` firing before a longer entity lease - means a requested 4s lease is
      silently honoured as 3s, with nothing reporting that to the caller. §3 item 10 already
      recorded the ceiling as a known limit; what is new is the exact rule and the silence. Raised
      as a core conformance item, see `PLAN.md`.
    - **`reliable_throughput` rate**: five consecutive reps at tc 0% gave 94.4 / 111.0 / 112.7 /
      94.9 / 111.5 Mbps - two clusters at ~95 and ~112, nothing between, `post_match_lost` 0 in all
      five. **Cause found 2026-09-23, after the CPU-governor hypothesis was measured and refuted**
      (both modes run at the same ~2391 MHz): it is **which core the sender lands on**. Instrumented
      with `sched_getcpu()`, 12 reps at 0% loss, nothing pinned: client on CPU0 → 94.5-94.6 Mbps
      (n=3, `cpu_main_share` 1.00 in every one); client not on CPU0 → 110.8-112.3 Mbps (n=9). No
      overlap. On both Pis `eth0`'s IRQ 108 is handled **entirely on CPU0** (404M/405M interrupts
      there, zero on CPU1-3), so the sender either shares a core with the NIC interrupt handler or
      it does not, and 3-in-12 is the 1-in-4 an unpinned four-core machine gives.
      **This is a property of the rig, not of TickLE** - the DDS columns ran on the same hosts with
      the same IRQ pinning and are subject to the same coin flip, so the cross-framework comparison
      is not biased. But **every single-run throughput figure here has a ~1-in-4 chance of reading
      ~15% low**, including TickLE's own headline numbers. Being fixed by running the benchmark
      under `taskset -c 1-3` so the sender never lands on the interrupt core; the affected Mbps
      cells are re-measured under that fix before this ◊ is lifted. (Measured on TickLE only so
      far; the same instrumentation is being added to the DDS harnesses rather than asserting it
      for them.) §2.2b's TickLE Mbps column is a median of 3 per cell and therefore reports whichever
      cluster that cell sampled; those cells are marked ◊ until re-measured with enough reps to
      report both modes. Loss columns are unaffected.
    Standing consequence for methodology: 3 reps is not enough for either measurement, and any
    future band quoted from them should say its n.

15. **[Design decision, deferred by the user 2026-09-25; reversed 2026-09-26 - DATA_FRAG is now being built, see rmw_tickle/DATAFRAG_PLAN.md]** Large
    messages under `rmw_tickle` are fragmented by the OS IP layer. Both DDS vendors fragment at the
    protocol level (`DATA_FRAG`) and never enter the kernel's reassembly path at all. Under packet
    loss that difference is not marginal: at a 2800-byte sample with 5% per-packet loss,
    **97.4% of the receiving kernel's reassembly attempts fail** and roughly one datagram in 47
    survives.

    | arm | ReasmReqds | ReasmOKs | ReasmFails | delivered | packets/sample |
    |---|---|---|---|---|---|
    | P1 (76 B) + 5% loss, control | 0 | 0 | 0 | 782,171 | 1.005 |
    | P4 (2800 B) + no loss, control | 423,388 | 211,694 | 0 | 211,694 | 2.014 |
    | **P4 + 5% loss** | 1,399,370 | 17,341 | **1,362,922** | 14,565 | 94.626 |

    Both controls matter. Nothing can fragment at 76 bytes and nothing does, so the counter is
    measuring this traffic and not something else on the host; and fragmentation with nothing lost
    is flawless, `ReasmOKs` equalling the delivered sample count exactly. **Neither fragmentation
    alone nor loss alone does anything.** `ReasmFails` counts fragments, so 1,362,922 / 2 = 681,461
    datagrams never reassembled - 46.8 per delivered sample, against 47.3 transmissions per sample
    from the interface counters. The same number: the wire amplification *is* the reassembly
    failure, not a retransmission decision TickLE makes.

    Why 97% rather than the 9.75% that two-fragment arithmetic predicts: an orphaned fragment is
    held for `ipfrag_time` (30 s) against `ipfrag_high_thresh` (4 MB, ~3000 orphans). At ~148,000
    datagrams/s with 5% loss orphaning ~14,400/s, that budget fills in ~0.2 s and stays full, after
    which the kernel drops queues that would have completed. A feedback loop, which is why the
    magnitude is 47x and not 2x. `ipfrag_max_dist` (64) is a second candidate that may dominate -
    the kernel discards an incomplete queue when fragments from one source arrive more than 64 IP
    IDs apart - and separating the two needs a host sysctl change, which was not made.

    Three hypotheses inside TickLE were ruled out first, each with a control, and recording that is
    the point: the Publisher retransmits exactly the samples an ACKNACK names and no others; the
    ACKNACK bitmap is sized to the subscriber's window rather than capped at 256 bits; and a
    far-ahead arrival makes core abandon the gap and jump the watermark rather than re-request
    (`jump_abandoned_seq=0` in every arm, so nothing was abandoned either). All three were looking
    in core for something that was never there.

    **The user's decision (2026-09-25): leave it as it is and keep this on the list.** The options,
    recorded so the next person does not re-derive them: (a) accept it, if the real 10Base-T1S loss
    rate is far below 5%; (b) tune `ipfrag_high_thresh`/`ipfrag_max_dist` on the host, which is not
    controllable on every deployment target; (c) implement protocol-level fragmentation in TickLE,
    which is what the DDS vendors do and is core work of real size. Raw output:
    `examples/perf_hil/results/a4_reassembly_2026-09-25.txt`, harness
    `examples/perf_hil/experiments/a4_reassembly_check.sh`.

16. **[Harness, latent - low priority, never observed to fire]** The FastDDS
    `liveliness_loss_detection` server reads three globals from `main()` that its listener thread
    writes, with no synchronisation: `g_last_recv_ns`, `g_loss_detect_ns` and `g_loss_detected` are
    plain `bool`/`uint64_t`. Inherited from the original and unchanged by the 2026-09-25 clang-tidy
    sweep, which deliberately left it alone because fixing it is a behaviour change rather than a
    lint fix (TickLE Dev).

    **The three harnesses do not have the same shape, and that is the part worth recording**, because
    the fairness rule ("fix it in all three or none") reaches a different conclusion once the shapes
    are known:

    | | the three shared globals | why |
    |---|---|---|
    | **FastDDS** | plain `bool` / `uint64_t` | listener runs on a middleware thread; nothing declares the sharing |
    | **CycloneDDS** | `volatile bool` / `volatile uint64_t` | same threading, but the compiler may not cache the reads |
    | **TickLE** | no shared globals of this kind | callbacks run on the node's own poll thread, so one thread touches them (`DESIGN.md`, "Concurrency") |

    So FastDDS is the only one carrying the raw shape. `volatile` is not a synchronisation primitive
    and does not make CycloneDDS's version correct under the C11 memory model, but it does remove the
    practically observable failure here - a read hoisted out of the polling loop and cached in a
    register, so the loop never sees the flag change. **Fixing FastDDS to match CycloneDDS makes the
    two more alike, not less**, and TickLE needs nothing because its single-threaded contract already
    excludes the race. That inverts the usual reason to defer: the fairness rule argues *for* this
    change rather than against it.

    **The failure mode is narrower than first written, and the loop's own structure is why**
    (TickLE Dev). The polling loop at `server.cpp:147` calls `nanosleep()` on every iteration
    (`:149`), which is an opaque external call, so the compiler cannot keep `g_loss_detected` in a
    register across it. The hoisted-read failure is therefore ruled out by the loop's structure
    independently of `volatile` - which also means `volatile` is buying CycloneDDS less here than the
    table above implies. What remains is the formal C++ data race only: no happens-before between the
    listener thread's store and `main`'s load. On this ARM64 target that can at most delay visibility
    by one poll period; it cannot lose the flag. So the risk is lower than "never observed" alone
    would suggest, and the distinctive signature below is kept in case that reading is wrong rather
    than because it is expected.

    It stays low priority because it has never been observed to fire: this scenario has produced a
    `loss_detected` reading on every run of every session, and the 2026-09-25 identity run measured it
    identical across both arms. Worth doing when something else touches that file.
17. **[TickLE core, robustness - found, measured, not acted on (2026-09-26, Dev)]** A RELIABLE
   Subscriber whose reorder buffer has fewer slots than its tracking window goes into a re-request
   storm under a persistent gap. Samples that overflow the reorder buffer are "requested again",
   and every ACKNACK re-sends them. In Dev's lossy-link simulation (a 64-slot buffer against a
   256-bit window, 20% loss) this reached about 1,800 datagrams per sample, fragmented or not, with
   132,536 overflows; 256 slots gave 2.94. Neither the benchmark server nor rmw_tickle is exposed,
   since both size slots at least as wide as the window, but any integrator who sizes the buffer
   smaller is. Candidate fixes: size-check at subscriber creation, or stop re-requesting samples
   the buffer cannot hold.

18. **[rmw_tickle, correctness - found 2026-09-26 (Dev), fix planned]** `publication_sequence_number`
   wraps to 0 after 65,535 messages. The core subscriber callback passes `seq_no` as `uint16_t`
   (`tickle.h:1356`), and rmw copies it straight into the ROS-visible field (`rmw_subscription.c:137`),
   which breaks `rmw/types.h`'s requirement that it increase per publisher, with the gap equal to the
   number of messages sent in between. It fires on any long-running topic, fragmented or not. The fix
   is the rmw-side contiguous message counter planned in `DATAFRAG_PLAN.md` 13.2.

19. **[TickLE core, robustness - found 2026-09-26 (Dev), not triggered in any measured cell]** The
   dynamic retry-interval estimator can diverge when recoveries include a re-request wait. It times
   each recovery from the *first* request, with no Karn's rule, so a sample whose repair was lost
   feeds a sample that includes the wait for the next request. Longer intervals then mean longer
   waits, which mean longer intervals. In a 10-fragment unit simulation with the re-request deferred,
   delivery fell to 170/200 and the interval grew from 77 us to 1.24 ms. Adding Karn's rule fixes
   delivery but collapses the interval to its floor in a zero-latency simulation. Candidates to test
   before trusting the estimator at high loss: Karn's rule with backoff, or excluding samples whose
   seq was named more than once. See `DATAFRAG_PLAN.md` 16.1.

## 4. Design philosophy and measurement rules

Last, deliberately. Everything above is what was measured; this is why it was measured that way,
and it is the part that decides whether any of the numbers above are worth anything.

### 4.1 What this document is for

This document tracks whether TickLE meets [`rmw_tickle/PLAN.md`](PLAN.md)'s own Project Goals 2 and
4: that TickLE core's own performance, and separately `rmw_tickle`'s own performance as a ROS 2
`rmw` implementation, are competitive with `rmw_fastrtps_cpp`/`rmw_cyclonedds_cpp` (FastDDS/
CycloneDDS). Two genuinely different questions, measured two different ways:

- **Sections 4.1-4.2**: TickLE core itself vs. FastDDS/CycloneDDS themselves, each used directly
  through its own native API (no ROS 2, no `rmw` layer at all) - isolates TickLE's own engine from
  whatever overhead `rmw_tickle`'s own wrapper adds on top.
- **Section 2.7**: `rmw_tickle` vs. `rmw_fastrtps_cpp`/`rmw_cyclonedds_cpp`, all three reached the
  same way a real ROS 2 application would (`RMW_IMPLEMENTATION` + `rclcpp`) - the number that
  actually matters to anyone choosing an `rmw` implementation.


### 4.2 How the core comparison is run

**Where**: `examples/perf_hil/{tickle,cyclonedds,fastdds}/` - one directory per framework, each
scenario built as a plain `client`/`server` pair using that framework's own native C/C++ API
directly (TickLE's own `tickle.h`, CycloneDDS's `dds.h`, FastDDS's own C++ API).

**How**: real hardware-in-the-loop, not same-host - the `tickle-hil` self-hosted CI runner's own
two dedicated Raspberry Pis, connected by a real point-to-point physical link
(`192.168.10.0/24`, `run_perf.sh`'s own `PERF_LINK_BROADCAST`), one Pi running the `client`
(publisher-ish role) and the other the `server` (subscriber-ish role). `examples/perf_hil/tickle/
run_scenario.sh` (and its CycloneDDS/FastDDS twins) orchestrate each run over SSH.

**What**: the same 9-scenario QoS matrix run against all three frameworks, using an identical
76-byte message shape (`seq`, `send_ns`, a 64-byte payload) so results are directly comparable byte
for byte, not just "similarly sized":

| # | Scenario | QoS exercised | What it measures |
|---|---|---|---|
| 1 | `best_effort_latency` | RELIABILITY (BEST_EFFORT) | low-rate ping-pong RTT |
| 2 | `reliable_latency` | RELIABILITY (RELIABLE) | same RTT pattern, RELIABLE |
| 3 | `best_effort_throughput` | RELIABILITY (BEST_EFFORT) | sender at max rate, fixed 8s window, no artificial loss - how much the receiver actually gets, and the resulting loss % |
| 4 | `reliable_throughput` | RELIABILITY (RELIABLE) | same max-rate/8s design, plus real `tc`/`netem`-injected loss on the sender's own egress at 0%, 1%, 5% - does RELIABLE's own retransmission actually recover it |
| 5 | `durability_late_join` | DURABILITY | publish N samples, subscriber joins late; TRANSIENT_LOCAL/DURABLE vs. VOLATILE side by side |
| 6 | `history_depth_burst_loss` | HISTORY | RELIABLE + depth=8 (fixed, matched across all three), inject a stall both within and beyond that depth |
| 7 | `deadline_miss_detection` | DEADLINE | 50ms deadline, one intentionally-missed interval - time-to-detect, false-positive rate |
| 8 | `liveliness_loss_detection` | LIVELINESS | matched lease, publisher process killed (`kill -9`) mid-stream - peer-loss detection latency |
| 9 | `lifespan_expiry` | LIFESPAN | 100ms lifespan, stall injected past that duration - confirms expired samples are not delivered |

Scenarios 3-4 were redesigned and re-measured 2026-09-21 (the user's own explicit request) after
finding real cross-framework condition mismatches in the original pass - see §3, item 6, for what
was wrong and fixed. Every number in §2.6 for scenarios 3-4 is from that redesigned, reproduced (2/2)
pass; scenarios 1-2 and 5-9 keep their original (2026-09-20) results, not re-measured this pass.


### 4.3 What the comparison refuses to compare, and why that is most of the value

Four rules void a cell. Each exists because a version of this comparison without it produced a
number that looked fine and meant nothing.

- **The payload-boundary gate.** At N0 the client's own packets-per-sample must match the datagram
  split the payload was designed for. It fired on **c2**: at 1388 B, FastDDS sends 2.000 datagrams
  per sample where the design says all three fit one. The size had been computed from an assumed
  60-76 B of RTPS framing; FastDDS's measured framing is 209.6 B. That cell is void and the sizes
  are corrected for the next session (see below).
- **Incomplete delivery.** A RELIABLE cell where any framework's server received fewer samples than
  its client sent cannot be compared. It fired on **c6**, and `loss_pct` did not: all three reported
  `loss_pct=0.0` while FastDDS had delivered 1,641 of 4,987 samples (67% missing) and TickLE 13,802
  of 14,002. `loss_pct` measures gaps between the sequence numbers that *arrived*; samples that
  never arrived leave no gap to count, so it reads a clean zero across a 67%-vs-1.4% difference and
  would have been scored a three-way TIE.
  **Changed 2026-09-26 by the user's decision** (translated: "compare the complete vendors with each
  other"), recorded before the first run it applies to was read. An incomplete *vendor* no longer
  voids the cell: it is excluded from that cell's verdicts, because its per-sample figures come from a
  truncated run, and is reported on its own DELIVERY FAILED line. TickLE is then scored against the
  vendors that delivered everything. An incomplete *TickLE* makes every metric of the cell a LOSE.
  The rule is deliberately asymmetric, in the direction that costs TickLE, since applying the vendor
  rule to TickLE would let its own delivery failure pass unscored. It was triggered by c6 (p4 under
  5% loss), where FastDDS delivered 28-29% in two separate runs while TickLE and CycloneDDS delivered
  100%. `campaign_summary.py`'s `delivery_verdict()` carries the rule, and both branches were checked
  against real result files before use.
- **History-policy mismatch.** It fired on **c9**: TickLE ran KEEP_LAST 64 while both DDS harnesses
  were hard-coded KEEP_ALL, with nothing in the output saying so.
- **Absolute CPU seconds are not a verdict metric at all.** Every throughput cell runs for a fixed
  *duration*, so a framework that sends more samples necessarily burns more CPU seconds. At c1 that
  inverts the ranking outright - FastDDS beats TickLE on absolute `stime_s` while doing a third of
  the work in the same five seconds. `utime_s`/`stime_s` are reported and marked not-comparable;
  `cpu_s_per_Msample` and `cpu_s_per_MB` are the normalised forms and are what "less CPU" means
  here. This rule was changed **after** seeing data, which is stated rather than buried: the
  argument does not depend on which way it fell, since a slower TickLE would have been handed a free
  WIN by the same mechanism.


### 4.4 Fairness: what each framework is configured with, and where they differ

Audited on 2026-09-26 at the user's request, from each harness's source and cross-checked against the
settings its own RESULT line reports. The principle has three parts:
- **The DDS QoS policies are set to the same values for all three frameworks.**
- **Everything that is not a QoS policy is left at each product's shipped default.** For the
  vendors that means heartbeat periods, transport limits and so on. For TickLE it means the defaults
  `rmw_tickle` ships.
- **The environment is identical.**

What follows is what the code actually does, including the places where it falls short of that
principle.

**QoS per scenario, as set in the harnesses:**

| Scenario (cells) | QoS | TickLE | CycloneDDS | FastDDS |
|---|---|---|---|---|
| `reliable_throughput` (c1-c7) | reliability / history / durability | RELIABLE / KEEP_ALL / VOLATILE | same | same |
| | KEEP_ALL bound | 512 KiB of history, capped at 2,048 samples (so 2,048 at p1, about 180 at p4) | `resource_limits` 4,000 samples, plus the library's own ~500 kB write-history watermark | `resource_limits` 4,000 samples, no byte bound |
| | `max_blocking_time` | 100 ms | **10 s** | 100 ms (library default) |
| | drain cap at teardown | 3 s | 3 s | 3 s |
| `best_effort_throughput` (c8) | reliability / history | BEST_EFFORT / no history | BEST_EFFORT / KEEP_LAST 1 (default) | same as CycloneDDS |
| `reliable_throughput` KEEP_LAST (c9) | history | KEEP_LAST 64 | **KEEP_ALL** (not passed `-K`) | **KEEP_ALL** (not passed `-K`) |
| `reliable_latency` (c10-c12) | reliability / history | RELIABLE / KEEP_LAST 8 | same | same |

**Environment, identical for all three:**
- Same two Raspberry Pi 5s, same session, interleaved by framework, same CPU governor.
- Every process pinned to cores 1-3 (`taskset -c 1-3`), away from the NIC interrupt core.
- Data restricted to the test link: eth0 for CycloneDDS (`CYCLONEDDS_URI`) and FastDDS (profile XML).
  TickLE uses its link broadcast.
- Release builds on all three sides. TickLE rows assert `core_build=release`; the vendors are apt
  release packages.
- Identical payload sizes, gated against every framework's measured limit by `check_bench_shapes.sh`.
- The same idle-based server lifetime and 3 s drain cap.
- `tc netem` applied to the client's egress, read back before each cell.

**Where the settings differ, and which way each difference cuts:**

1. **The KEEP_ALL bound is not identical.** It is equal in bytes for TickLE and CycloneDDS (about
   512 kB), while FastDDS may hold 4,000 samples whatever their size (about 11 MB at p4). At p1,
   TickLE holds the fewest samples of the three (2,048 against 4,000).
   - For throughput under loss, a deeper history lets more data stay in flight, so this cuts
     **against TickLE**.
   - For memory, it cuts **in TickLE's favour against FastDDS at p2-p4.** FastDDS's peak RSS grows
     from 17.4 MB at p1 to 28.3 MB at p4, and up to about 11 MB of that can be the history TickLE is
     not allowed to keep. Subtracting all of it still leaves FastDDS at about 17 MB against TickLE's
     2.2 MB, so **the verdict holds, but the margin shown against FastDDS at p2-p4 is inflated.**
2. **`max_blocking_time` is 10 s for CycloneDDS and 100 ms for the others.** It matters only when a
   writer's history is full. Every harness counts only successful writes, so no direction has been
   shown. It is still a QoS difference and is to be aligned.
3. **c9 does not compare KEEP_LAST.** TickLE runs KEEP_LAST 64 while both DDS harnesses run their
   KEEP_ALL default, because the campaign never passes them `-K`, although both accept it now. The
   summariser's policy rule voids the cell, so it is not scored unfairly, but it is not compared at
   all.
4. **Not a QoS difference, recorded so it is not mistaken for one:**
   - FastDDS sends a p4 sample as one UDP datagram for the kernel to fragment, because its transport
     default `maxMessageSize` is 65,500 B (DATAFRAG_PLAN section 12). That is FastDDS as shipped.
     Adding a tuned-FastDDS arm would be the user's decision.
   - CycloneDDS's `SPDPInterval` is shortened to 1 s. That affects only how quickly the two sides
     discover each other, not anything measured after they match.
   - TickLE runs `rmw_tickle`'s shipped defaults: the dynamic retry interval, piggybacked heartbeats,
     reorder slots equal to the tracking window, and the 512 KiB KEEP_ALL budget. The vendors run
     theirs.

**Correction to item 1 (Dev, 2026-09-26), found in the control arm of the flag that aligns it.**
TickLE's KEEP_ALL bound in these runs was not the 512 KiB arena. TickLE's harness reports its actual
bound as `keepall_bound_samples=`, and it is the reader's default 256-datagram window: **256 samples
at p1 and 128 at p4**, against the vendors' 4,000. So the asymmetry was larger than stated above, and
it cut both ways for TickLE too. TickLE buffered much less, which lowers its memory figures, and it
kept much less in flight, which costs its throughput under loss.

**Conclusion of the audit: no setting was found that decides a verdict in TickLE's favour.** One
difference (item 1, memory against FastDDS at p2-p4) makes a TickLE margin look larger than it is,
and items 1-3 are departures from "identical QoS" whichever way they cut. The planned correction
is to pass all three frameworks the same explicit bounds, and to re-run the affected cells:
- the KEEP_ALL bound as the same number of samples per payload shape, matching TickLE's 512 KiB
  budget
- `max_blocking_time` 100 ms for all three
- KEEP_LAST 64 for all three at c9

**The user decided on 2026-09-26 (translated): "if values were set differently anywhere, set them
to the same QoS and re-measure."** Every framework's reliable_throughput harness now takes `-N`, the
KEEP_ALL bound in samples (Dev, `47189580`). `campaign_sweep.sh` passes all three the same explicit
values, and voids any row whose RESULT line does not show them:

| cells | all three frameworks get | asserted on every row |
|---|---|---|
| reliable_throughput, KEEP_ALL (c1-c7) | `-N` = p1 2,048, p2 405, p3 368, p4 187 samples, and `-B 100` | `keepall_samples=`, TickLE's `keepall_bound_samples=`, `max_blocking_ms=100.000` |
| reliable_throughput, KEEP_LAST (c9) | `-K 64 -B 100` | `keep_last_depth=64` (TickLE: `keep_all=0 reliable_depth=64`), `max_blocking_ms=100.000` |
| best_effort, latency | unchanged: already identical | - |

The per-shape counts are what TickLE's shipped 512 KiB budget allows, `min(2048, 512 KiB / sample)`.
CycloneDDS's write-history watermark (~500 kB) is an implementation default, not a QoS, and is left
alone. At p4, 187 samples come to slightly more than that watermark.

**Pre-registered for the aligned run, before it starts:**
- **Unshaped throughput and bandwidth (c1-c4): within about 5% of the previous run for every
  framework.** The bound rarely binds without loss.
- **FastDDS's peak RSS falls at p2-p4**, toward about 17-19 MB at p4 from 28.3 MB, because its
  history was up to about 11 MB of that. **TickLE's rises somewhat**, since its reader window
  now grows to the bound. No memory verdict is expected to flip.
- **Under loss (c5-c7) TickLE's throughput holds or rises and the vendors' holds or falls**,
  because TickLE's in-flight bound grows (256 → 2,048 at p1, 128 → 187 at p4) while theirs shrinks.
- **FastDDS still does not complete c6**: the cause is its IP fragmentation, which no QoS changes.
- **c9 becomes a real comparison** and is expected to go to TickLE as c1 does.
- Any LOSE that appears is a result, reported as such, and not something to tune away.

**FastDDS tuned for a fair evaluation (the user's decision, 2026-09-26, translated: "let's adjust
FastDDS's parameters too, with the aim of evaluating it fairly").** FastDDS's shipped UDPv4 transport
default, `maxMessageSize` 65,500, makes it hand every p3/p4 sample to the kernel to IP-fragment (item 4
above). At p4 under 5% loss that lost 427 reassemblies and delivered 38%. `fastdds_eth0_only_mms1472.xml`
differs from the shipped profile in that one element, `maxMessageSize` 1472, so FastDDS fragments in
RTPS the way CycloneDDS and TickLE do:
- **Nothing else is tuned.** Heartbeat periods and every other FastDDS knob stay at their defaults,
  and CycloneDDS and TickLE are not tuned by analogy.
- **Both FastDDS configurations are reported.** The shipped one stays visible, and every FastDDS
  reliable_throughput RESULT line says `transport_profile=`.
- The change matters only where a sample exceeds one datagram, which is p3 and p4. At p1 and p2 the
  two profiles send identical datagrams.
- **At p3 the three frameworks now send different packet counts, and that is expected** (Dev's
  arithmetic). Tuned FastDDS's p3 message is about 1,484 B: RTPS 20 + INFO_TS 12 + DATA ~24 +
  encapsulation 4 + 1,424. That exceeds 1,472, so FastDDS splits p3 in two in RTPS. CycloneDDS
  already splits p3 at its 1,344 B FragmentSize, and TickLE sends p3 whole, since its limit is
  1,444 B. A shift in FastDDS's p3 row between the two profiles is this, not noise.

Pre-registered before the tuned runs:
- **Mechanism, p4 without loss:** the server's kernel reassembly requests per sample fall from 2.98 to
  about 0, with every sample written and `drained=acked`. **If FastDDS instead refuses or fails the
  writes, its default synchronous publish mode is not fragmenting.** The tuned arm is then invalid
  as it stands. Making it work would need a second parameter (the publish mode), and that goes back
  to the user rather than being changed here.
- **p4 under 5% loss: FastDDS delivers everything** (`drained=acked`, received = sent), so that cell
  becomes a scored three-way comparison.
- **p3/p4 without loss:** FastDDS's bytes per sample rise slightly against the shipped profile, since
  every RTPS fragment carries its own headers where an IP fragment carries 20 B, and its throughput
  stays about the same.
- **TickLE keeps every row.** Any row it loses is reported as lost.

### 4.5 The one thing the campaign predicted wrong, and what it cost

The sweep's header pre-registered that TickLE should *lose* c6 - P4 under loss - "because a lost IP
fragment costs it the whole datagram while RTPS retransmits one fragment". TickLE won it 4.5x. The
prediction's mechanism was real (TickLE's throughput retention falls from 90.6% at P1 to 6.6% at P4,
and its lead narrows from 9.9x to 4.5x) but its verdict was wrong, because both vendors degrade
further still.

Had the prediction not been written down first, that cell would have been recorded as a win and the
6.6% would have gone unremarked. It did not: the cell is now void for incomplete delivery, and
chasing *why* TickLE spends 47 transmissions per sample there led to the kernel's IP reassembly
collapsing rather than to anything in TickLE - item 15 in section 3, deferred by the user.

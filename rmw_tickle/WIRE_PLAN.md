# Wire protocol optimisation

> **Names (2026-09-27):** `tt_Node` is now `tt_Context`, `tt_NODE_*` is `tt_CONTEXT_*` and a public `node_id` field is `context_id`
> (rmw_tickle/CONTEXT_NODE_PLAN.md stage 1, 25ac7fe0). This document keeps the names of its time.

**User instruction (2026-09-26):** "지금 이대로 가면 rmw_tickle이 다른 DDS rmw를 이길 수 있을 것 같아. 그 다음엔
wire protocol 최적화를 시작하자. 예를 들어 announce 단계에서 요약본을 하나의 패킷 안에 announce 하고 필요한
정보를 별도로 요청해서 받게 되면 대역폭과 discovery 시간 모두 개선돼. 이와 같이 wire protocol을 변경해서 개선할
수 있는 부분이 있으면 그 것을 개선하도록 하자. 조건은 Wire protocol을 변경하기 전보다 모든 테스트에서 성능이 더
좋아져야 한다는 것이야."

Translation: once rmw_tickle beats the DDS rmws, optimise the wire protocol. The discovery summary with the
list requested separately (done: `DISCOVERY_PLAN.md`, `tt_VERSION` 8) is the model. **Condition: after a wire
change, every test performs better than before.**

The current format is specified in `DESIGN.md` "Wire Protocol", including the per-datagram overhead table
this plan starts from.

## 1. The rule, as applied (Plan's reading, set overnight 2026-09-26, for the user's review)

Each wire change is pre-registered with the tests it targets. It is merged only if:

- **(a)** every targeted metric improves beyond 2 × the combined standard error;
- **(b)** no metric anywhere in the campaign regresses beyond 2 × SE. The campaign is:
  - the native aligned cells (p1-p4, all QoS, with and without loss; COMPARISON `A`/`T` rows);
  - the rmw rows (block and poll sweep, RTT/CPU/RSS; `R` rows);
  - the discovery M-series.

**Open for the user:** many byte savings are real but too small for a latency test to resolve. For example,
12 bytes at 1 Gbit/s is ~0.1 us of a 250 us round trip. Such a change passes (b) as "not worse", but not
the literal "better in every test". If the user means the literal reading, only changes that move every
test are eligible. Until the user says so, (a) + (b) is applied, and each change's report says which
tests it moved and which it merely did not worsen.

## 2. W0: where the bytes go today (measure first)

Before choosing, measure what each test actually puts on the wire, split by kind: DATA framing, DATA
payload, HEARTBEAT, ACKNACK, discovery, and rmw's prefix. Plan does this from rig captures of the native
campaign cells and the rmw rows, counted with `tickle_pcap_count.py` for TickLE. It yields, per test, the
bytes each candidate below could remove, so candidates are ranked by measured share, not estimate.

## 3. Candidates (to be ranked by W0)

| id | change | saves | risk / cost |
|---|---|---|---|
| W1 | Shorter DATA header: the 4-byte `endpoint_id` + 4-byte `entity_id` become a 2-byte per-node writer handle, announced in the discovery list | ~6 B per DATA and FRAG_FIRST | DATA arriving before its writer's list can't be routed. Today `endpoint_id` alone lets a subscriber match data before discovery completes. A handle must keep that (e.g. fall back to the long form until the list is acked), or durability/late-join cells may regress. |
| W2 | 32-bit timestamp (us, high bits rebuilt on receive) instead of 64-bit ns | 4 B per DATA | ns → us precision for `source_timestamp`, and rebuilding relies on clocks within ±35 min. LIFESPAN and DEADLINE are unaffected at us. |
| W3 | rmw's 8-byte publication sequence number sent as a varint delta from the core `seq_no` (the difference is the fragments so far, usually 0) | ~7 B per rmw sample | rmw-only; needs the core's datagram seq_no visible to rmw on receive. |
| W4 | One combined header for a datagram with a single submessage (`tt_Header` + `tt_SubmessageHeader`, 8 → 4-5 B) | ~3-4 B per datagram | Two parse paths. |
| W5 | Discovery list: type names written once per list, then referenced | large share of a list | Lists are only sent on change or request since v8, so this moves join bytes and M3 time, not steady state. |
| W6 | HEARTBEAT/ACKNACK cadence under load (piggyback rate, ack coalescing) | packets in reliable cells | Behaviour, not format. Recovery latency under loss must not regress. |

Payload encoding (CDR-4) is out of scope: it is the interface contract, not framing.

## 4. Order

1. **W0** (Plan, overnight).
2. Dev finishes LIVELINESS (`tt_VERSION` 9).
3. Candidates in W0's order, **one per `tt_VERSION`**. Each is pre-registered here before code: its target
   metrics, and the full campaign before and after, run on one rig session per arm.
4. A candidate that fails (b) is reverted and recorded here with its numbers, not retried silently.

## 5. W0 result (2026-09-26, `22cc13f1`)

`wire_inventory.sh`: campaign_sweep's 12 TickLE cells over veth on the PC, 12 ok. The byte shares are exact;
the throughput cells' ack shares are indicative. `results/wire_inventory_W0_2026-09-26.txt`.

| cells | bytes/sample | TickLE framing | HB + ACKNACK | W1 | W2 | W4 |
|---|---:|---:|---:|---:|---:|---:|
| p1 throughput and latency (76 B), all QoS/loss | 146-161 | ~19% | 0-0.9% | 3.7-4.1% | 2.5-2.7% | 2.4-2.5% |
| p2-p4 (1.3-2.9 KB) | 1,363-2,945 | 1.7-2.1% | ≤ 0.1% | 0.2-0.4% | 0.1-0.3% | 0.2-0.3% |

TickLE framing here is `tt_Header`, the submessage header and the DATA/FRAG header. Discovery is 3.9% of the
p1 latency cells (10 s runs at 10 Hz) and ~0 elsewhere.

**What this means for the plan:**
- **The format candidates together remove ~9% of p1's bytes and under 1% of every larger shape's.**
  HEARTBEAT/ACKNACK is already under 1% (piggybacked), so W6 has almost nothing to take.
- **None of them can move a latency test** (~0.1 us of ~250 us). **Nor can they move p1 throughput**,
  which is bound by per-sample CPU and syscalls, not the link: 116 Mbit/s on a 1 Gbit/s link. At p4,
  which is link-bound, they are worth 0.2-0.5% of rate.
- So under the user's literal condition ("every test better"), **no format candidate qualifies**. Under
  Plan's reading (targets improve, nothing regresses), they qualify on bytes/sample alone. This is
  stated here plainly for the user's morning review. The work proceeds under Plan's reading, and each
  report lists which tests moved and which only held.

**Decision (Plan, overnight):**
- The format candidates go out as one bundle, one `tt_VERSION` after LIVELINESS: W2, W3 (rmw), W4, and W1
  only if its before-discovery routing is solved without a regression.
- Each candidate's bytes are measured exactly and separately with `wire_inventory` on the PC.
- The full rig campaign runs once, bundle against baseline, for the no-regression check. This keeps rig
  time for the one comparison that needs it.
- W6 is dropped. W5 moves the discovery M3 join bytes only, and waits.

The larger remaining gains found today are not wire formats: the pong's receive wake (~6 us, ppoll against a
blocking recv, `RMW_PERF_PLAN.md` 8.2) and the ~21 us poll-thread → executor handoff. Both stay first in
line for Dev after LIVELINESS.

## 6. First bundle, `tt_VERSION` 10: W2 + W3 + W4 (pre-registered 2026-09-26, before any code)

W1 is held until DATA that arrives before its writer's list can still be routed without a regression.

**The changes:**
- **W2:** the DATA/FRAG_FIRST timestamp becomes 32-bit microseconds. Its high bits are rebuilt on receive from
  the receiver's clock, which assumes the two clocks are within ±35 min of each other. Saves 4 B per sample.
- **W3:** rmw_tickle's 8-byte publication sequence number is sent as a varint delta from the core `seq_no`. The
  difference is the fragments so far, usually 0, so this is ~1 B instead of 8 and saves ~7 B per rmw sample.
- **W4:** a datagram carrying exactly one submessage uses one combined header. Saves ~3.5 B per such datagram.

**Targets, exact (`wire_inventory.sh`, the 12 TickLE cells on PC veth; bytes are deterministic, so no SE):**

| cells | today (W0) | expected | how |
|---|---:|---:|---|
| p1 (76 B), per sample | 146-161 B | -7.5 to -8 B (about -5%) | W2 4 + W4 ~3.5-4 |
| p2-p4, per sample | 1,363-2,945 B | the same absolute -7.5 to -8 B, < 0.6% | as above |
| rmw Bench, per sample (rig capture) | ~204 B | about -15 B | W2 4 + W4 ~4 + W3 ~7 |

A candidate whose measured saving falls short of its expectation by more than 1 B per sample is investigated
before the campaign.

**No-regression (the rule, 2 × SE):** one rig campaign, bundle against its parent. It covers:
- the native aligned cells (A/T rows);
- the rmw scored session (V rows);
- the poll sweep (P rows);
- the discovery M2/M3 grid.
Nothing may move beyond 2 × SE in the wrong direction. The report lists every row that moved, and every row
that only held.

**Correctness, before the campaign (unit tests, each killed by its mutant):**
- **W2:** a timestamp at the wrap boundary and at ±30 min of skew rebuilds exactly; the LIFESPAN and DEADLINE
  tests are unchanged.
- **W3:** the psn across fragment counts 1-32, across a writer restart, and across the uint16 psn wrap that
  43d49fa8 fixed.
- **W4:** both parse paths, with mixed single- and multi-submessage datagrams in the fuzz corpus.

**As stated in section 5:**
- These savings cannot move a latency test or p1 throughput. The bundle can therefore meet the user's
  condition only under Plan's reading (targets improve, nothing regresses), not the literal "every test
  better".
- The report says so again. If the user rules for the literal reading, the bundle is reverted.

### 6.1 Revised before measurement (2026-09-27): CDR alignment caps W3 and shapes W4

Dev found the constraint while building W2. The generated CDR code reads fields through direct pointer casts
and relies on the payload being 4-byte aligned in the datagram (`tools/typesupport` emit.py, and `tickle.c`'s
static asserts). So every framing element in front of the CDR must remain a multiple of 4 bytes.

- **W2 as built:** a 16-byte DataHeader and a 17-byte FRAG_FIRST. The largest single-DATA CDR grows from 1,444 to
  1,448 B. The timestamp is rebuilt within ±35.8 min, and callbacks still get ns. Tested at ±30 min of skew,
  across the 32-bit us wrap, and on us truncation; two mutants killed.
- **W3 revised:** the psn prefix becomes a varint of ((first datagram seq_no - psn) mod 2³² << 1 | has_high),
  followed by the high 32 bits only when psn ≥ 2³², zero-padded to 4. That is typically 4 B instead of 8, so
  **-4 B per rmw sample, not the pre-registered -7**. The shortfall is alignment. A trailer after the CDR would
  save ~5.5 B on average for more complexity. **Plan's decision: the prefix form.**
- **W4 as designed:** a 4-byte combined header is used only for a datagram carrying exactly one submessage
  addressed to all nodes: DATA, fragments, the summary and ordinary HEARTBEATs. Its first byte is a new
  single-form marker ('k'/'t'), followed by version, source and type; the length is taken from the datagram.
  Saves 4 B per such datagram.
  - The cost is a weaker foreign-traffic check: one marker byte and the version instead of two magic bytes
    and the version. A random datagram passes with ~3 × 10⁻⁵ probability, and must then also hit a local
    32-bit endpoint hash, so this is accepted.
  - Both parse paths go into the fuzz corpus.

**Revised byte targets (still before measurement):**
- p1: -8 B per sample (W2 4 + W4 4).
- p2-p4: -8 B on the first fragment and -4 B on each later one.
- rmw Bench: -12 B per sample (W2 4 + W3 4 + W4 4).

## 7. v10 (`fd57b01d`) against its parent (`8f3811f4`): bytes (2026-09-27)

`wire_inventory.sh` with SHA pinned, the 12 TickLE cells on PC veth, 12 + 12 ok, and the discovery M2 grid, 9 + 9 ok.
Raw rows are `results/wire_v10_{inv,m2}_*_2026-09-27.txt`. Bytes on the wire per sample:

| cells | parent | v10 | difference | target (6.1) |
|---|---:|---:|---:|---:|
| p1 throughput, every QoS | 146.0-146.1 | 138.0-138.1 | **-8.0** | -8 |
| p2 / p3 throughput | 1,362.7 / 1,494.7 | 1,354.6 / 1,486.6 | **-8.1** | -8 |
| p4 throughput | 2,932.3 | 2,920.2 | **-12.1** | -12 (-8 on the first fragment, -4 on the second) |
| p1 / p2 latency | 161.0 / 1,377.0 | 152.3 / 1,368.3 | **-8.7** | -8, plus the smaller summaries |
| p4 throughput, 5% loss | 2,943.4 | 2,934.1 | -9.3 | -12 |
| p1 throughput, reorder | 146.6 | 139.2 | -7.4 | -8 |

- **Every target is met within 1 B except the p4 loss cell.** Its framing fell as designed: headers -7.6 and DataHeader
  -4.0, so -11.6 B. That run's retransmitted payload was 2.2 B per sample higher than the parent's, so random loss
  masks the difference. The format is not the cause.
- **Discovery M2 steady state falls 3-6% in every cell**, since the summary lost 4 B to W4. At N = 16, E = 32 it went from
  1,101 to 1,046 B/s received per node.

The no-regression part, the rig campaign of bundle against parent (native cells, rmw block and poll sweep, and the rmw
capture), is running and follows in section 8.

## 8. v10 against its parent on the rig: the no-regression rule (2026-09-27)

### 8.1 Native cells: bytes fall everywhere, CPU per sample rises everywhere, so v10 fails as it stands

`campaign_sweep.sh`, TickLE only, 5 repetitions per build, the parent (`8f3811f4`) and then v10 (`fd57b01d`) in one rig
session. All 60 + 60 rows are ok. Raw rows are `results/wire_v10_campaign_{8f3811f4,fd57b01d}_2026-09-27.txt`, and
the reading is `experiments/ab_compare.py campaign` at 2 x SE, which was checked on a file against itself (all held)
and on figures inflated in either direction. It gives 34 better, 52 held and **22 WORSE**.

- **Better.** Wire bytes per sample fall in every cell, matching section 7: -5.5 to -6.1% at p1, -0.4 to -0.6% at
  p2-p4. The send rate rises 0.4-0.6% at p2, p3 and p4, where the link is the limit.
- **WORSE.** CPU per sample rises 0.4-1.2% at t 2.3-15, and the p1 send rate falls 0.6-1.1%, since p1's
  sender is CPU-bound:
  - p1 client at c1, c5, c8 and c9;
  - p1 server at c5, c7, c8 and c9;
  - p2 server at c2.
- **It is user time, not the kernel.** Per sample, stime is flat or lower, since there are fewer bytes to copy. utime
  rises in every cell:
  - client: +15 to +40 ns, e.g. c1 895 -> 912 and c5 935 -> 974;
  - server: +20 to +70 ns, e.g. c1 618 -> 672 and c5 792 -> 862.

  `cpu_mhz` is the same in both arms. The arms ran one after the other, not interleaved, but a drift would not rise
  in user time alone and in every cell.
- **Suspected cause, not yet measured:**
  - The receive path rebuilds the 64-bit timestamp from the u32 in `timestamp_from_wire()`, which reads the clock
    (`tt_get_ns()`) once per received sample. The parent made no clock read there.
  - The send path adds a 64-bit division and `to_single_form()` to every datagram.
- **Consequence under section 1's reading:** v10 may not stand with a CPU regression, however small. Dev fixes the
  cost without giving up the bytes. The fix is judged by the same campaign, re-run as interleaved A B B A blocks against
  `8f3811f4`.

### 8.1a Dev: the cause measured on the PC, and the fix (2026-09-27)

`perf` is not available to the sessions on the PC (`perf_event_paranoid` 4, no sudo for it), so the cost was measured
with a benchmark of TickLE core alone: `experiments/core_cost_bench.c`, driven by `experiments/core_cost_ab.sh`.
- **The benchmark.** Two nodes run in one process, with a HAL of their own: sends are captured into memory, and receives
  hand the captured datagrams back through `tt_Node_poll()`. `tt_get_ns()` is the real clock, and its calls are counted.
  It measures p1 Bench samples BEST_EFFORT. The send phase publishes N samples, polling once after each; the receive
  phase hands them all to the subscriber's node. There is no kernel and no network, so this is user time only.
- **The runs.** Each build ran pinned to one CPU, in rounds alternating between builds. Differences are paired by
  round (mean +- SE).

**Result, 300,000 samples x 40 rounds, against the parent `8f3811f4`:**

| build | send ns/sample | recv ns/sample | clock reads per sample (send / recv) |
|---|---:|---:|---|
| parent `8f3811f4` | 189.6 | 83.8 | 2 / 1 |
| v10 (at `22a8f2cb`) | -2.4 +- 0.7 | **+26.4 +- 0.5** | 2 / **2** |
| this fix | +0.1 +- 0.9 | -1.4 +- 0.7 | 2 / 1 |

- **The receive regression is the clock read**, as suspected. `timestamp_from_wire()` read the clock once per
  received sample, which the parent did not; on the PC that is +26 ns per sample. On the Pi, the server's utime rose
  19-70 ns.
- **The fix rebuilds the timestamp against the time the running poll already read** (`tt_Node.rx_clock_ns`). Any time
  within minutes will do, because the window is +-35.8 min. The poll refreshes it after every wait and clears it on
  return, and a receive outside a poll reads the clock itself.
- **Storing it cost the sender ~3 ns, and the storage was changed to remove that.** The sender polls once per sample, so
  the first version (a microsecond value, divided on every poll) cost it about 3 ns. It is now stored as raw
  nanoseconds, beside `poller_thread`, which the poll writes anyway.
- **The send path shows no v10 regression on the PC**: -2.4 +- 0.7 ns. The 64-bit division is by a constant (a
  multiply), and `to_single_form()` rewrites four bytes. The +15-40 ns seen at the Pi's clients is not reproduced by
  the core alone. The interleaved campaign will say whether it was the sequential arms or something the Pi does
  differently.
- **Found on the way.** LIVELINESS_PLAN 10's summary skip (`397d927c`) recorded every send with an atomic
  read-modify-write, which cost ~8 ns per send.
  - It is now plain, since every send runs under the state lock.
  - It records nothing unless the node's summaries run at the short-lease cadence, which is the only case with
    anything to skip. A node without short leases, like every campaign cell, pays one branch.

For the ABBA campaign against `8f3811f4`: the build to use is this commit.


### 8.2 rmw rows and the rmw capture (2026-09-27)

`rmw_crosshost_rtt.sh`, rmw_tickle only. Block mode and the poll sweep (0/50/100/200 us), bench and array1k, both QoS,
3 repetitions per build, the parent's session and then v10's. All 60 + 60 rows are ok. Raw rows are in
`results/wire_v10_{rmw,cap}_{8f3811f4,fd57b01d}_2026-09-27.txt`.

- **Bytes, from the capture.** The ping and pong data datagrams fall from 112 to **100 B (-12 B, the target)** in both
  QoS, and the summaries from 28 to 24 B. All 100 replies are unicast. The only broadcasts other than summaries are
  the three discovery datagrams each node sends at creation, so the pong-side first-reply broadcast does not show here.
- **Timing and CPU.** `ab_compare.py rmw` finds 3 better, 106 held and 11 WORSE out of 120 comparisons. That is what
  chance gives at n = 3: simulated for normal data, P(t > 2) is 6.0% one-sided, so about 7 WORSE and 7 better are
  expected under no change at all.
  - Two of the WORSE rows are the ping's tick-counted CPU (0.04 against 0.05 s, one 10 ms tick).
  - Two are the pong's peak RSS (+4 and +13 KB).
  - One is the pong's CPU at bench BEST_EFFORT, poll 50 us (+3.1%). The same metric is 3.1% better at bench RELIABLE,
    poll 0 us.
  - Six are round trips, +1-2 us (+0.2-0.8%). They lean one way: no round-trip row is better. The native path's
    +26 ns per received sample (8.1a) cannot make 1-2 us, so a drift between the two sessions is the likelier
    reading. The interleaved re-run below decides it.

### 8.3 Reading rule for many comparisons (amendment, 2026-09-27, before the A B B A results)

Sections 1 and 6 say "nothing WORSE beyond 2 x SE". Applied to about 110 comparisons per run it fails a build that
changed nothing: at 6 repetitions per arm, P(t > 2) is 3.8% one-sided, so about 4 WORSE rows are expected by chance.
From here on, and before any A B B A result is read:

1. **Candidate.** A row WORSE at 2 x SE in the pooled A B B A run.
2. **Confirmed**, and the build fails, if either
   - the same metric is WORSE in at least half of the cells it appears in (8.1's utime was WORSE in every p1 cell;
     chance cannot do that), or
   - a targeted re-run of the candidate's cells, A B B A with 6 per arm, finds it WORSE again. Two chance WORSE in a
     row are about 0.15% per metric, about 0.2 expected over the whole table.
3. **Not confirmed:** the row is listed as a chance candidate with its t, and not hidden.
4. **Not counted as timing or CPU evidence:** figures quantised coarser than the difference (tick-counted CPU,
   `ping_cpu_s`). They stay in the table.

The v10 fix (`4dc7ad49`, 8.1a) is judged this way on the native cells (`campaign_ab_chain.sh`) and on the rmw rows
(`rmw_lease86_rig_chain.sh` with TAG=wire_v10fix_rmw), both queued on the rig.

**Amendment, 2026-09-27 (after CONTEXT_NODE_PLAN.md 4a): code layout and between-run drift.** In `core_cost_ab.sh`
on the PC, one binary does not reproduce itself between runs to within 2 x SE.

- **The evidence (4a).**
  - The same bench binary, byte-identical in code and addresses, was measured in two separate 20-round pairs against
    one parent: default-mode receive -0.5 ns (2 x SE 0.9), then +1.2 ns (2 x SE 0.7).
  - Moving only cold functions, with not one instruction of the receive path changed, gave -R receive -4.3 ns
    (2 x SE 1.1).
  - A within-run 2 x SE therefore measures run-to-run noise inside one run. It does not measure layout, or drift
    between runs.
- **From here on,** a bench CPU criterion on a commit that changes code size is read against two controls:
  - a **placement control**: the same commit with its changed cold code relocated, or an A/A pair of a trivially
    re-linked binary;
  - a **repeat** of the pre-registered pair on another occasion.
  - A difference inside the swing those controls show is recorded as **layout**, not WORSE.
  - A difference beyond it, in the same direction in every repeat, is WORSE.
- 8.9's and D5's few-ns results are consistent with this. They are noted here, not re-judged.

### 8.3a The Pi bench's own floor (2026-09-27, Plan)

`core_cost_pi_drift.sh`: one build (`f22fd4fa`) measured against itself on the client Pi, 4 occasions 15 min apart, 10
rounds each, default and -R. Raw rows are `results/core_cost_pi_drift_2026-09-27_summary.txt`. Same binary against
itself, ns per sample:

| mode | send A/A over 4 occasions | recv A/A over 4 occasions |
|---|---|---|
| default | -0.62, +2.22, -0.87, +1.01 | +0.04, +0.14, +0.16, -0.17 |
| -R | -2.04, -2.05, +0.62, +0.07 | -1.04, -0.25, +0.30, +0.37 |

- **2 of the 16 comparisons fall beyond their own within-run 2 x SE**, with nothing changed. So a within-run 2 x SE
  alone overstates what a single Pi difference means, as it did on the PC (CONTEXT_NODE_PLAN 4a).
- **The Pi floor, applied from here on:** a core-bench difference inside ±2.5 ns on send or ±1.2 ns on receive is not
  evidence of a change, however many SE it is. Beyond it, in the same direction on a repeat occasion, it is.

### 8.4 The fix (`4dc7ad49`) against the parent, A B B A: the receiver is fixed, the sender is not (2026-09-27)

`campaign_ab_chain.sh`, `8f3811f4` / `4dc7ad49` / `4dc7ad49` / `8f3811f4`, 3 repetitions per block, 72 + 72 ok rows.
Raw rows are `results/wire_v10fix_abba_{A_8f3811f4,B_4dc7ad49}_2026-09-27.txt`. Read by 8.3: 40 better, 48 held, 20 WORSE.

- **Bytes:** better in every cell, as in 8.1.
- **Receiver: fixed.** Server CPU per sample is now better in c2, c3, c4, c8 and c9 (-0.4 to -1.1%). It is WORSE only
  in c5 (+0.3%, t 3.9): one cell of nine, so a chance candidate under 8.3. Server utime still rises 13-43 ns, but its
  stime falls 20-45 ns, so the total falls.
- **Sender: confirmed WORSE.** Client CPU per sample is WORSE in 6 of the 9 throughput cells, which meets 8.3's "at
  least half" rule:
  - p1 RELIABLE: c1, c5, c7, c9, +0.7 to +0.9%;
  - p4: c4 +0.5% and c6 +1.1%.

  The one BEST_EFFORT cell, c8, is 1.4% *better*. The rise is user time, +39 to +53 ns per sample in the reliable
  cells against +2 in c8, with stime flat. So on the Pi v10 costs the reliable sender ~40-50 ns per sample.
  - The client's peak RSS is WORSE in c8 and c9 (+5 and +11 KB), 2 cells of 12: chance candidates.
- **Why the PC bench missed it:** its sender is BEST_EFFORT, and the cost is on the Pi. Dev is now repeating the bench
  in RELIABLE, and reading v10's reliable send path. The candidates: the cache record's layout, and the payload's
  alignment after the 16 B DataHeader. A Pi run of the bench follows on the rig.
- **Status: v10 on `main` fails section 1's rule on the reliable sender's CPU, +0.5 to 1.1%.** It stays for the
  user's ruling at 08:00 unless a fix is measured by then. Reverting it would mean untangling it from the liveliness
  work committed after it.

### 8.5 The core alone on the Pi: v10's sender cost is +3 ns, not +40 (2026-09-27)

`core_cost_pi.sh` (Dev, fixed by Plan for a stdin defect that made the first run measure one ref only). It is
TickLE core's own send and receive per sample, built natively on the client Pi (10.1.1.214) at each commit and run
alternately on one pinned CPU. It uses an in-memory HAL, so it measures no socket calls. 15 paired rounds x 200k
samples. Raw rows are `results/core_cost_pi_{R,BE}_2026-09-27.txt`. ns per sample against `8f3811f4`, +- SE:

| mode | `4dc7ad49` (v10 + fix) send | recv | `fd23de40` (+ D1 + D3) send | recv |
|---|---:|---:|---:|---:|
| RELIABLE, KEEP_LAST 64 | **+3.2 +- 0.7** | +1.8 +- 0.3 | -26.5 +- 0.8 | -31.0 +- 0.3 |
| BEST_EFFORT | +1.7 +- 1.1 | -6.8 +- 0.8 | -33.6 +- 1.5 | -39.3 +- 0.8 |

- **In the core, v10 with its fix costs the reliable sender 3 ns per sample.** That is real (t 4.4) but a tenth of
  what 8.4's campaign client showed (+39 to +53 ns of user time). The rest is outside the core loop the bench runs.
  The bench leaves out the HAL's socket calls, the client harness and the ACKNACK arrivals as the rig times them.
  Neither the HAL nor the harness changed between the two commits.
- **D1 + D3 more than cover it:** -26 to -34 ns on send and -31 to -39 ns on receive, on the Pi.
- **What this means for section 1:**
  - As v10 stands on its own, the rule is still broken by the campaign's +0.5-1.1% on reliable client CPU.
  - `main` also carries D1, D3 and (after its rig verdict) D4. Measured against `8f3811f4` as a whole, it is very
    likely better on every CPU row. Whether that counts is the user's reading of the rule. Plan's reading judges
    the wire change on its own, which is why 8.4 kept D1 out of the arm.
  - A final `8f3811f4` against `main` A B B A is queued after the D-series run, so that either reading has its number.

### 8.6 Commit by commit, the clients' own send loop on the Pi: v10 costs the core nothing (2026-09-27)

`core_cost_pi.sh` with `BENCH_ARGS="-c -R"`, which runs the campaign clients' scheduler-driven send (`send_one()`
rescheduling itself under `tt_Node_poll(-1)`) on a RELIABLE KEEP_LAST 64 writer. 10 paired rounds per ref. The
ablation arm `73edfe5f` is `4dc7ad49` with every seq_cst atomic in tickle.c made relaxed; it is never merged. Raw
rows are `results/core_cost_pi_cR_2026-09-27.txt`. ns per sample against `8f3811f4`, +- SE:

| ref | what it adds | send | recv |
|---|---|---:|---:|
| `fd57b01d` | v10 | **-0.5 +- 0.8** | +26.2 +- 0.6 |
| `22a8f2cb` | 8.6 | -0.6 +- 1.4 | +20.2 +- 0.4 |
| `397d927c` | summary skip (atomic per send) | +13.1 +- 1.1 | +19.7 +- 0.5 |
| `4dc7ad49` | receive-clock fix, skip gated | +5.8 +- 1.7 | -3.1 +- 0.3 |
| `eef6f089` | D1, D3, 8.6 reverted | +4.3 +- 1.9 | -29.3 +- 0.6 |
| `73edfe5f` | ablation: relaxed atomics | +4.5 +- 1.3 | -2.1 +- 0.6 |

- **v10's wire format costs the core's send loop nothing on the Pi (-0.5 +- 0.8).** Its receive cost (+26) is the
  clock read fixed in 8.1a.
- **The ARM atomics are not the cost:** relaxing them moves send by -1.3 +- 2.2.
- **A few ns of send cost arrived with the summary skip (+13, gated to +5.8 in `4dc7ad49`) and remain at `eef6f089`
  (+4.3).** Dev follows this up; it is not a wire change.
- **The campaign's +40-50 ns on the reliable client in 8.1/8.4 is therefore not in the core's code.** The bench runs
  every line of the core except the HAL's socket calls, so what remains is the real send path. There v10 hands
  `sendmsg()` a datagram whose single-form head starts 4 bytes into `tx_buffer`, a changed start and length. The
  alternative is the tick-based utime/stime split misattributing kernel time. The next step is a real-socket A/B on
  the Pi of `8f3811f4` against `fd57b01d`, the same loop over UDP. If the head's alignment is the cause, building
  the single form so that the datagram's first byte stays 8-aligned is the fix. That is for the morning.

### 8.7 The fix's rmw rows, A B B A: nothing confirmed WORSE (2026-09-27)

`rmw_lease86_rig_chain.sh` with TAG=wire_v10fix_rmw, `8f3811f4` / `4dc7ad49` / `4dc7ad49` / `8f3811f4`. Each block has 3
repetitions, IDLE_S=10, block mode and the poll control at 100 us, bench and array1k, both QoS. 48 + 48 ok rows, 0
VOID. Raw rows are `results/wire_v10fix_rmw_{A_8f3811f4,B_4dc7ad49}_2026-09-27.txt`. Read by 8.3: 4 better, 49 held,
3 WORSE.

- **The round trip holds in every cell.** Block mode is -0.8 to +0.5% (t -1.5 to +1.1), and poll -0.2 to +0.3%. The
  +1-2 us lean of 8.2's sequential run was the session, not the build.
- **The WORSE rows are all chance candidates,** one cell each:
  - the pong's peak RSS at bench BEST_EFFORT block (+7 KB);
  - the pong's CPU at array1k RELIABLE block (+2.2%, t 2.3). The same metric holds in the other 7 cells.
  - the ping's tick-counted CPU, which 8.3 rule 4 does not count.
- **Result:** the rmw half of the rule is met at `4dc7ad49`. What still fails section 1 is the native reliable
  client's CPU (8.4). Its cause lies outside the core loop (8.6).

### 8.8 The real socket on the Pi: where the client's +40 ns goes (pre-registered 2026-09-27, before code)

**Why.** The campaign's native RELIABLE client costs 40-50 ns more user time per sample under v10 (8.4). The client
runs at full clock on one CPU, with an identical per-sample event mix in both arms (Dev, 8.6 addendum). The core
alone, in every loop measured on the Pi and the PC, shows +3 to +6 ns (8.5, 8.6, OPTIMIZATION_PLAN 11.5). The one
path the bench does not run is the real HAL: `hal_linux.c`'s sends, receives and polls on real UDP sockets. Its code
is unchanged, but what core hands it changed with v10: a 20-byte single-form head starting at `tx_buffer + 4`, and a
different `tt_Node` layout around `node->hal`.

**The measurement.**
- `core_cost_bench` built with `-DBENCH_REAL_HAL`: the same two nodes and the same `-c -R` loop (the clients'
  scheduler-driven RELIABLE send), but through `hal_linux.c` on the Pi's own interface.
- Data goes node to node over the Pi's local address; the discovery broadcast leaves on the rig link, which is idle
  under the rig lock.
- Each round sends 512 samples, and then the reader drains them.
- Per phase, the thread's utime and stime (`getrusage(RUSAGE_THREAD)`) per sample, the campaign's metric, next to
  wall time.
- Arms: `8f3811f4`, `fd57b01d` (v10 alone) and current main, 10 rounds, paired, pinned (`core_cost_pi.sh`, which
  takes the rig lock, hil scope).

**How to read it, written before running:**
- **send utime rises ~+40 ns at `fd57b01d`:** the cost is on the real send path. Then, and only then, a hypothesis
  arm: the single form built so that the datagram it hands `sendmsg()` starts 8-aligned (the classic header placed
  at `tx_buffer - 4` relative, so the single form begins at `tx_buffer + 0`), against `fd57b01d`. If that arm
  removes the rise, alignment is the cause.
- **send utime flat (within 2 x SE) at `fd57b01d`:** the real socket path does not carry it either. The remaining
  difference between this loop and the campaign client is the peer: a second Pi's ACKNACK timing and the NIC. That
  goes back to a campaign-level experiment, and the report says so.
- **Anything else,** e.g. the rise appearing only in stime, or only at main: recorded as seen, with no mechanism
  claimed.
- **Control:** `8f3811f4` against itself as a second arm pinned the same way. Its difference must be within 2 x SE,
  or the run is void.

**Result (2026-09-27, the Pi `10.1.1.214`, CPU 3, 10 rounds x 400000 samples; `/tmp/core_cost_pi_socket_2026-09-27.txt`).**

How it was run, where it departs from the plan above:
- The bench is its own file, `experiments/core_cost_socket.c`, rather than `core_cost_bench -DBENCH_REAL_HAL`. It
  links TickLE as an application does, with no whitebox include. `core_cost_pi.sh BENCH=socket` runs it.
- A round is 256 samples, not 512. The first run, at 512, is void: every row of all four arms stalled after its
  first round, 443 or 444 of 512 received. The Pi caps `SO_RCVBUF` at `net.core.rmem_max` = 212992, which holds
  ~443 of these datagrams, and nothing polls the writer while the reader drains, so a drop is never resent. The
  PC's cap is 4 MiB, which is why the PC run did not show it. The bench now takes the round as its second argument.
  `core_cost_pi.sh` also marks a stalled row as failed, so a run like that can no longer be summarised as data.
- The control arm is `8f3811f4^0`: the same tree archived and built a second time, and run as a separate arm.
- The same bench on the PC, in a private netns at 512, gave `fd57b01d` send user+sys +8.3 +- 5.4 ns.

Paired against `8f3811f4`, per sample, in ns (mean of the 10 round differences, +- SE):

| arm | send wall | send utime | send stime | send user+sys | recv wall | recv utime |
|---|---|---|---|---|---|---|
| `8f3811f4^0` (control) | -7.3 +- 5.1 | -8.3 +- 22.1 | -11.0 +- 22.2 | -19.3 +- 12.4 | -0.8 +- 3.6 | -10.1 +- 8.4 |
| `fd57b01d` (v10 alone) | -0.5 +- 2.1 | -20.7 +- 8.9 | +24.1 +- 14.8 | +3.4 +- 14.3 | +14.3 +- 3.6 | +20.4 +- 9.2 |
| `86492b99` (main) | +24.5 +- 5.2 | -10.6 +- 15.3 | +8.7 +- 18.5 | -2.0 +- 17.9 | -65.2 +- 3.5 | -56.4 +- 6.5 |

Base medians: send 5387 ns, recv 874 ns. Every run delivered 400000 of 400000, with the same datagram count in
every arm (400006 or 400007).

How it reads:
- **The control passes.** Each of its columns is within 2 x SE.
- **Send utime is flat at `fd57b01d`, by the pre-registered branch.** There is no rise, and the +40 ns is not on
  the real send path. The alignment arm is therefore not run.
  - The utime -20.7 and stime +24.1 split is the tick moving between the two. Their sum, +3.4 +- 14.3, is flat.
  - Send wall at `fd57b01d` is -0.5 +- 2.1.
  - What is left between this loop and the campaign client is the peer: a second Pi's ACKNACK timing, and the NIC.
    That is a campaign-level question, not a core one.
- **Recorded as seen, with no mechanism claimed:**
  - `fd57b01d` receives +14 ns wall (4 x SE). This is v10's receive cost, which the D-series (OPTIMIZATION_PLAN 11)
    was written against.
  - Main receives -65 ns wall and -56 ns utime against the pre-v10 parent (-7.5%).
  - Main's send wall is +24.5 +- 5.2 (+0.45%, 9 of 10 rounds above the base median), while its send user+sys is
    flat (-2.0 +- 17.9). The wall time outside the thread's CPU grew from 244 to 279 ns (medians).
    - It is not the wire change: `fd57b01d` is flat.
    - It comes from a post-v10 change.
    - By 8.3 it is only a candidate. The 8.9 campaign A B B A has no CPU or rate row WORSE at main.

**Follow-up: where main's send wall +24.5 comes from (pre-registered 2026-09-27, before running).**

Plan asked for this bisect. The measurement is the 8.8 bench and runner unchanged, with a few settings:
- Round 256, 16 rounds x 400000 samples, CPU 3, under the hil lock.
- `ALTERNATE=1`: every second round runs the refs in reverse order, so a drift along a round lands on every arm
  alike.
- `STEPS=1`: each arm is also paired against the arm listed before it.

The arms, in order:
1. `fd57b01d`, the base.
2. `fd57b01d^0`, the control.
3. Every post-v10 commit that touches `src/` or `include/`, oldest first: `22a8f2cb` `397d927c` `4dc7ad49`
   `5f460ad2` `45cc3299` `3015e15d` `c51ac8c2` `eef6f089` `6f019b1d` `10999967`.
4. `86492b99` last. Its `src/` and `include/` are identical to `10999967`'s, which makes it a second control at
   the far end.

How to read it (send wall; an adjacent step's SE was 5-6 ns at 10 rounds, so ~4-5 at 16):
- **Void** if either control's difference is outside 2 x SE: `fd57b01d^0` against `fd57b01d`, or `86492b99`
  against `10999967`.
- **Not reproduced** if `10999967` against `fd57b01d` is below +12 ns or below 3 x SE. The 8.8 figure then stays
  an unexplained candidate. Nothing further is run.
- **A step is real** at >= 3 x its own SE and >= 12 ns, which is half the effect.
  - With 11 steps, 3 x SE keeps the chance of a false step near 3%.
  - 12 ns is the smallest step that could carry half of +24.5.
  - One real step carrying at least half the total names that commit. Its mechanism is then examined on that
    commit alone, pre-registered separately.
  - Two or more real steps: each is named, and their sum is set against the total.
  - A total that reproduces with no real step: it is spread over several commits. It is reported that way, and no
    commit is named.
- **The 8.6 pair:** a step at `22a8f2cb` that `eef6f089` takes back is 8.6's reverted behaviour, not main's cost.
- **Recv wall steps** are recorded as seen. They are the D-series' own reading (OPTIMIZATION_PLAN 11), not this
  question.

**Bisect result (2026-09-27 10:02-10:12, the Pi, 16 rounds, alternating order; raw:
`results/core_cost_pi_bisect_2026-09-27.txt`; 8.8's own run is `results/core_cost_pi_socket_2026-09-27.txt`).**
All 208 rows delivered 400000 of 400000.

Send wall, in ns per sample. The first column is paired against `fd57b01d`; the step is paired against the arm
before it. Send user+sys is against `fd57b01d`.

| arm | send wall | step | send user+sys |
|---|---|---|---|
| `fd57b01d^0` (control) | +4.5 +- 3.3 | +4.5 +- 3.3 | +9.0 +- 6.4 |
| `22a8f2cb` 8.6 | +7.1 +- 3.8 | +2.6 +- 2.4 | +16.2 +- 6.0 |
| `397d927c` LIVELINESS 10 | +18.4 +- 4.3 | +11.3 +- 3.5 | +12.7 +- 8.4 |
| `4dc7ad49` 8.1a | -1.2 +- 5.9 | **-19.6 +- 6.3** | -8.7 +- 20.8 |
| `5f460ad2` D1 | +21.2 +- 5.6 | +22.4 +- 9.0 | +33.4 +- 23.2 |
| `45cc3299` D3 | +17.1 +- 6.4 | -4.1 +- 6.6 | -3.4 +- 13.9 |
| `3015e15d` LIVELINESS 11.1 | +60.0 +- 4.7 | **+42.9 +- 7.6** | +58.2 +- 6.9 |
| `c51ac8c2` trace points | +55.7 +- 5.9 | -4.3 +- 5.8 | +54.1 +- 10.4 |
| `eef6f089` 8.6 reverted | +25.4 +- 5.4 | **-30.4 +- 5.1** | +19.1 +- 15.0 |
| `6f019b1d` D5 | +0.5 +- 4.9 | **-24.9 +- 4.8** | -19.3 +- 13.0 |
| `10999967` D4 | +18.6 +- 5.8 | **+18.2 +- 5.9** | -2.0 +- 13.7 |
| `86492b99` (control, same tree) | +20.2 +- 5.9 | +1.5 +- 5.6 | -0.4 +- 18.2 |

Steps in bold are real by the rule: at least 3 x SE and at least 12 ns.

How it reads, by the rules above:
- **Not void.** Both send-wall controls are within 2 x SE: +4.5 +- 3.3 and +1.5 +- 5.6. (The first control's recv
  wall is -4.9 +- 1.7, which is 2.9 x SE. So recv steps under ~5 ns are not to be read, and none is.)
- **Reproduced, only just.** `10999967` is +18.6 +- 5.8 against `fd57b01d`: at least 12 ns, and 3.2 x SE.
- **Five real steps, in both directions, so no single commit is named.**
  - The real steps: `4dc7ad49` -19.6, `3015e15d` +42.9, `eef6f089` -30.4, `6f019b1d` -24.9, `10999967` +18.2.
  - Their sum is -13.8, against a total of +18.6. The rest of the total sits in steps below the bar: `397d927c`
    +11.3 (3.2 x SE, but under 12 ns) and `5f460ad2` +22.4 (2.5 x SE).
- **The 8.6 pair clause does not apply.** `22a8f2cb` adds +2.6 and `eef6f089` takes away 30.4, so the revert is not
  undoing 8.6's own step.
- **Recorded as seen, with no mechanism claimed:**
  - `3015e15d`'s rise is also real CPU: its send user+sys is +58.2 +- 6.9 at that commit. `6f019b1d` (D5, the send
    gate written against the summary skip's send cost) brings the user+sys back to -19.3 +- 13.0.
  - Main's send user+sys against `fd57b01d` is flat: -2.0 +- 13.7.
  - `eef6f089` removes fields from `tt_Node` and its step is -30.4 with no matching rise at `22a8f2cb`. This is the
    same kind of layout-shaped move the 8.9 layout test found: +1.7% client CPU from reordering fields alone.
  - Receive: main is -82.5 +- 2.8 against `fd57b01d`. The real steps are `4dc7ad49` -34.7, `5f460ad2` -25.9 and
    `10999967` -33.3; `6f019b1d` is +9.7.
- **What it leaves:** main's +18.6 of send wall in this loop, with its CPU flat. It is spread over several steps of
  both signs, some of them layout-shaped, and the 8.9 campaign A B B A has no CPU or rate row WORSE at main. By
  8.3 it stays a bench-loop observation, not a regression.

### 8.9 The pre-v10 parent against `main`, A B B A: no CPU, rate, byte or latency row WORSE (2026-09-27)

`campaign_ab_chain.sh`, `8f3811f4` / `173268a6` / `173268a6` / `8f3811f4`. `main` at `173268a6` carries v10 with its
receive fix, the summary skip, D1, D3, D4, D5 and the ride-ahead. 3 repetitions per block, 72 + 72 ok rows, 0 VOID.
Raw rows are `results/wire_final_abba_{A_8f3811f4,B_173268a6}_2026-09-27.txt`. Read by 8.3: 38 better, 66 held,
4 WORSE.

- **CPU per sample: WORSE nowhere.**
  - The server is better at c5, c6, c7, c8 and c9 (-1.4 to -2.4%, t -4 to -24).
  - The client is better at c2 (-0.9%) and held everywhere else. That includes the reliable p1 cells c1 (-0.1%) and
    c9 (-0.1%), where 8.4 found v10 alone +0.7-0.8%. On `main`, the D-series more than covers it.
- **Send rate:** better at c2, c3, c4, c6 and c7 (+0.4 to +3.6%), held elsewhere.
- **Wire bytes:** better in every cell.
- **Latency:** every latency cell held (p1 0.209 -> 0.208 ms).
- **The 4 WORSE rows are all peak RSS, +6 to +12 KB (+0.3-0.5%):** the client at c4 and c9, the server at c5 and c8.
  - That is 4 of 21 peak-RSS rows, so they are chance candidates under 8.3, not confirmed.
  - The client's peak RSS leans upward in 11 of 12 cells (+0.0 to +0.5%), however, so a small real rise is
    plausible. It is checked statically (binary size, `sizeof(struct tt_Node)`) and by a targeted re-run before it
    is called either way.
- **Standing against section 1:**
  - On the user's literal reading ("every test better than before the wire change"), `main` meets it on CPU, rate,
    bytes and latency. The one open item is the peak-RSS lean.
  - On Plan's reading, which the user chose at 08:00 ("결정 1은 너의 추천을 따를게"), the wire change is judged
    alone. There v10's reliable client cost on the Pi (8.4) stays open until 8.8's real-socket run explains it.

**8.9 follow-up: the peak-RSS lean, a layout test (pre-registered 2026-09-27, before the run).**
- **Static facts (Dev, x86 release, build.sh's flags):**
  - `tickle.c` text grew 66,627 -> 70,267 B (+3.6 KB).
  - `sizeof(struct tt_Node)` grew 73,712 -> 73,792 B (+80 B), from ten new fields in the middle of the struct.
  - No new data anywhere. The client's static bss fell by 32.8 KB (the smaller reliable record).
  - The campaign's client and server keep their node as a 73 KB stack local, so the +80 B moves every later field:
    `rx_buffer`, `hal` and the counters.
- **Arm B, branch `layout-new-fields-at-end` (`48329585`):** `173268a6` with those ten fields moved to the end of
  `tt_Node`. Every earlier field is back at its `8f3811f4` offset (`rx_buffer` 17568, `hal` 23600,
  `tx_datagrams` 73640). Behaviour is unchanged; unit tests are green. Arm A is `173268a6` as it is.
- **The run:** campaign cells c1 and c9, TickLE only, 6 reps per arm, A B B A.
- **Reading:**
  - **B's client peak RSS below A's beyond 2 x SE, by ~4 KB or more:** the lean is the node's field layout (a hot
    structure crossing into another page). The fix is to place the new fields where they cost no page, and the
    placement is re-measured.
  - **No difference:** the lean is the +3.6 KB of code, or something outside the node. It stays open and is not
    worth more rig time at +0.3-0.5%.
  - **Guard:** CPU and rate rows must not differ between A and B beyond 2 x SE, since the arms differ only in layout.
    A CPU difference is recorded, not explained away.

**Result (Plan's run, 2026-09-27 09:41-10:03).** Campaign cells c1 and c9, 6 reps per arm, A B B A. Arm A
`173268a6`, arm B `48329585`. Raw files: `results/layout_abba_{A_173268a6,B_48329585}_2026-09-27.txt`.
`ab_compare.py campaign`: 2 better, 12 held, 6 WORSE.

| cell | row | A | B | change | t |
|---|---|---|---|---|---|
| c1 | client peak RSS KB | 1912.7 | 1912.7 | 0.0% | 0.0 |
| c9 | client peak RSS KB | 1682.0 | 1674.0 | -0.5% | -2.2 (better) |
| c1 | server peak RSS KB | 1823.3 | 1818.7 | -0.3% | -1.5 |
| c9 | server peak RSS KB | 1828.7 | 1822.7 | -0.3% | -2.6 (better) |
| c1 | client CPU s/Msample | 5.323 | 5.415 | +1.7% | +5.9 (WORSE) |
| c1 | client send Mbps | 114.36 | 111.87 | -2.2% | -3.2 (WORSE) |
| c9 | client CPU s/Msample | 5.195 | 5.222 | +0.5% | +3.9 (WORSE) |
| c9 | client send Mbps | 117.18 | 116.58 | -0.5% | -4.1 (WORSE) |

Server CPU is held in both cells.

**How it reads, by the rule above:**
- **The guard fails.** With the new fields moved to the end, the client's CPU rises, in both cells, beyond
  2 x SE. The arms differ only in layout, so the comparison is confounded, and arm B is not a candidate change.
- **Today's order is the better one for CPU.**
- **The RSS lean moves only at c9, by 6-8 KB, and not at c1.** Layout does not cleanly explain it.
- **The lean stays open:** at most ~8 KB (+0.5%), layout-sensitive, and not worth CPU to remove.
- **`48329585` stays a never-merge branch.**

## 9. W1 on paper (Dev, 2026-09-27; no code until the user's ruling on section 1)

**What it can save.**
- The DATA header today is `endpoint_id`, `seq_no`, `timestamp` and `entity_id`, 16 B. A 2-byte writer handle in
  place of the two ids gives 10 B.
- The payload must start 4-aligned (the CDR-4 constraint of section 6.1), so the header rounds to 12 B, and the
  saving is **4 B per DATA and FRAG_FIRST, not 6**:
  - p1: 146 -> ~142 B per sample, about -2.7%;
  - p2-p4: -0.1 to -0.3%.
- On section 5's reading this moves no latency test and not p1 throughput (CPU-bound). At p4 (link-bound) it is
  worth ~0.2% of rate.

**Routing before the writer's list: a self-describing long form.**
- A DATA may still carry today's two ids, plus the handle, for 20 B. A reader maps (source node, handle) -> (endpoint,
  writer) from the first long form it sees, or from the writer's discovery list, which would carry each writer's
  handle.
- Once mapped, the short form routes with a direct table lookup, replacing today's hash probe on `endpoint_id` and
  the linear writer-proxy search. That is a hypothesis for a small receive-CPU gain, to be measured with
  `core_cost_bench.c` before anything is claimed.
- When the writer sends the long form:
  - **RELIABLE:** to a reader until that reader's first ACKNACK. The per-reader state exists already (`tt_PeerAck`).
    A retransmission always goes long, so a reader that missed the mapping loses nothing: it NACKs and gets the
    sample back in the long form.
  - **BEST_EFFORT:** there are no acks. It uses the long form for a window after each new peer match, and on every
    Nth sample thereafter (N = 16 costs 0.25 B per sample on average). A reader that lacks the mapping drops short
    forms, counts them, and requests the writer's list. That request already exists (v8's discovery request).
  - A writer broadcasting to readers it does not know is the hard case: a reader whose announce was lost. Today such
    a reader matches from the first DATA. With W1 it waits up to N samples or one list round trip.
- **Where it could regress:**
  - the first-sample latency of a BEST_EFFORT late joiner;
  - join tests under loss (M-type), where the announce that would flip the writer to the long form is lost;
  - the receive path gains a second form and a mapping table, plus per-reader "mapped" state on the writer.

**Recommendation.**
- Under the user's literal condition ("every test better"), W1 does not qualify, for the same reason section 5
  gives for all format candidates: it moves bytes, not the bound of any test. Its only plausible CPU gain is the
  routing lookup, and that is a hypothesis.
- Under Plan's reading (targets improve, nothing regresses), it is worth doing only if:
  1. the core bench shows the direct lookup is at least as cheap as today's route, so no CPU regression can come
     with it; and
  2. the BEST_EFFORT join cases are measured with the Nth-sample long form against today, on the M-type join tests
     at 0 and 5% loss.
- Order if approved: the bench prototype of the lookup first (PC, an hour). Pre-register it here before code.

### 9.1 W1 pre-registered (2026-09-27, before code)

**Why now.** The user's decision 2 held W1 until v10's reliable-sender question was closed. 8.8 closed it: the
client's +40 ns is not on the send path, and the core alone shows no rise. Plan asked for W1 in the order of the
recommendation above: the bench first, then the join cases, then the rig.

**The prototype, on a branch (`w1-writer-handle`). No main, and no `tt_VERSION` bump until Plan has read the
bench.**
- **Writer handle.** Each local Publisher gets a 16-bit handle when it is created, unique on its node.
- **Long form:** today's DATA header (`endpoint_id`, `seq_no`, `timestamp`, `entity_id`) plus the handle and 2 B of
  padding, 20 B, as a new submessage type. It routes as today.
- **Short form:** the handle, 2 B of flags (zero), `seq_no` and `timestamp`, 12 B. It saves 4 B per DATA against
  today's 16. FRAG_FIRST is out of the prototype: p1 does not fragment, and the bench runs p1.
- **Receiver route table.**
  - One per node: `tt_RX_ROUTE_SIZE` (64) entries, direct-mapped on (source, handle).
  - Each entry keeps a tag, the Subscriber, the writer proxy, `endpoint_id`, `entity_id`, and the node's
    `route_generation` when the entry was filled.
  - A long form routes as today and fills the entry, but only when exactly one local Subscriber takes it and it
    passed RxO.
  - A short form is one lookup, one tag compare and one generation compare, then the same delivery code as today
    (ordering, reliable tracking, reorder), handed the cached proxy.
- **What moves the generation:** an endpoint created or destroyed, a discovered entity added, changed or removed,
  and a writer proxy claimed or reclaimed. Those are the only events that can change which Subscriber, proxy or
  RxO verdict a (source, handle) means. An announce that changes nothing does not move it.
- **A short form that misses** (no entry, wrong tag, stale generation, or several local Subscribers) is dropped
  and counted (`short_unrouted`), and the node asks the source for its writer list. That request exists since v8.
- **When the writer sends which form:**
  - **RELIABLE:** long to a reader until that reader's first ACKNACK (`tt_PeerAck` has the state). A
    retransmission always goes long.
  - **BEST_EFFORT:** long for 16 samples after each new peer match, and every 16th sample after that. That
    averages 0.25 B per sample back.

**Step 1 - the bench (PC, `core_cost_bench.c`, `core_cost_ab.sh` paired rounds, 20 rounds x 300000 samples).**
- **Arms:** `main` at the branch point against the branch.
- **Cases**, ns per received sample, plus send per sample:

  | case | bench flags |
  |---|---|
  | 1 writer | `-w 1` |
  | 1 writer, discovery on | `-w 1 -D` |
  | 8 writers | `-w 8` |
  | 8 writers, discovery on | `-w 8 -D` |
  | 32 writers | `-w 32` (build below) |
  | 32 writers, discovery on | `-w 32 -D` (build below) |
  | RELIABLE, 1 writer | `-R` |
  | RELIABLE client loop | `-c -R` |

- **The 32-writer cases.** Both arms are built with `-Dtt_MAX_PEER_COUNT=32 -Dtt_MAX_DISCOVERED_ENTITIES=64
  -DBENCH_MAX_WRITERS=32`, because today's defaults (8 proxies, 16 discovered entities) cannot hold 32 writers.
  `core_cost_ab.sh` gains `BENCH_CFLAGS` for this, applied to both arms alike.
- **PASS** needs all three:
  1. **Recv not WORSE at 1 writer, with or without discovery.** That is the case D2 lost (+2.35 +- 0.19 ns,
     OPTIMIZATION_PLAN 11.3). WORSE means beyond 2 x SE.
  2. No other case WORSE, recv or send. The writer's long/short choice is send-side work.
  3. The short form's bytes: each DATA 4 B smaller in the capture.
- **What each outcome means:**
  - Better at 8 and 32 writers, or with discovery on, and not WORSE at 1: the hypothesis holds. On to step 2.
  - Nothing better, nothing WORSE: W1 is bytes only, as section 5 predicts. Plan decides whether -2.7% of p1 bytes
    is worth the second form and the table.
  - WORSE anywhere: FAIL. Reverted, and recorded like D2.
- **Unit tests on the branch, before the bench is read.** Each is shown to fail against a mutant that skips the
  check it pins:
  - a stale generation never routes;
  - a tag mismatch never routes;
  - a short form with no route is dropped and counted, never delivered to the wrong Subscriber;
  - two local Subscribers of one topic both still receive every sample;
  - a RELIABLE reader that missed the mapping NACKs, and gets the sample back in the long form.

**Step 2 - the BEST_EFFORT join cases (only after step 1 passes).**
- **A late joiner:**
  - a BE reader created while a writer is already publishing, at 10 Hz and at maximum rate, at 0% and 5% loss;
  - the metric is the time from the reader's creation to its first delivered sample, W1 against the branch point.
- **The unknown reader:**
  - the same, with the reader's first announce dropped, so the writer does not know it;
  - the reader lives on the every-16th long form or on its list request, whichever comes first;
  - this is the case section 9 names as W1's hard one.
- **PASS:** the first-sample time is not WORSE beyond 2 x SE in either case, at either loss.
  - If the unknown-reader case is WORSE by the list round trip, that is W1's price, and it is stated, not waved
    away.
  - Plan decides with the number in hand.
- PC first, in a private netns with `tc netem` loss. Then the M-type rig cells.

**9.1 amended before code (2026-09-27, Plan's reading of 9.1).**

**1. A table miss never drops a routable sample.**
- The route table is a cache. A direct-mapped table evicts on a collision: two writers whose (source, handle) share
  a slot. As first written, a known writer's short form could then miss and be dropped, a loss today's code does
  not have.
- **Now:** every long form also records (source, handle) -> (`endpoint_id`, `entity_id`) in a handle directory.
  - The directory holds `tt_RX_HANDLE_DIRECTORY` entries (64), is searched linearly, and is kept per node.
  - A writer's entry is replaced when a later long form from the same (source, handle) carries different ids,
    e.g. a restarted writer.
  - An entry is dropped when its source node departs.
- **On a route-table miss:**
  1. the directory is searched;
  2. if it knows the writer, the sample goes through today's full route (endpoint probe, RxO, proxy search), the
     route entry is refilled, and the sample is delivered;
  3. only a (source, handle) the directory does not know is dropped and counted (`short_unrouted`), and triggers
     the list request.
- **Slot index:** `(source ^ (handle << 3)) & (tt_RX_ROUTE_SIZE - 1)`. Writers 1-32, each with handle 0, then take
  distinct slots at the default 64.
- **The collision cost on record.** An added bench case, `-w 32` built with `-Dtt_RX_ROUTE_SIZE=16`, puts two
  writers on every slot, so each sample misses and takes the slow path. It is recorded, not judged: it is the
  worst case, and the default table does not produce it at 32 writers. Both arms are built with the same flags.
- **Added unit tests, each with a mutant that drops on a miss:**
  - two writers forced into one slot both deliver every sample;
  - a short form whose route entry was evicted is still delivered, through the directory.

**2. The BEST_EFFORT unknown reader: the pass line, and the fallback it is judged with.**
- **The problem.** As first designed, a reader whose first announce was lost drops up to 15 short forms before
  the next long form: 1.5 s at 10 Hz. Today it loses none. By the user's rule that is a WORSE test, unless a
  fallback prevents it.
- **Step 2's pass line:** delivered samples and first-sample time are not WORSE beyond 2 x SE than the branch
  point, in every case: late joiner and unknown reader, 10 Hz and maximum rate, 0% and 5% loss. **Otherwise W1
  fails as designed,** and is recorded and not merged.
- **The fallback, designed now rather than after the result:**
  - **Writer.** It sends the long form to a reader until it has seen that reader acknowledge the mapping.
    - A RELIABLE reader's first ACKNACK is the acknowledgement, as before.
    - A BEST_EFFORT reader acknowledges in its next announce or summary. That carries the (source, handle) pairs
      it has mapped since the last one, as the v8 discovery ack already carries seen generations.
    - While any matched reader is unacknowledged, every sample the writer broadcasts goes long.
  - **Reader.** Its first short form from an unknown (source, handle) sends the list request at once, not at the
    summary cadence. The writer's list answer carries each writer's handle.
    - Up to `tt_W1_HOLD` (8) unroutable short forms are held per node while the request is out.
    - They are delivered in arrival order once the mapping arrives, from the answer or a long form, and dropped
      after `tt_DISCOVERY_REQUEST_RETRY` if it does not.
  - **What remains, stated in advance.** A reader the writer has never heard of, sent short forms faster than
    one list round trip can map it: more than 8 samples within one round trip.
    - At 10 Hz that window holds no second sample, so nothing is lost.
    - At maximum rate it will hold more than 8. **The expected result there is WORSE on delivered samples, and
      W1 then fails as designed.** It is not to be explained away.
    - The only way through would be a larger hold, which costs memory an embedded node does not have for this.
      That would be a new proposal, pre-registered on its own.

**9.1 amended again before code (2026-09-27): the long form is today's DATA, and the handle travels in the
announce.**
- **Why.** A 20-B long form stored in the reliable cache, which must keep the long form so that retransmits go
  long, breaks every place that assumes a 16-B DataHeader:
  - the cache's CDR length;
  - `send_tail_as_fragments()`;
  - the FRAG_FIRST copy;
  - `tt_FRAG_DATA_HEADER_LENGTH`.
  That is a larger change than the question the bench asks.
- **Now:**
  - The **long form is today's DATA**, 16 B, routable without any mapping. The encoder, the reliable cache,
    retransmits and fragments are unchanged, so a retransmission is long by construction.
  - The **writer's handle travels in its announce entry.** `tt_UpdateEntity` gains a 16-bit `handle`, which is
    how 9's "or from the writer's discovery list" route works. The handle directory is filled from announces and
    list answers, not from DATA.
  - The **short form** (12 B: handle, flags, `seq_no`, `timestamp`) is made at send time. A datagram that goes
    out in the single-submessage form, and holds exactly one user DATA whose writer chose short, has its 16
    bytes before the payload rewritten in place. It is then sent from 8 bytes further in. Cached copies are
    taken before this, so they stay long.
  - The route table, the directory's miss path, the generation, and when the writer chooses short are as
    amended above.
- **The handle costs the announce no bytes (checked 2026-09-27, the struct as it stands).**
  - The layout of `tt_UpdateEntity` (packed):

    | field | bytes |
    |---|---|
    | `endpoint_id` | 4 |
    | `entity_id` | 4 |
    | `kind` | 1 |
    | `qos` | 1 |
    | `tracking_words` | 2 |
    | `deadline_duration_ns` | 8 |
    | `liveliness_lease_duration_ns` | 8 |

    Then come the type and name strings.
  - `tracking_words` is a Subscriber's field: its RELIABLE window. A Publisher entry always announces 0 there, and
    `decode_update_entities()` reads it only for `tt_KIND_TOPIC_SUBSCRIBER`.
  - A Publisher entry carries its handle in those same 16 bits, so no entry, announce or list answer grows.
  - DISCOVERY_PLAN M1 (join bytes) and M2 (steady state) therefore cannot move from this. Step 3 records them
    anyway, as a control: equal to the branch point, byte for byte.

- **Bytes improve.** A long sample costs 16 B instead of 20. BEST_EFFORT after its first 16 samples then saves
  4 x 15/16 = 3.75 B per sample, against 3.5 as first designed.
- **What it costs.** A reader that has not processed the writer's announce cannot map a short form until the
  next announce or a list answer. That is the unknown-reader case, whose pass line and fallback are above.
- **Step 1 is unchanged:** its cases, and what counts as PASS.

**9.1 finding before any number (2026-09-27): the unknown-reader case occurs in an ordinary startup race, so W1
cannot ship without the ack fallback.**
- **What was seen.** The branch prototype, without the fallback, fails `test_thread_safety`: 2 nodes, 8
  publisher threads, BEST_EFFORT, no loss injected. The reader shows seq gaps, 1 to 3 per run.
- **The counters from three runs:**
  - reader `short_unrouted` 31, 19 and 15;
  - `short_routed` ~74,850 and `short_slow` 4-5, of ~74,900 short forms sent.
- **The cause.**
  - A writer goes short `tt_W1_LONG_EVERY` samples after it matched the reader, which happens when the writer
    processed the reader's announce.
  - The reader may not yet have processed the writer's announce, which carries the handles.
  - The two announces cross independently, so this happens at every startup, not only under loss.
- **What follows.**
  - The ack fallback amended above is required, not an extra: the writer goes long until each reader has
    acknowledged the mapping.
  - Without it, W1 is a delivery regression.
- **The order, as agreed with Plan:**
  1. Step 1's bench now, on the prototype without the fallback. It is the cheap kill gate: recv WORSE at 1
     writer ends W1. Every run prints `short_unrouted`, which must be 0; the bench finishes discovery before
     its timed phases.
  2. If it passes, the fallback is built. `test_thread_safety` must show 0 gaps over 20 runs, with the build
     without the fallback as the control that shows gaps.
  3. **The step-1 bench is then re-run on the fallback build, and that run is the one judged for send.** The
     fallback puts per-reader ack state on the send path, so the first run's send figures cannot stand for the
     final design.

**Step 3 - bytes (deterministic).**
- p1's `wire_bytes_per_sample` falls by 4 B per DATA, less the long forms' share. For BE after the first 16
  samples, that is 4 x 15/16 = 3.75 B.
- It must show in the rig rows exactly as computed; an SE of 0 makes any other value a finding.

**The rig run (only after steps 1-2 pass, and with Plan's reading of the bench).**
- The campaign cells A B B A, W1 against its parent, read by 8.3:
  - a pooled WORSE is a candidate;
  - it is confirmed only by the same sign in every cell of its kind;
  - a confirmed WORSE on any row fails W1.
- The rows W1 is for: bytes at p1-p4, and receive CPU at the 8-writer cells if step 1 shows it.
- Every other row must be held or better.

### 9.2 W1 step 1 result (bench, 2026-09-27): FAIL - recv WORSE at 1 writer, and almost everywhere else

**Run.**
- `w1_bench.sh` over `core_cost_ab.sh`, 20 paired rounds x 300000 samples, CPU 15.
- Branch point `0a099426` against the prototype `889cc995` (branch `w1-writer-handle`, without the ack fallback).
- Raw rows: `results/w1_bench_*_2026-09-27.txt`. Reader: `w1_bench_read.py`.
- Every W1 row has `short_unrouted=0` and delivered 300000 of 300000.

W1 minus base, ns per sample, mean +- SE:

| case | base recv | recv | send |
|---|---:|---:|---:|
| 1 writer | 50.9 | **+12.28 +- 0.11 WORSE** | **+2.66 +- 1.12 WORSE** |
| 1 writer, discovery | 53.4 | **+10.31 +- 0.21 WORSE** | **+2.98 +- 1.10 WORSE** |
| 8 writers | 52.4 | **+10.46 +- 0.22 WORSE** | +0.23 +- 0.90 held |
| 8 writers, discovery | 56.1 | **+6.80 +- 0.26 WORSE** | **+1.52 +- 0.66 WORSE** |
| 32 writers | 54.8 | **+6.24 +- 0.21 WORSE** | **+2.77 +- 0.52 WORSE** |
| 32 writers, discovery | 62.1 | -0.53 +- 0.30 held | **+2.81 +- 0.57 WORSE** |
| RELIABLE `-R` | 81.1 | -0.25 +- 0.19 held | +2.10 +- 2.88 held |
| RELIABLE client `-c -R` | 75.1 | -0.30 +- 0.21 held | -1.38 +- 1.04 held |
| 32 writers, table of 16 (collisions, recorded not judged) | 55.5 | +20.22 +- 1.52 | +3.19 +- 1.33 |

**Bytes.** Exactly as computed: 96.00 -> 92.25 B per sample for BEST_EFFORT, which is 3.75 B, 4 x 15/16.

**The verdict, by the rule written before the run: FAIL.**
- Recv is WORSE at 1 writer, the case D2 lost, and by five times D2's margin (+12.3 against +2.35).
- It is also WORSE at 8 and 32 writers, and send is WORSE in most cases.
- The prototype is not merged. The branch stays as the record, like D2's.
- The ack fallback is not built: the cheap gate has closed.

**Recorded as seen.**
- **The route lookup costs more than the route it replaces, even at 32 writers.** Only 32 writers with discovery
  on, today's slowest route, comes out held (-0.5).
- **RELIABLE never went short in the bench** (`short_sent=0`). A lossless RELIABLE reader never ACKNACKs, so "long
  until the reader's first ACKNACK" leaves such a stream long for good.
  - Its rows therefore measure only W1's overhead on a long stream: recv held, send held.
  - As designed, W1 would save RELIABLE nothing without loss.

**The mechanism of the recv cost: narrowed, not identified.**
- **Not alignment.** A diagnostic arm, `1d16d3ec`, never to be merged, pads the short header to 16 B, which puts
  the CDR back at 4 mod 8 as today's DATA has it. It is just as WORSE: recv +9.58 +- 1.86 at 1 writer and
  +6.21 +- 0.28 at 8 writers with discovery. The prototype in the same run: +10.30 and +6.45.
  Raw rows: `results/w1_bench_diag_aligned_*`.
- **Not the shared delivery code.** The RELIABLE rows carry no short forms, and their recv is held. So the
  prototype's changes to `deliver_data_to_subscriber()` and the larger context cost the long path nothing.
- **Therefore:** the cost is in the short form's own handler, `process_data_short()`, about 10 ns more than the
  endpoint probe, RxO check and proxy search it skips at 1 writer.
- **The profiler was not available here** (`perf_event_paranoid` 4, and no `sudo` for perf), so where inside the
  handler those ~40 cycles go is not known. Nothing is claimed about it.

**What this closes.**
- W1 as designed does not pass.
- Its bytes (-2.7% at p1) would come with a receive CPU cost at every writer count, measured here, and with a
  startup-race delivery loss that needs a fallback on the send path (9.1 finding).
- A new form of W1 would be a new proposal, pre-registered on its own. It would have to explain the handler cost
  first.

### 9.3 W5's ceiling, estimated before any code (2026-09-27): about 3% of join bytes on a real ROS 2 graph - dropped (Plan, 2026-09-27)

**What W5 would save.** A type name already written earlier in the same list would be replaced by a 2-byte
reference. Per repeat, that saves the string's own `2 + len + 1` bytes, less the 2 of the reference. An entity
entry is `28 + (2 + type + 1) + (2 + name + 1)` bytes with no padding (`tt_encode_string()`). A list datagram
carries ~24 B of preamble (single header, DataHeader, announce header).

**Where the formats come from.** A real list was captured from `pong_node` in a private netns. It is 156 B: the
preamble plus two entities with `type = "rmw_perf_pingpong/msg/Bench"`, `name = "/pong"` and `"/ping"`.

**The default ROS 2 node was computed, not captured.** `ros2 topic pub` could not start under rmw_tickle in this
install: "no rmw_tickle typesupport" for rosout's `rcl_interfaces/msg/Log`. Its entities are rclcpp's defaults:
- `/rosout`;
- `/parameter_events`, published and subscribed;
- the six parameter services and `get_type_description`;
- one application topic.

| list | entities | list bytes | repeated types | saving | of the list |
|---|---:|---:|---:|---:|---:|
| native campaign, throughput client | 1 | 73 | 0 | 0 B | 0% |
| native campaign, latency node (ping + pong) | 2 | 120 | 1 | 11 B | 9.2% |
| rmw pingpong node (captured) | 2 | 156 | 1 | 28 B | 17.9% |
| default rclcpp node + 1 app topic | 11 | 998 | 1 (`ParameterEvent`) | 34 B | 3.4% |
| default rclcpp node + 4 app topics of one type | 14 | 1,241 | 4 | 118 B | 9.5% |
| synthetic M1/M3 tool, E = 4 | 4 | 300 | 3 | 33 B | 11.0% |
| synthetic M1/M3 tool, E = 32 | 32 | 2,232 | 31 | 341 B | 15.3% |

**How it reads.**
- **Join bytes are more than the list.** A join is a summary (~75 B on the wire), a request, and the list plus its
  42 B of Ethernet/IP/UDP headers. That puts the default ROS 2 node at about **2.9% of join bytes** (34 of
  ~1,185), and the rmw pingpong node at ~8% of ~345 B.
- **In absolute terms, W5 saves tens of bytes once per join or change.** Since v8 no list is sent in steady state,
  so M2 cannot move.
- **M3 join time cannot move measurably.** Even 341 B at link speed is under 3 us, against M3's 1.8 ms median.
- **No list changes its datagram count.** The 2,232-B list needs two parts before and after (1,891 B).
- **The 15% is the synthetic tool's own shape.** It gives every endpoint the same 10-character type, so a type
  repeats E - 1 times. Real graphs repeat a type rarely: `ParameterEvent` for its publisher and subscription, and
  application topics that share a message type.

**Checked afterwards on a real default node (2026-09-27, lyrical, rmw_tickle with the interface packages built
by `build_ros2_interfaces.sh`).** A default-option rclcpp talker announces 10 entities: `/rosout`, the six
parameter services, `/parameter_events` (publisher only; no subscription on this distro),
`get_type_description` and `/chatter`. No type repeats, so W5 would save it **0 B**. The computed row above
assumed a `/parameter_events` subscription as well.

**Recommendation: drop W5.** Its ceiling on real graphs is about 3% of join bytes, tens of bytes per join, with no
effect on packet count or join time. Building it would add a second string form to the discovery decoder, which
every node runs, to save that. This follows the rule Plan set for it: a ceiling under a few % is dropped, not
built.

## 10. Everything since the COMPARISON build, on the rig (2026-09-28, Plan)

`campaign_ab_chain.sh`, A = `6910d840` (the build COMPARISON's A/T/V rows were measured on, 2026-09-27) and
B = `268dd20e`. Between them: the Context/Node restructure (stages 1-3, wire **v11**), the capacity work (4a, 4b) and
the rmw gap fixes g2, g3, g5, g6, g8, g9, g10. 12 cells, 3 repetitions per block, 72 + 72 ok rows, 0 VOID. Then a
**targeted re-run** of every cell that flagged, c1, c5, c9 and c10, 5 repetitions per block, 20 + 20 ok rows, as
section 8.3 rule 2 requires before anything is called WORSE. Raw rows are
`results/restructure_{abba,confirm}_*_2026-09-28.txt`.

Run 1 gave 12 better, 70 held, 26 WORSE. The re-run separates the real from the chance:

| finding | run 1 | re-run (n=10 per arm) | verdict |
|---|---|---|---|
| client CPU per sample, p1 throughput | c1 +0.8%, c5 +1.2%, c9 +0.7% | c1 **+0.7%** (t 4.3), c5 **+0.5%** (t 3.1), c9 **+0.5%** (t 3.1) | **CONFIRMED WORSE** |
| server CPU per sample, c5 (5% loss) | +0.9% | **+0.4%** (t 5.0) | **CONFIRMED WORSE** |
| wire bytes per sample, c10 (p1 latency) | +1.4 B (+0.5%) | **+1.4 B** (+0.5%, t 3.8) | **CONFIRMED WORSE** |
| peak RSS | WORSE in 5 of 12 cells, +6 to +12 KB | c5 +5.2 KB and c10 +6.4 KB WORSE; c1 and c9 held | **CONFIRMED WORSE**, about +5 KB |
| client CPU per sample, c10 | **+11.1%** | +0.8%, t 0.2, **held** | **not confirmed: run-1 noise** |
| c10 round trip | +1.4% | +0.5%, t 1.4, held | not confirmed |
| server CPU per sample, c1 / c9 | -0.4% / -0.9% | **-0.5%** / **-0.8%** | **CONFIRMED BETTER** |

- **The c10 +11% client CPU did not reproduce.** In run 1 it was 141.7 -> 157.5 cpu_s/Msample; in the re-run,
  146.0 -> 147.2 with t 0.2. A latency cell sends 100 samples in 10 s, so its per-sample CPU is a fixed cost divided
  by 100 and swings widely. Nothing is claimed from it, and the same caution applies to any future reading of c10's
  CPU.
- **The wire-bytes rise is wire v11's node entries,** and it is the only wire cost. The throughput cells are
  byte-identical (138.1 / 124.1 at p1), because an announce amortises over about a million samples; the latency cells
  send 100, so +40 B per node per list shows as +1.4 B per sample. Section 1's rule is therefore **not met by v11**:
  it buys a feature (remote nodes in `ros2 node list`, `ros2 param`) at a byte cost that the user approved in advance
  as a feature cost, not as an optimisation. Recorded as such, and not scored as a pass.
- **The client CPU rise, +0.5 to 0.7% at p1 (about 30 ns per sample), has a named hypothesis to test,** in Plan's
  order of suspicion: g10's depth check on every reliable publish (every p1 throughput cell here is RELIABLE);
  g9's local-delivery branch per publish; g8's id state on the send path. Dev tests it with the core bench, `-R`
  against g10's parent, before anything is changed.
- **Consequence for COMPARISON.md:** its A, T and V rows were measured on `6910d840`. Client CPU rows are therefore
  about 0.5-0.7% optimistic at p1 and peak RSS about 5 KB optimistic, and the latency cells' wire bytes 1.4 B
  optimistic. Every verdict against the vendors is unaffected: the margins there are multiples, not percent. The
  table is re-measured once large-message stage 1 lands, since that changes the rmw rows anyway; until then this
  section is its footnote.

### 10.1 The p1 client's +0.5-0.7%: not the system calls (2026-09-28, Plan)

Section 10 confirmed it twice. Four things now exclude a mechanism, and none of them finds one:

1. **Core's own per-sample send path is flat** across `25ac7fe0..268dd20e`, plain and as the client's scheduler-driven
   loop, and g10's depth check costs +0.3 ns (Dev, `results/core_cost_p1_client_cpu_2026-09-28.md`).
2. **g8, g9 and g6 are not in a native build at all.** Their code compiles only where rmw_tickle's CMakeLists defines
   `tt_CONTEXT_ID_CLAIM`, `tt_LOCAL_DELIVERY` and `tt_DISCOVERY_OPTIONS`; `examples/perf_hil/tickle/build.sh` defines
   none of them. Worth remembering before any future native suspicion list.
3. **`struct tt_Context`'s hot fields have not moved.** 73,792 -> 73,992 B, and `tx_buffer` (14,112), `rx_buffer`
   (17,620) and `hal` (23,656) are at identical offsets in both.
4. **The system calls per sample are identical** (`p1_client_syscalls.sh`, `strace -c -f` on the c1 client at both
   commits, about 214,000 samples each; raw in `results/p1_client_syscalls*_2026-09-28.txt`):

| syscall | A per sample | B per sample | difference |
|---|---:|---:|---:|
| sendto | 1.02157 | 1.02160 | +0.00003 |
| recvmmsg | 0.00427 | 0.00428 | +0.00001 |
| ppoll | 0.00422 | 0.00422 | +0.00000 |
| recvfrom | 0.00420 | 0.00421 | +0.00001 |
| all calls | 2.07430 | 2.07439 | +0.00009 |

By this section's pre-registered reading, agreement within 1% means the rise is **not the call count**. What remains
is the cost per call or code placement in the client binary, and separating those needs per-thread `schedstat` at
nanosecond resolution rather than `getrusage`, whose user/kernel split is attributed per tick and so cannot resolve
30 ns per sample (the campaign's own utime/stime rise is +17/+23 ns at c1, each inside its own noise while their sum
is not).

**Recorded and left open.** It is 30 ns on 5,300, no vendor verdict moves (TickLE 5.3 against CycloneDDS 6.9 and
FastDDS 19.0 at that cell), and large-message stage 1 rewrites this path, whose own pass 5 re-measures these cells.
The next campaign should carry per-thread schedstat so the question answers itself.

**Three harness defects found in this run, each of which had made a result look like data:**
- a `pkill -f '<scenario>/server'` pattern that matches nothing, because the real command line is `./server -Q -d 20`
  after a `cd`. The previous arm's server survived into the next one (CLAUDE.md rule 2: a pattern that matches
  nothing). The PID now comes from the launch itself and is checked through `/proc/PID/exe`.
- `d=~/tickle/...` expanded on **this** host, so every remote `cd` failed, silently, with the client's stderr
  discarded. The path is the Pi's own now, and the client's stderr is kept.
- `setsid ... &` inside an ssh command holds that ssh open until the server exits, so the command substitution
  blocked for the server's whole life and the client always ran against a dead server. It needs its own subshell,
  `( setsid ... & )`, which is why the same thing had worked when typed by hand.

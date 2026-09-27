# Wire protocol optimisation

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

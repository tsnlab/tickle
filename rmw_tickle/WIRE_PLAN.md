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

The rmw half (block and the poll sweep, the rmw capture) follows in 8.2.

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

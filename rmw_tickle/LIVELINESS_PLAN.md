# LIVELINESS detection: the lease runs from the last sign of life, on a timer of its own

**User decision (2026-09-26):** "liveliness detection도 계획에 집어넣자." ("Put liveliness detection in
the plan too.") It follows the user's question of the same day: "TickLE의 liveness 기준을 announce 또는
data (어차피 announce도 data의 일종이니까) 로 잡으면 문제가 해결 되지 않나?" - would basing TickLE's
liveness on announce *or* data (an announce being a kind of data anyway) solve the problem?

## 1. The problem, as it stands

`COMPARISON.md` row 46 (LIVELINESS detection) is marked not comparable. §3 items 10 and 14 have the
detail, and `PLAN.md` holds the 3 s cap as a conformance gap. Three separate defects stack:

1. **The lease is anchored on the announce, not on the last sign of life.** Today an entity is declared
   dead when the announce is older than its lease **and** any traffic from its node is older than half
   the lease (`node_entity_alive_locked()`, `tickle.c`). Data only postpones the verdict and never
   restarts the lease. The HIL harness, like the DDS twins, measures from the last *data* sample. In DDS
   that sample *is* the lease refresh, so the two cancel and FastDDS/CycloneDDS land within ~1 ms of the
   lease. In TickLE they are two clocks, and their phase (0 or ~500 ms) shows up as a bimodal spread.
2. **One-second granularity.** `check_liveliness()` runs every `tt_NODE_UPDATE_INTERVAL`, so a death is
   noticed up to a second after the lease ran out. On the rig it was +610 ms.
3. **The 3 s cap.** The node-level sweep (`tt_LIVELINESS_MISS_THRESHOLD` × `tt_NODE_UPDATE_INTERVAL`)
   fires first for any lease above ~3 s. A requested 4 s lease is silently honoured as 3 s.

The user's proposal fixes (1). (2) and (3) need their own changes, so all three are in this plan.

## 2. Design

**Rule 1, the lease runs from the last sign of life** - the DDS semantics:
- **AUTOMATIC:** any datagram from the entity's node (summary/announce, DATA, HEARTBEAT, ACKNACK) refreshes
  the lease of every AUTOMATIC entity on that node. That is DDS's participant-level assertion.
  `traffic_last_seen[node]` already records it; it becomes the anchor instead of a veto.
- **MANUAL_BY_TOPIC** (`tt_UPDATE_QOS_LIVELINESS_MANUAL`): only that writer's own DATA refreshes it (and
  its HEARTBEAT, the writer asserting itself). DATA already carries the writer's `entity_id`, so this
  needs **no wire change**. The earlier note in PLAN.md that it would need a new per-entity activity
  signal predates tt_VERSION 7, where every DATA identifies its writer. Other traffic from the same node
  must not keep a manual writer alive.

**Rule 2, a timer at the expiry, not a 1 s sweep:** the node keeps one scheduled check at the earliest
expiry among the entities it tracks (last sign of life + lease) and re-arms it when that moves. Detection
is then lease + scheduling latency, not lease + up to one interval.

**Rule 3, the node-level verdict does not override a longer lease:** a node is only declared gone as a
whole when every entity it announced is past its own lease, or on its goodbye. The fixed
`tt_LIVELINESS_MISS_THRESHOLD` window stays only as the floor for a node with no leased entities.

**Constraints:**
- Embedded-conscious core (PLAN.md goal 5): no allocation, and at most one per-writer timestamp beyond
  what writer proxies already hold.
- One scheduler entry per node, not per entity.
- The summary cadence (`DISCOVERY_PLAN.md`) and its liveliness role are unchanged.

## 3. Owner and order

Dev implements, after the discovery M3/M5 results are in; Plan measures. Rules 1-3 are the contract the
tests check, and where the code disagrees with the design, Dev says so first.

## 4. Pre-registered measurements (before implementation)

**L1, unit tests with mutants (Dev):**
- AUTOMATIC stays alive on data alone with summaries dropped;
- MANUAL does not stay alive on another topic's data;
- a 4 s lease is not cut at 3 s;
- the verdict lands within a scheduler tick of the expiry.
Each is killed by its mutant: the old anchor, a missing MANUAL filter, the node sweep winning, the 1 s
sweep.

**L2, the rig** (`liveliness_loss_detection`, `kill -9` mid-stream, lease 1.0 / 2.0 / 4.0 s, n = 20 per
lease, the DDS twins in the same session). Pass if:
- median `detect - lease` is within 20 ms for every lease, 4.0 s included;
- no rep is more than 50 ms from the median (the bimodal ~500 ms mode is gone);
- the TickLE column measures the same quantity as the DDS columns, so row 46 can be scored.

**L3, no false deaths:** a 2 s lease, data at 10 Hz, 5% netem loss, 120 s: 0 departures.

**Control:** L2 is also run on today's core in the same session, and must reproduce today's figures
(bimodal spread, 4 s → ~3 s). Otherwise the harness has changed and L2 is not read.

## 5. Amendments from Dev's review (2026-09-26, before implementation)

Dev read the plan against the code and raised seven points. Plan accepted all of them; the rules above
are read with these amendments.

1. **An idle node's summary must outpace its shortest lease.** Since v8 an idle node's only sign of life
   is the 1 s summary, so a 1 s AUTOMATIC lease with no data would flap. The summary interval becomes
   min(`tt_NODE_UPDATE_INTERVAL`, shortest lease among the node's own entities / 3), as a DDS participant
   asserts at a fraction of the lease. At default leases this changes nothing, and a summary is ~28 B.
   **L3 gains an idle case:** 1 s lease, no data, 5% loss, 120 s, 0 departures.
2. **rmw's lease floor goes.** `rmw_qos.c` rejects leases below `tt_LIVELINESS_MISS_THRESHOLD` ×
   `tt_NODE_UPDATE_INTERVAL` (3 s), a floor that exists only because of defect 3. With rules 1-3 and
   point 1 it drops to about 2 × `tt_NODE_TX_INTERVAL` or goes away, so ROS users actually get the fix.
3. **rmw's own re-scan goes.** `check_subscription_liveliness()` re-scans every lease, which would make
   rmw-level detection up to two leases even with an exact core verdict. rmw updates the status and
   wakes from the core's discovery callback, in both directions.
4. **MANUAL_BY_TOPIC is refreshed by (source node, endpoint_id)**, since `tt_DiscoveredEntity` has no
   entity_id and DATA carries endpoint_id. That adds one `last_asserted` per discovered entity.
   - Two writers of the same endpoint on one node share that clock. This is a known approximation,
     stated here.
   - The lookup on the receive path is gated on a per-source count of MANUAL entities, so
     AUTOMATIC-only traffic pays nothing. Dev measures the gated cost on the PC ping-pong before
     claiming it.
   - **`assert_liveliness()` must reach the wire.** It sends a HEARTBEAT from that writer with a new
     `tt_HEARTBEAT_FLAG_LIVELINESS` bit, as RTPS does, rate-limited to one per lease/3, for best-effort
     writers too. **Plan's decision:** that is a new meaning for an existing submessage, so tt_VERSION
     goes 8 → 9, by the one-version rule. v8 is hours old and deployed nowhere, so the bump costs
     nothing.
5. **Timer:** one scheduler entry per node at the earliest expiry. It is not re-armed on refresh, since
   a refresh only moves an expiry later. When it fires, it tombstones what is past lease and re-arms at
   the next expiry. It is re-armed early only for a new or newly asserted entity with an earlier
   expiry. The receive path does no scheduler work.
6. **Node-level death** = silent for max(`tt_LIVELINESS_MISS_THRESHOLD` × interval, the longest lease
   among its entities), or its goodbye.
7. **L2's control stands as written.** Under rule 1 the last data sample is the AUTOMATIC anchor, so the
   TickLE column measures DDS's quantity.

**L4 added: the rmw level.** Once points 2 and 3 are in, rmw `RMW_EVENT_LIVELINESS_CHANGED` latency
through `rclcpp` at a 1 s and a 4 s lease. Pass if it is within 20 ms of the core verdict (L2) for both.

## 6. The node-level floor is 3.5 intervals of silence (2026-09-26, `b90a047d`)

The M5 dip (`DISCOVERY_PLAN.md` §7) turned out to be a liveliness boundary, not discovery:
- The node-level dead limit was exactly 3 intervals of silence. After two lost summaries, the third
  arrives exactly 3 s after the last one heard.
- Every node's periodic tasks run slightly late and drift at their own rate. So an observer's 1 s
  check and the sender's third summary cross back and forth, and the verdict became a coin toss per
  observer.
- v7 hid it: a 32-endpoint announce was two datagrams, and both refreshed liveliness.

Dev reproduced it (12 of 40 trials with 100/170 us scheduler lateness and two lost summaries, 0 of 40
without drift). The limit is now `tt_LIVELINESS_SILENCE_NS` = 3.5 intervals, which is three missed
summaries (0 of 20 under drift; the old-limit mutant gives 6 of 20).

**Rule 6 therefore reads:** node-level death = silent for max(`tt_LIVELINESS_SILENCE_NS`, the longest
lease among its entities), or its goodbye. Under the 1 s sweep, node-level detection moves from 3-4 s to
3.5-4.5 s. Rule 2's timer removes that sweep artefact, making it 3.5 s after the last sign of life.

## 7. A node's lease is capped, as a DDS participant's is (2026-09-26, Plan's decision overnight)

Dev asked whether rule 3 should be capped: as written, a node stays alive for the longest lease any of its
entities announced, so a core user announcing 1 h would keep a dead node for an hour. In DDS the
participant lease governs the participant whatever its writers' leases are, and a writer with a longer
lease still disappears when its participant expires. TickLE follows that:

node limit = max(`tt_LIVELINESS_SILENCE_NS`, min(longest announced lease, `tt_NODE_MAX_LEASE_NS`)),
where `tt_NODE_MAX_LEASE_NS` is a `config.h` default of 10 s (CycloneDDS's participant lease; Fast DDS uses
20 s), which a build can override.

rmw maps `RMW_DURATION_INFINITE` to no lease (Dev, in implementation), so ROS users are unaffected. **L1
gains a test:** entity lease 30 s, node silent → gone at 10 s plus a tick. The mutant without the cap
keeps it 30 s.

## 8. L3 result: fails once in the idle case (2026-09-26, `af87e150` against `ccbacb36`)

`liveliness_l3.sh`, veth on the PC, 5% loss both ways, 3 × 120 s per case per core. Raw rows:
`results/liveliness_l3_2026-09-26.txt`.

| case | before (`ccbacb36`) | after (`af87e150`) |
|---|---|---|
| data: 2 s lease, 10 Hz | 3/3 held | 3/3 held |
| idle: 1 s lease, no data | **3/3 false deaths** (last announce ~1,999 ms old) | 2/3 held, **1 false death** (silence 1,000.0 ms) |

- The control works: today's core flaps on a 1 s lease with only summaries, as amendment 1 predicted.
- **The change fails L3's pre-registered 0.** Summaries go out at lease/3, so three consecutive lost summaries
  exceed the lease. That is 0.05³ = 1.25 × 10⁻⁴ per summary; over ~360 summaries in 120 s it is ~4.5% per run.
  One false death in three runs is consistent with that.
- The verdict landed at exactly 1,000.0 ms of silence, so rule 2's timer is doing its job. The shortfall is
  the summary cadence, not the detection.

**Fix (Plan's decision):** a node's summary interval becomes min(`tt_NODE_UPDATE_INTERVAL`, shortest own lease
/ 5). Five consecutive losses are then needed: 0.05⁵ ≈ 3 × 10⁻⁷ per summary, ~2 × 10⁻⁴ per 120 s run. A summary
is ~28 B, so at a 1 s lease that is 5 per second, ~150 B/s. L3 is re-run on the fix with more repetitions
of the idle case (10 × 120 s), and must show 0.

## 9. L3 passes on the lease/6 cadence (2026-09-27, `39570d7a` against `af87e150`)

Dev set the divisor to 6, not 5, and the arithmetic was his. With n summaries lost in a row, the peer hears
nothing for (n + 1)/k of the lease, so k - 1 losses already sit on the boundary and k = 6 is what "five losses
in a row" needs. It is `tt_LIVELINESS_LEASE_DIVISOR` in `config.h`. It also covers `tt_Publisher_assert_liveliness()`'s rate
limit and rmw's lease floor (6 ms).

Idle case re-run: 1 s lease, no data, 5% loss both ways, 10 × 120 s. `results/liveliness_l3r_2026-09-26.txt`.

| core | false deaths |
|---|---:|
| `af87e150` (summaries at lease/3), the control | 6 of 10, each at 1,000.0-1,000.1 ms of silence |
| `39570d7a` (summaries at lease/6) | **0 of 10** |

**L3 passes** (the data case passed in section 8, and the change does not touch it). L2 on the rig is still to
run. Its first two attempts were void because of harness faults, not the core; see `RMW_PERF_PLAN.md` and
the `liveliness_l2.sh` history.

## 10. L2 on the rig: 4 s passes, 1 s and 2 s miss the 20 ms bar (2026-09-27)

`liveliness_l2.sh`, before `ccbacb36`, after `48a87342`, CycloneDDS and FastDDS, lease 1/2/4 s, 20 repetitions, the four
arms interleaved. 240 rows, 0 void; the TickLE server watched only the client's node (`-N 3`), and no PC node took part
(`results/liveliness_l2_2026-09-27.txt`). Two earlier attempts were void, from a harness PID bug and then PC test nodes
reaching the Pis over the management LAN.

Median of (detect - lease), measured from the last data sample received, as all three harnesses measure:

| arm | 1 s | 2 s | 4 s | reps > 50 ms from the median |
|---|---:|---:|---:|---|
| CycloneDDS | +0.1 ms | +0.1 | +0.1 | 0 |
| FastDDS | -0.9 ms | -0.9 | -0.9 | 0 |
| TickLE before (control) | +215 | +284 | **-763** (cut to ~3.2 s) | 11 / 9 / 15 of 20 |
| **TickLE after** | **+33.6** | **+31.4** | **+0.2** | 0 / 0 / 0 |

- **The control reproduces today's figures:** a wide spread, and 4 s cut short.
- **After the change:**
  - 4 s is no longer cut, and no repetition strays.
  - **1 s and 2 s miss the pre-registered "within 20 ms".**

**Why:** TickLE is exact against its own anchor. At 1 s and 2 s, the time from the last announce to the verdict minus the
lease is 0.1 ms in the median: the verdict lands exactly one lease after the last sign of life. But under rule 1 any
datagram is a sign of life. A short lease makes the node send summaries often (every 167 ms at 1 s), so the last datagram
before the kill is often a summary ~30 ms after the last data sample, and the harness measures from the data. At 4 s the
summaries are 667 ms apart and the last datagram is almost always data, hence +0.2.

**Remedy (Plan's decision; the criterion is unchanged):**
- The extra summaries that exist only for a short lease (the lease/6 cadence below `tt_NODE_UPDATE_INTERVAL`) are skipped
  while the node has sent any datagram within that cadence. That datagram already asserted liveliness.
- The 1 s summary that discovery needs is always sent.
- Under traffic, the last sign of life is then the data, as in DDS, and fewer packets go out.
- L2 is re-run with the same criteria. L3's idle case is unaffected (no data means every summary goes), and its data
  case is re-run too.

**Implemented (Dev, 2026-09-27).** "Sent any datagram" had to become "reached every peer": a datagram addressed to
one peer asserts nothing at another, whose lease would then run on the 1 s summary alone.
- Each send records who it reaches (`note_reached()`): a broadcast, everyone; an addressed send, its peers (a bit
  per node id). `node_update()` skips a short-lease summary only when every known peer is in that record, then
  clears it. A node that knows no peer never skips.
- A datagram holding only a summary is not counted, or each summary would cancel the next and halve an idle
  node's cadence. The first build did exactly that, and the existing idle-lease test caught it.
- The 1 s summary goes whenever skipping it would leave two more than `tt_NODE_UPDATE_INTERVAL` apart.
- New counter `tt_Node.summaries_skipped`.
- Tests: 100 ms data with a 1 s lease sends 9-11 summaries in 10 s, not ~60, with at least 45 skipped and no
  lapse; idle afterwards, at least 17 in 3 s and no lapse; a peer not addressed keeps its summaries (whitebox,
  three nodes); the MANUAL test now also asserts that summaries were skipped while its lease lapsed on time.
  Mutants caught: always send; a summary alone counts as traffic; the 1 s summary skipped too; an addressed
  send counted as reaching everyone.
- **Residual, stated in advance:** the 1 s summary still goes under traffic, so once a second the last datagram
  before a kill can be a summary up to one data period after the last sample.

## 11. L2 on the summary skip: passes as written, but 2 s is two modes (2026-09-27, `4dc7ad49` against `ccbacb36`)

`liveliness_l2.sh`, 20 repetitions, the four arms interleaved in every rep. All 240 rows are ok, and no row
names a foreign node. The after arm is `4dc7ad49`: the summary skip of section 10 (`397d927c`), plus
WIRE_PLAN 8.1a's receive fix, which changes no summary behaviour. Raw rows are
`results/liveliness_l2d_2026-09-27.txt`. Residual = detect - lease, in ms:

| arm | lease 1 s: median, max \|rep - median\| | 2 s | 4 s |
|---|---|---|---|
| TickLE after | **+0.1, 0.1** | **+15.6, 18.1** | **+0.1, 0.1** |
| TickLE before (control) | +302.4, 125.4 | +182.6, 393.3 | -709.0, 472.4 |
| CycloneDDS | +0.1, 93.9 (one rep +94) | +0.1, 0.0 | +0.1, 0.0 |
| FastDDS | -0.9, 78.4 (one rep +77) | -0.9, 0.0 | -0.9, 0.0 |

- **The control reproduces today's figures:** bimodal at 1 and 2 s, and 4 s cut to ~3.3 s. So the after arm is read.
- **By section 4's criteria the after arm passes at every lease:**
  - the median is within 20 ms and no rep is more than 50 ms from it;
  - 4 s is not cut.
  - 1 s and 4 s are now as tight as CycloneDDS: every rep within 0.2 ms of the lease.
- **2 s passes only through the median.** Its 20 reps are two modes of 10 each, +0.1..0.2 and +31.0..33.7. The median
  falls between them. This is the residual stated in advance in section 10:
  - the 1 s summary still goes under traffic;
  - in half the reps the last datagram before the kill was that summary, ~32 ms after the last sample;
  - the lease then ran from the summary, not from the sample.
  It is not scored as a pass in COMPARISON.md: against both vendors' +0.1 / -0.9 ms, a mode at +32 ms is a loss.
- **Next (Plan's decision):** Dev's prepared change lets the 1 s summary ride in the node's next data datagram under
  traffic, rather than go alone. Then the last datagram before a kill is always data. L2 is re-run with the same
  criteria plus one more, fixed now, before that run:
  - no mode: every rep within 5 ms of the median at every lease.
  L3's data1 case is re-run with it.

### 11.1 The ride-ahead change (Dev, 2026-09-27)

**What it does.**
- When the 1 s summary falls due while the node's traffic reaches every peer, it no longer goes at its tick. It goes
  just ahead of the node's next send: its own datagram, broadcast, built apart from tx_buffer
  (`send_summary_ahead()`).
- A node that stops under traffic therefore stops on its data.
- With no send by the next tick there was no traffic, and the summary goes on its own as before.
- It is gated like the skip (`summary_skip_armed`), so a node without short leases pays nothing. New counter
  `tt_Node.summaries_ridden`.

**Why a datagram of its own, not a ride inside the data datagram.** The summary is broadcast-only content, and the
data may be unicast to some peers. So it goes microseconds ahead of the data instead of inside it. The last
datagram before a kill is then the data in every case.

**Tests.**
- A kill 95 ms after the last sample, at ten phases across the 1 s cycle: every lapse comes one lease after the last
  DATA. Without the ride, 1 phase in 10 is late, which is the residual above, reproduced.
- Three nodes (whitebox): a peer that no data goes to still receives every summary.
- Mutants:
  - a rider never sent fails the summary count;
  - the ride sent at the tick fails the kill-phase test.

## 12. L3 on the summary skip: no false departure (2026-09-27, `397d927c` against `af87e150`)

`liveliness_l3.sh`, PC veth, 5% loss in both directions, 10 repetitions x 120 s per case. The cases:
- data: lease 2 s, 10 Hz;
- data1 (new): lease 1 s, 10 Hz, where the skip applies;
- idle: lease 1 s, no data.

The control is the lease/3 core (`af87e150`), which section 8 showed can fail the idle case. Each row carries
`client_tx_pps`, the client veth's packets per second, as the arm's identity. Raw rows are
`results/liveliness_l3s_2026-09-27.txt`.

| case | after: departures, client_tx_pps | before (control): departures, client_tx_pps |
|---|---|---|
| data | **0/10**, 12.35 | 0/10, 12.26 |
| data1 | **0/10**, 12.03 | 0/10, 13.87 |
| idle | **0/10**, 6.53 | **4/10**, 3.38 (1.93 in the four runs that departed) |

- **Pass:** the after core has no false departure in any case, and the control fails idle 4 times in 10, so the test can fail.
- **The skip is visible in data1.** The after client sends 1.8 fewer packets per second: its short-lease summaries give
  way to the data. Idle does not skip (6.5/s = the lease/6 cadence).
- **The ride-ahead change (section 11.1, `3015e15d`)** was re-run the same way (data1 and idle, 10 x 120 s) against
  `397d927c`: **0/20 departures** in either arm. client_tx_pps is 12.05 against 12.01 in data1 and 6.56 against 6.55
  idle. That is as expected: the 1 s summary still goes as its own datagram, now just ahead of the next send, so only
  its timing changes, not the count. Raw rows are `results/liveliness_l3t_2026-09-27.txt`. Its L2 on the rig (the
  no-mode criterion of section 11) follows the rig queue.

## 13. L2 on the ride-ahead change: every rep within 0.2 ms of the lease, at every lease (2026-09-27, `0f220ba0` against `ccbacb36`)

`liveliness_l2.sh`, 20 repetitions, the four arms interleaved in every rep, 240 ok rows, no foreign node. Raw rows are
`results/liveliness_l2e_2026-09-27.txt`. Residual = detect - lease, in ms: median, max |rep - median| (range):

| arm | lease 1 s | 2 s | 4 s |
|---|---|---|---|
| TickLE after | **+0.1, 0.1** (+0.1..+0.2) | **+0.1, 0.1** (+0.0..+0.2) | **+0.1, 0.1** (+0.1..+0.2) |
| TickLE before (control) | +286.8, 68.2 | +289.5, 471.0 | -708.2, 412.4 |
| CycloneDDS | +0.1, 94.1 (one rep +94) | +0.1, 93.6 (one rep +94) | +0.1, 92.7 (one rep +93) |
| FastDDS | -0.9, 78.4 (one rep +78) | -0.9, 0.0 | -0.9, 0.0 |

- **The control reproduces**, so the after arm is read.
- **PASS on section 4's criteria and on section 11's added no-mode criterion** (every rep within 5 ms of the median):
  all 60 TickLE reps lie within 0.2 ms of their lease.
- **The two modes of section 11 are gone at 2 s.** With the 1 s summary sent just ahead of the next data datagram,
  the last datagram before a kill is always data.
- **In this session TickLE is the tightest of the three:** CycloneDDS has one rep ~+94 ms late at every lease, and
  FastDDS one +78 ms at 1 s.
- **Next:** COMPARISON.md row 46 is re-scored from these figures.

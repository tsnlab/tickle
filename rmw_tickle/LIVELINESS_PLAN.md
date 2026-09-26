# LIVELINESS detection: the lease runs from the last sign of life, on a timer of its own

**User decision (2026-09-26):** "liveliness detection도 계획에 집어넣자." ("Put liveliness detection in
the plan too.") It follows the user's question of the same day: "TickLE의 liveness 기준을 announce 또는
data (어차피 announce도 data의 일종이니까) 로 잡으면 문제가 해결 되지 않나?" - would basing TickLE's
liveness on announce *or* data (an announce being a kind of data anyway) solve the problem?

## 1. The problem, as it stands

`COMPARISON.MD` row 46 (LIVELINESS detection) is marked not comparable. §3 items 10 and 14 have the
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

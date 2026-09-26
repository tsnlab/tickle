# Discovery: a small periodic summary, with the full list pulled on demand

**User decision (2026-09-26):** "너의 제안대로 announce를 노드 ID와 세대 번호만 담은 작은 요약으로 줄이고,
받는 쪽이 모르는 세대 번호를 보면 그 때 목록을 요청하도록 하면 효율적일 것 같아. 이 것도 계획에 넣고
진행시키자." Translation: shrink the periodic announce to a small summary of node ID and generation
number, and let a receiver that sees a generation it does not know request the list then. Put it in the
plan and proceed.

## 1. Why: what discovery costs today

Since tt_VERSION 7, a node's discovery announce is a DATA sample of the built-in endpoint
`tt_DISCOVERY_ENDPOINT_ID` (`tickle.h`), in the RTPS arrangement. It carries the node's **whole endpoint
list**, and it is **broadcast every `tt_NODE_UPDATE_INTERVAL` (1 s)**, changed or not. Since `641370cd` it
also goes out at once when an endpoint is created, as it already did on destroy.

- **Measured on the rig** (2 nodes, the rmw ping/pong, `results/rmw_sysstamp_2026-09-26/`). Non-data
  traffic is 2.2 packets/s and ~390 B/s for rmw_tickle, against 4.7-16.0 packets/s and 760-1,470 B/s for
  the two DDS implementations. At this scale TickLE's discovery is the cheapest of the three.
- **Scaling, estimated, not measured.** One entity is 28 fixed bytes plus its type and topic names:
  about 100-130 B for a ROS endpoint. 30 processes of 20 endpoints each would broadcast ~66 KB/s, and
  every node would parse ~600 entities per second. DDS in steady state sends only SPDP's small
  participant data (every 3 s in Fast DDS, 30 s in Cyclone), and SEDP's endpoint data once per change.
  At that scale TickLE's steady-state discovery would cost an order of magnitude more.

Section 5's first measurement tests that estimate before any code depends on it.

## 2. Design

The RTPS reliable protocol already has the shape the user asked for, and TickLE already has its
submessages: HEARTBEAT says "my newest is N", ACKNACK asks for what is missing, and DATA carries it.
Discovery uses them on the built-in endpoint:

| role | submessage | addressed | contents |
|---|---|---|---|
| periodic summary (every 1 s) | HEARTBEAT, `endpoint_id = tt_DISCOVERY_ENDPOINT_ID`, `entity_id = tt_DISCOVERY_ENTITY_ID` | broadcast | `first_available_seq_no = last_seq_no = generation` |
| request for the list | ACKNACK on the same endpoint | unicast to the summary's sender | `seq_no = generation` wanted |
| the list | DATA (FRAG_FIRST/FRAG_CONT at entity boundaries when large), exactly today's announce | unicast to the requester | node's full endpoint list |
| on a change | the full DATA, as `641370cd` sends it | broadcast | new generation |

A summary is ~28 bytes on the wire, whatever the node's endpoint count.

**Rules:**
1. **Liveliness:** a summary refreshes the sender's liveliness exactly as an announce does now. The
   lease math and `tt_NODE_UPDATE_INTERVAL` do not change.
2. **Known generation:** a summary whose generation matches the one already applied for that node is
   liveliness only. Nothing is sent back.
3. **Unknown generation, or unknown node:** one ACKNACK to the sender for each such summary. The summary
   cadence itself bounds requests to one per peer per interval. A lost request or reply is simply
   retried on the next summary, so it costs at most one interval, the same bound as today, and nothing
   can storm. (Dev, in implementation: a per-peer "request outstanding" mark would need a timeout to
   clear it after a loss, and that timeout would be the next summary. So there is no mark.)
4. **Answering a request:** the full announce, unicast to the requester. If more requests for the same
   generation arrive than `tt_UNICAST_PEER_THRESHOLD` before the next flush, it is broadcast once instead.
5. **Changes are still pushed:** the full list is broadcast when an endpoint is created or destroyed.
   The pull only covers a receiver that missed it, joined later, or restarted.
6. **First contact:** a node that hears an unknown node's full announce still answers with its own full
   announce, unicast, as `reply_with_own_announce()` does. That keeps start-up at one exchange.
7. **Embedded-conscious core** (PLAN.md goal 5): no allocation. The only new state is two per-node
   fields for rule 4's reply rate; the applied generation per source is already kept
   (`update_generation`).
8. **Wire:** no new submessage types, but HEARTBEAT/ACKNACK on endpoint 0 is a new meaning, and a
   tt_VERSION 7 node would never see a periodic list again. So tt_VERSION goes 7 → 8.

**Not changed:** the discovery table, rmw graph semantics, `tt_NODE_UPDATE_INTERVAL`, liveliness,
DATA_FRAG, and the announce's own encoding.

## 3. Owner and order

- Dev implements, after the running rig series (sysstamp, bpftrace, poll sweep).
- Plan runs section 5 and reports.
- The design above is a proposal for Dev to challenge where the code disagrees. Rules 1-8 are the
  contract the tests check.

## 4. Risks written down before starting

- A node whose summaries arrive but whose full list never does (it answers no requests) looks alive
  but is never matched. Rule 3's re-request on every summary bounds this, and a test must drop the
  replies to prove it.
- A burst of new nodes makes every peer request from each of them. Rule 4's broadcast fallback turns
  N unicasts into one.
- Mixed tt_VERSION 7/8 on one network: version mismatch is already logged per remote node, and
  nothing else is attempted.

## 5. Pre-registered measurements (before implementation)

**M1, the scaling estimate, on the current code.** PC, private netns/veth. N ∈ {2, 8, 16} nodes × E ∈ {4,
32} endpoints each, 30 s steady state, discovery bytes/s received per node. If traffic does not grow
with N × E as section 1 estimates, the motivation is weaker than stated and this plan is reported back to
the user before it continues.

**M2, the same grid after the change. Pass if:**
- at E = 32 and N ≥ 8, bytes/s per node ≤ 1/3 of M1;
- at N = 2 and E = 4, within +10% of M1, so small systems don't pay for it.

**M3, discovery latency.** A node started into a running set learns every peer's endpoints. Median over
10 starts: after ≤ before + 5 ms.

**M4, a change propagates.**
- An endpoint added on one node is known to all within a few ms, via the broadcast list.
- With that broadcast dropped on purpose, it is known within one interval plus a round trip, via the
  pull.

**M5, loss.** With 5% netem loss, every node converges on every change within 2 s: the same as today.

**Controls (mutants the tests must catch):**
- no request on an unknown generation (M4's dropped-broadcast case never converges);
- two requests per summary (requests must equal summaries while a list is missing, and stop once it
  arrives);
- a summary that does not refresh liveliness (peers expire).

**M6, the rig.** The rmw rows are re-run: non-data bytes/s fall from ~390 B/s, and the RTT rows do not
move beyond their spread.

## 6. M1 result, today's code (2026-09-26)

`discovery_scaling/discovery_scaling.sh`, core `41fa005f`, built from a snapshot of that commit, never from
the shared working tree. N idle nodes of E publishers in private netns on one bridge; veth byte counters
over 30 s after a 5 s warm-up; 2 repetitions, 18 rows, 0 void. The repetitions agree within 1%.
`results/discovery_scaling_M1_2026-09-26.txt`. Per node, bytes/s received / sent (rep 1):

| | E = 0 | E = 4 | E = 32 |
|---|---:|---:|---:|
| N = 2 | 122 / 74 | 385 / 350 | 2,393 / 2,344 |
| N = 8 | 560 / 77 | 2,505 / 356 | 16,455 / 2,352 |
| N = 16 | 1,151 / 78 | 5,299 / 364 | **35,213** / 2,364 |

- One announce is ~74 + 71·E bytes, with ROS-length endpoint names.
- Received per node is (N - 1) × one announce, exactly as section 1 estimated: 15 × 2,345 = 35,175
  against 35,213 measured.
- **The premise holds**, so the change proceeds. M2's pass criteria are unchanged: at E = 32 and N ≥ 8,
  at most 1/3 of these; at N = 2 and E = 4, within +10% of 385 B/s received.

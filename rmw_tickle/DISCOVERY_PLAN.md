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
3. **Unknown generation, or unknown node:** one ACKNACK to the sender. At most one outstanding request
   per peer; it is re-sent only on a later summary that still shows an unapplied generation. So a lost
   request or reply costs at most one interval, the same bound as today, and nothing can storm.
4. **Answering a request:** the full announce, unicast to the requester. If more requests for the same
   generation arrive than `tt_UNICAST_PEER_THRESHOLD` before the next flush, it is broadcast once instead.
5. **Changes are still pushed:** the full list is broadcast when an endpoint is created or destroyed.
   The pull only covers a receiver that missed it, joined later, or restarted.
6. **First contact:** a node that hears an unknown node's full announce still answers with its own full
   announce, unicast, as `reply_with_own_announce()` does. That keeps start-up at one exchange.
7. **Embedded-conscious core** (PLAN.md goal 5): no allocation. The only new per-peer state is the
   request-outstanding mark; the applied generation per source is already kept (`update_generation`).
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
- a request per summary instead of one outstanding (request count exceeds one per peer per generation
  per interval);
- a summary that does not refresh liveliness (peers expire).

**M6, the rig.** The rmw rows are re-run: non-data bytes/s fall from ~390 B/s, and the RTT rows do not
move beyond their spread.

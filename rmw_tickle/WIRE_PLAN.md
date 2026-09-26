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

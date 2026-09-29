# Comparing against zenoh-pico: what can be matched, what cannot, and how

**Status: urgent, started 2026-09-29 at the user's request, ahead of the shared-memory work.** Three steps were asked
for: work out how to configure zenoh-pico so it corresponds to `COMPARISON.md`'s cells, build a real test and measure,
then add it to the table. This file is step 1 and the pre-registration for steps 2 and 3.

*(Naming note: `WIRELESS_PLAN.md` refers to the wider zenoh system as "Case A" at the user's request. Here the product
is named, because this is a measured framework in a comparison table alongside FastDDS and CycloneDDS rather than a
design reference. Say so if that was not the intent.)*

## 1. The finding that shapes everything: zenoh-pico does not repair loss

Read from its own source at `d9b4eea`, not from its documentation, and it says so itself in the same words on both
transports:

| file | what it says |
|---|---|
| `src/transport/multicast/rx.c:194` | `// @TODO: amend once reliability is in place. For the time being only` |
| `src/transport/unicast/rx.c:106-107` | `// @TODO: amend once reliability is in place. For the time being only` / `// monotonic SNs are ensured` |

So `Z_RELIABILITY_RELIABLE` in zenoh-pico means **monotonic sequence numbers are ensured** - out-of-order and duplicate
handling - and **not retransmission**. Over UDP, in either transport, a lost sample stays lost.

**This is a capability difference, not a performance one, and the comparison has to say so rather than score it.**
Running our RELIABLE cells against a nominally-RELIABLE zenoh-pico would produce numbers where we look better for paying
for machinery it does not run, and worse under loss where it simply drops what we repair. Neither number would mean what
a reader takes it to mean.

## 2. The three configurations, and which of our cells each can match

| config | transport | router needed | our cells it can match |
|---|---|---|---|
| **zp-mcast** | peer mode, UDP multicast | no | **best-effort cells only.** Both sides genuinely best-effort, both on UDP, no repair on either side - a like-for-like row |
| **zp-tcp** | peer mode, TCP (`Z_FEATURE_LINK_TCP`) | no, if peer-to-peer TCP works as the unicast transport suggests | **reliable cells by outcome, not by mechanism.** TCP supplies the repair, so nothing is lost - but by head-of-line blocking rather than per-datagram repair, which is the trade to report rather than hide |
| zp-router | client mode, TCP to a router | **yes** | **none of ours.** A router hop is a different topology from our direct link; it would be a separate measurement, not a cell |

**zp-mcast is the primary configuration** because it needs no router and shares our transport, and **zp-tcp is the
honest way to show the reliable cells** with its mechanism named in the row.

## 3. The QoS mapping, from the API that exists

zenoh-pico's publisher-side knobs, from `include/`:

| our QoS | zenoh-pico | fidelity |
|---|---|---|
| BEST_EFFORT | `Z_RELIABILITY_BEST_EFFORT` | **exact** - neither repairs |
| RELIABLE | `Z_RELIABILITY_RELIABLE` | **name only over UDP** (section 1). Real only on TCP, and then it is TCP's reliability |
| HISTORY KEEP_ALL + `max_blocking_time` | `Z_CONGESTION_CONTROL_BLOCK` | **close**: block rather than drop when the link is full is the same choice DDS's KEEP_ALL makes |
| HISTORY KEEP_LAST | `Z_CONGESTION_CONTROL_DROP` | **close in spirit**: drop when full. Not a depth - zenoh has no per-writer history depth, so a KEEP_LAST *depth* cell has no counterpart |
| DEADLINE, LIVELINESS, LIFESPAN, DURABILITY | - | **absent.** Our cells 5-9 have no counterpart and are not scored |
| (no equivalent) | `z_priority_t`, 8 levels | ours has no counterpart; left at default so it cannot flatter either side |

Build-time knobs that must be **stated in every row**, because they change what is being measured:
`Z_FEATURE_BATCHING`, `Z_FEATURE_FRAGMENTATION`, `Z_BATCH_MULTICAST_SIZE`, `Z_BATCH_UNICAST_SIZE`, `Z_FRAG_MAX_SIZE`.

## 4. The measurement, mirroring the campaign rather than inventing one

A zenoh-pico harness under `examples/perf_hil/zenohpico/` with the same shape as the other three: `client` publishes,
`server` subscribes, both print a `RESULT:` line through `BenchStats.h` so one parser reads all four frameworks and the
instrument gate applies unchanged. The same payload shapes (p1 64 B, p2 1280 B, p3 1412 B, p4 2788 B), the same
durations, the same two Pis, the same rig lock.

**Pre-registered reading, before any build:**

- **Only best-effort cells are scored in zp-mcast.** A reliable cell in that configuration is reported as **N/A with the
  reason**, never as a win.
- **zp-tcp rows carry their mechanism in the row** - "reliability by TCP, head-of-line blocking" - so a reader cannot
  take them for a like-for-like against per-datagram repair.
- **Under loss, zp-mcast losing samples is not a defeat to report as one.** It is the absence of repair, which section 1
  already establishes; the row states delivered fraction and says the feature is absent.
- **A batching or fragmentation setting that differs between the arms voids the row**, the same rule the campaign
  already applies to QoS.
- **If zenoh-pico wins a cell, that is the result.** Its wire is compact and its batching is real; nothing here is
  arranged to prevent that, and a cell we lose is more useful than one we win.

## 5. Adjacent, and nearly free: `rmw_zenoh_cpp` is already on the rig

`ros-jazzy-rmw-zenoh-cpp` and `ros-jazzy-zenoh-cpp-vendor` are installed on the client Pi. The rmw block (`V` rows) swaps
`RMW_IMPLEMENTATION` against one unchanged binary, so **rmw_zenoh can be added to those rows with no new harness at
all** - a different product from zenoh-pico, and a comparison our existing method already supports. Worth doing
alongside, and it is a separate row set from anything in this file.

## 6. Open, and to be settled by running rather than reading

1. whether peer-to-peer **TCP** works without a router in zenoh-pico (the unicast transport asserts `Z_WHATAMI_PEER`, so
   it looks supported);
2. what zenoh-pico's multicast discovery does on the rig's direct link, where there is no switch doing IGMP;
3. whether `Z_FEATURE_BATCHING` is on by default in a plain build, since batching is exactly where a small-payload
   throughput cell would be decided;
4. its per-sample wire overhead at p1, which is the figure most directly comparable to our 138.6 B.

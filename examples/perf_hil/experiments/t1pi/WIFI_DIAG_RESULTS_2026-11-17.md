# Wi-Fi diagnosis results (phase 1 of WIFI_STRATEGY_2026-11-17.md), 2026-11-17

**Simulation only** (mac80211_hwsim + patched wmediumd on the two rig Pis, T1pi topology; relative figures, never Wi-Fi performance). Diagnosis only: no library, wire, API or default change.
Plan (registered and committed before the first run): `examples/perf_hil/experiments/t1pi/WIFI_DIAG_PLAN.md`; branch `ab/wifi-diag`. Raw data: `~/rig_results_safe/wifi_diag_20261117/`
(`c_drip/`, `b_1m/`, `b_128k/`, `a_grid/`, `a_fresh/`, `a_ctrl_wire/`, `a_ctrl_fix54/`, `a_big/`, `a_prime/`, `table_*`). Every run: Pis' wlan0/eth0/modules snapshot compared after each run (no change, no abort), Pis left with no hwsim and no `t1p_*`.
Runs: (c) 24 TickLE 1 MiB (12 ctl, 12 o200); (b) 36 (1 MiB and 128 KiB x ctl/o60/o200 x TickLE/Fast DDS 2.14.6/CycloneDDS 0.10.5 x 2 rounds); (a) 15 topologies x 39 cells x 5 s (grid), 45 fresh-link cells, 2x15 control cells, 15 x 5 queue-limit cells, 12 priming topologies. Pcap and sampler on every (b)/(c) run.

## The finding that organises everything (read this first)
**The simulated link has two states, chosen by a lottery at bring-up, not by the application:** the STA's mac80211 *minstrel* rate control either holds the lowest CCK rate (**1.0M, "slow"**) or a high HT rate (**MCS8 and up, "fast"**) for the whole run.
* Slow: the uplink carries **80 datagrams/s of 1472 B (about 0.95 Mbit/s of IP bytes) and not more**, no loss but backpressure (the sender blocks). Fast: at least 800 datagrams/s (the grid's top rate) without loss on ctl.
* Which state a fresh topology gets is a coin flip: **72 slow / 85 fast over 157 topologies** (all arms, all frameworks; `table_state.txt`; per arm 40-60%), read from the sampler about 2 s after bring-up.
* The state never changes during a TickLE run (the STA's rate table is not updated by data frames: its attempt counter stays at 2 or 3 for the whole run, in 36 of 36 TickLE runs). It does change in DDS runs: both DDS stacks send **2 extra STA frames at start-up (presumably the multicast group join, IGMP; the priming test below shows a join is sufficient)** that feed minstrel (attempts 2->4, rate re-evaluated). Over the (b)+(c) runs: **TickLE 17 of 36 started slow and 17 of 36 ended slow (0 flips); Fast DDS 7 of 12 started slow and 0 of 12 ended slow (7 of 7 flipped); CycloneDDS 4 of 12 started slow and 2 of 12 ended slow (2 of 4 flipped).**
* **Priming test (a_prime, 12 topologies, ctl):** a multicast join from the STA namespace, with no other traffic, moved **4 of 4 slow links to a fast rate** (MCS8, MCS5, MCS11, MCS12) and the following cells ran at 200 and 400 datagrams/s with no loss. Cause shown for the state change; why data frames do not feed the table is not established (A-MPDU/tx-status handling in hwsim is a hypothesis, untested).
* This explains the "drip" regime, "Fast DDS delivers 15/15 where TickLE delivers 5-6" and the bimodality of every earlier Pi table. **Drip = slow link.**

## (a) Capacity curve
Plain UDP, 1472 B, no TickLE; medians over reps; the full tables are in the files `table_a_*.txt` (columns: steady datagrams/s in the middle arrival bins, range over reps, loss, took/dur = sender time against plan, mean burst fraction arrived, wmediumd CPU % of one core, busiest Pi core %, STA rate).
* **Steady limit: slow 80/s (79-82 in every cell and arm), fast above 800/s (ctl: 800 at B=1,16; 785 at B=64), independent of burst size B (1-64) and of the inter-burst gap** (cells B in {1,4,16,64}, R 20-800). At R above the limit the cell shows loss 0.40-0.43 and took/dur about 2.2 in the slow state: that is the sender blocking (backpressure on the socket buffer), the link itself loses nothing in ctl. Offered below the limit everything arrives (R=20, 40, 80).
* **Frame size:** slow state, bytes not datagrams: 1472 B -> 80/s (120 kB/s), 700 B -> 157/s (115 kB/s, B=1 and B=16, ctl), 100 B -> 400/s offered all delivered (40 kB/s). That is an airtime law of the 1 Mbit/s rate, not a per-datagram cost.
* **Hwsim/wmediumd or Pi CPU?** Not the Pi CPU: wmediumd (pinned to core 2) used 0-4% of one core in every cell (maximum 5% at 800/s), the busiest Pi core was at 93-98% only at B=1, R>=400, which is the python sender itself (single thread), not the link, and the **wire-only control (sender in the AP namespace, bypassing hwsim and wmediumd) carried 200 and 400 datagrams/s with 0-1% loss in both "states"** (`a_ctrl_wire`). The limit is a property of the simulated radio path: the STA's rate state (the airtime of the rate minstrel has chosen). A fixed rate by `iw ... set bitrates legacy-2.4 54` was rejected by the kernel (-22), so that control gave nothing; the priming test is the evidence for the rate.
* **Fresh link (a_fresh, 45 cells):** the same states: slow 80/s (79-80.5), fast 200/s and 375-400/s (ctl, o200), 277-387 (o60/o200 at B=16, R=400: the Gilbert-Elliott bad phases of o60/o200 cost 3-14%).
* **Burst tail-drop in the fast state (a_big, one burst every 2 s, ctl):** delivered frames per burst 99 of 128, 205 of 256, 317 of 512, 469 of 723, 479 of 1024 (o60/o200: 128, 220-234, 321-337, 372-415, 536-538). Bursts up to 91 arrive whole (grid); beyond about 100 frames the queue admits a fraction. A 1 MiB sample sent back to back is 723 units, so about 35% of the first transmission is dropped on any fast link too (also the pcaps of (c): 47-70% of each burst arrives).
* **Slow state burst:** delivered 128 of 128, 219 of 256, 218 of 512 (ctl): the same ceiling of about 220 frames, then backpressure.

## (b) Timeline against DDS
Per-run table (`table_b.md`, 36 rows) below; summary by framework and state class:

| size | fw | class | n | done/seen med | compl p50 ms med | amplification med | burst p50 med | gap us med | busy fps med | NACK interval med s |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 MiB | cyclonedds | fast | 5 | 8/15 | 1086 | 1.86 | 93 | 13 | 200 | 0.20 |
| 1 MiB | cyclonedds | slow | 1 | 0/10 | - | 1.03 | 50 | 9 | 85 | 0.59 |
| 1 MiB | fastdds | fast | 6 | 15/15 | 986 | 2.03 | 80 | 23 | 496 | 0.11 |
| 1 MiB | tickle | fast | 4 | 15/15 | 1032 | 1.70 | 185 | 13 | 384 | 0.11 |
| 1 MiB | tickle | slow | 2 | 6/7 | 15158 | 1.05 | 92 | 12 | 78 | 1.22 |
| 128 KiB | cyclonedds | fast | 5 | 15/15 | 33 | 1.07 | 97 | 8 | 97 | 3.81 |
| 128 KiB | cyclonedds | slow | 1 | 15/15 | 1260 | 1.96 | 88 | 15 | 27 | 1.09 |
| 128 KiB | fastdds | fast | 6 | 15/15 | 174 | 1.88 | 30 | 25 | 78 | 0.60 |
| 128 KiB | tickle | fast | 3 | 15/15 | 2 | 1.13 | 91 | 10 | 78 | 4.00 |
| 128 KiB | tickle | slow | 3 | 15/15 | 1090 | 1.07 | 91 | 10 | 75 | 3.99 |


What the packet timelines show (sender-side pcap on the STA radio, receiver-side pcap on the server tap, clock offset from the first matching units):
* **Fast DDS 2.14.6 does not rate-limit, does not pace and does not send smaller datagrams.** Per frame it is unpaced (gap between frames about 23 us, TickLE 10-13 us), one RTPS DATA_FRAG per Ethernet frame (maxMessageSize 1472 as configured; 810 fragments per MiB against TickLE's 723 units), burst sizes p50 70-90 frames (p90 140-170), TickLE p50 171-185 (p90 290-370, max up to 680): **Fast DDS's bursts are half as large**, because its repair NACK/HEARTBEAT cycle is 0.09-0.11 s (burst-to-burst 0.04-0.06 s) with a send amplification of 1.9-2.4 (TickLE 1.7). No flow control is visible.
* **TickLE in the fast state is equal to Fast DDS at 1 MiB:** 15/15 samples in 12 of 12 fast runs (c) and 4 of 4 (b); completion p50 977-1123 ms against Fast DDS 610-1114 ms; TickLE's repair interval is 0.10-0.14 s (not 1 s: the 1 s repair rounds of the earlier root cause belong to the slow link, where the interval was 1.2 s), amplification 1.7. **The 11-14 root-cause note "rounds are about 1 s apart" is true only in the slow state** (1.17-1.23 s here), where a round of about 96 frames is exactly what 80/s carries in 1.2 s.
* **Slow state, 1 MiB:** a sample is 723-810 frames; 80 frames/s means 9-10 s of link time per sample at a 4 s period and a 68 s run: at most 68 s x 80/s = 5440 frames = 7.5 samples whatever the protocol. TickLE delivered 3-7 of 15 (amplification only 1.05: it is not wasting the link), CycloneDDS 0 of 10 in its one slow run. **No sender can deliver 15/15 in that state;** Fast DDS 15/15 in the same arm happened because its runs were not slow after start-up (see above).
* **128 KiB:** TickLE sends a sample as two large datagrams (hybrid runs, up to 45 IP-fragmented frames), completion p50 2-33 ms in the fast state (Fast DDS 16-174 ms, CycloneDDS -7 to 528 ms; the CycloneDDS negatives are the clock-offset estimate, +-10 ms) and 1.09 s in the slow state (floor 91 frames / 80 per s = 1.14 s; CycloneDDS 1.26 s, Fast DDS 1.28 s at the same slow state). TickLE is at the floor in both states; 2 rounds only, so no WIN claim.
* CycloneDDS at 1 MiB in the fast state completed 5-10 of 15 samples (pcap-complete) in the window: IP fragments of up to 10 frames (units/dg about 2), a fragment lost loses the whole datagram (`dg_nframes_max`).
* Counters: in all TickLE runs the receiver-side pcap count agrees with the node counters within the capture loss that tcpdump may have (not separately measured).

## (c) Drip entry
24 unpaced TickLE 1 MiB runs (12 ctl, 12 o200). Classification as registered: drip = delivered <= 12 of 15; good >= 14. Result: **12 drip (delivered 3-7), 12 good (all 15/15), no mixed.**
The pcap criterion (20 s busy window at 60-100 frames/s with a coefficient of variation < 0.5) agrees with the delivery criterion in **24 of 24** runs (drip: 79-80 frames/s, cv 0.03-0.06; good: mean 195-229 frames/s, cv 1.3-1.7).
**Separating factor: the STA's rate state at the start: 12 of 12 drip runs had attempts 2 and best rate 1.0M; 12 of 12 good runs had attempts 3 and MCS8** (no overlap, no exception; state is fixed before TickLE starts and never changes in the run).
Counters, each on the same table (`table_c.md` below): peer registrations 7-16 in drip runs against 2-7 in good runs, hybrid runs 0-963, `tx_udp_broadcast` 45-53 against 64-70, discovery datagrams in the first 10 s 18-23 against 23-27, retransmitted 220-3094 against 6680-9288. All of these separate the classes but are **consequences**: a link that carries 80 frames/s delays discovery replies and ACKs, so the node re-registers peers 3x as often and sends fewer broadcasts per second of run. The 11-vs-3 re-registrations lead is a symptom. NACK suppression, subscriber window and announce competition were not needed to explain any run (`large_no_buffer`, `large_dropped`, `large_abandoned` 0 in the earlier counters; delivered count in drip runs equals the link capacity bound in the time available).

Per-run table (c):
| arm | run | delivered/15 | STA rate start>end | STA att at start | recv frames/s busy p50 | peak 100 ms (fps) | 20 s mean fps / cv | peer reg | retransmitted | hybrid runs | disc dg first 10 s (c) | tx_udp_bcast | summary_copies |
|---|---|---:|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| ctl | r10 | 7 | 1.0M>1.0M | 2 | 80 | 230 | 80 / 0.05 | 13 | 220 | 963 | 21 | 49 | 53 |
| ctl | r11 | 15 | MCS8>MCS8 | 3 | 112 | 2310 | 218 / 1.58 | 3 | 8706 | 0 | 26 | 69 | 65 |
| ctl | r12 | 7 | 1.0M>1.0M | 2 | 80 | 220 | 80 / 0.06 | 12 | 258 | 0 | 20 | 51 | 52 |
| ctl | r1 | 15 | MCS8>MCS8 | 3 | 695 | 2460 | 212 / 1.61 | 5 | 7832 | 0 | 26 | 69 | 65 |
| ctl | r2 | 6 | 1.0M>1.0M | 2 | 80 | 160 | 80 / 0.03 | 10 | 348 | 0 | 20 | 50 | 52 |
| ctl | r3 | 7 | 1.0M>1.0M | 2 | 80 | 170 | 80 / 0.04 | 7 | 422 | 0 | 21 | 53 | 54 |
| ctl | r4 | 7 | 1.0M>1.0M | 2 | 80 | 160 | 80 / 0.03 | 15 | 412 | 361 | 20 | 50 | 51 |
| ctl | r5 | 15 | MCS8>MCS8 | 3 | 681 | 2430 | 198 / 1.67 | 3 | 7324 | 0 | 26 | 69 | 65 |
| ctl | r6 | 15 | MCS8>MCS8 | 3 | 116 | 2080 | 208 / 1.52 | 5 | 8418 | 0 | 26 | 69 | 65 |
| ctl | r7 | 7 | 1.0M>1.0M | 2 | 80 | 170 | 80 / 0.05 | 16 | 463 | 361 | 23 | 47 | 50 |
| ctl | r8 | 15 | MCS8>MCS8 | 3 | 304 | 1950 | 195 / 1.63 | 5 | 8210 | 0 | 27 | 70 | 65 |
| ctl | r9 | 7 | 1.0M>1.0M | 2 | 80 | 190 | 80 / 0.03 | 14 | 232 | 361 | 20 | 51 | 53 |
| o200 | r10 | 15 | MCS8>MCS8 | 3 | 648 | 2000 | 218 / 1.62 | 4 | 8462 | 0 | 25 | 70 | 65 |
| o200 | r11 | 15 | MCS8>MCS8 | 3 | 662 | 2380 | 202 / 1.56 | 2 | 6729 | 0 | 25 | 69 | 65 |
| o200 | r12 | 15 | MCS8>MCS8 | 3 | 362 | 2310 | 215 / 1.49 | 7 | 9288 | 0 | 25 | 69 | 65 |
| o200 | r1 | 3 | 1.0M>1.0M | 2 | 78 | 160 | 79 / 0.03 | 7 | 3094 | 0 | 19 | 20 | 18 |
| o200 | r2 | 6 | 1.0M>1.0M | 2 | 79 | 230 | 80 / 0.05 | 9 | 281 | 0 | 21 | 52 | 52 |
| o200 | r3 | 15 | MCS8>MCS8 | 3 | 148 | 2460 | 229 / 1.53 | 5 | 8269 | 0 | 24 | 69 | 64 |
| o200 | r4 | 5 | 1.0M>1.0M | 2 | 78 | 170 | 79 / 0.03 | 13 | 342 | 0 | 18 | 45 | 47 |
| o200 | r5 | 15 | MCS8>MCS8 | 3 | 117 | 2090 | 193 / 1.35 | 4 | 6680 | 0 | 23 | 68 | 64 |
| o200 | r6 | 6 | 1.0M>1.0M | 2 | 79 | 170 | 79 / 0.04 | 16 | 277 | 241 | 22 | 50 | 51 |
| o200 | r7 | 15 | MCS8>MCS8 | 3 | 506 | 1920 | 214 / 1.61 | 4 | 8915 | 0 | 24 | 68 | 64 |
| o200 | r8 | 6 | 1.0M>1.0M | 2 | 79 | 190 | 79 / 0.03 | 11 | 434 | 361 | 19 | 47 | 46 |
| o200 | r9 | 15 | MCS8>MCS8 | 3 | 108 | 1960 | 202 / 1.28 | 5 | 7425 | 0 | 24 | 69 | 65 |


## Ranked levers (expected effect from this data; phase 2 is where they are tested)
The data change the picture: the large-sample gap to Fast DDS in the earlier tables is dominated by the simulator's rate lottery, not by TickLE's repair behaviour. In the fast state TickLE is at the link floor like Fast DDS (1 MiB p50 about 1.0 s, 128 KiB p50 2-33 ms); in the slow state no protocol can pass (floor 9-10 s per MiB sample, 1.1 s per 128 KiB).
1. **L0 (new, not in the strategy; harness, not library): make the comparison state-fair.** Either prime every T1pi topology so that it starts fast (the multicast join of the test above, which is what DDS does by itself) or record the STA rate state per run and score slow-state runs separately. Effect: removes the entire bimodality (TickLE 1 MiB delivery 15/15 in 12 of 12 fast runs against 3-7 in slow ones), so the Pi cells would read as TIE/WIN against Fast DDS instead of VOID. No wired metric is touched (harness only). Needs the user's decision, because it changes how earlier tables are read.
2. **L2 adaptive repair-burst limiting** (and **L3 slow start**, same expected effect): fast state only. They would cut send amplification from 1.7 to about 1.1 (the first send of 723 units loses about 35%, and each repair is a burst that loses again): about 40% fewer datagrams and bytes sent on large samples. Latency: not expected to improve, p50 is already at the floor (1.0 s). It could shorten the tails (completion max 2-9 s at o60/o200, p99 against Cyclone is the one PC LOSE). Wired metric at risk: large-sample throughput and CPU per MB (any pacing floor must be neutral on ETHERNET). Evidence strength: medium (amplification and burst loss measured, tail effect not).
3. **L1 repair cadence:** expected effect about zero. In the fast state the repair interval is already 0.10-0.14 s (Fast DDS 0.09-0.11); in the slow state rounds are capacity-bound (a 1.2 s round carries the 96 frames the link passes). The 1 s cadence is a symptom of the slow state, not a limiter. Do not spend phase 2 here first.
4. **L4 drip-state fix in the library:** nothing in the library can raise an 80 frames/s link; TickLE already wastes only 5% there (amplification 1.05). The only effects available are not sending a large sample that cannot finish (delivery policy: user decision) and not re-registering peers as often, which gives no delivery. The state itself is fixed by L0 or by fixing the simulator (frames of the STA not feeding its rate control).
**Recommended first step: L0 (harness) followed by a re-run of the Pi large-sample cells with the primed link, 10 reps in the deciding cells; L2/L3 only if the primed tails still lose.**

## Deviations from the plan, honest
* The plan's grid assumed a stateless link; the state split (slow/fast) was added to the readout (it is read from the sampler, not chosen). Cells in slow topologies at R above 130/s lost the sender (timeout, blocked on the socket): counted as `n_notx` and read from the receiver side only.
* Added after the first chain, before reading it (amendment 1): `a_big` (bursts of 128-1024) and `a_prime` (multicast-join priming, 12 topologies); chain script `t1pi_wdiag_chain2.sh`.
* Burst-edge and completion-time figures come from pcap pairs with a clock offset estimated from the first 20 matching units (error up to ~10 ms); tcpdump drops were not counted. The `fix54` control was rejected by the kernel and gave no information.
* One probe topology was brought up before the plan was written (section 0 of the plan).
* The ">= 5 reps" of (a): grid and fresh cells have 5 reps per arm over topologies, but the slow/fast split leaves 1-4 reps per (arm, state) cell; the plateau 80/s is the same in all of them (min 79, max 82).

## Tables
### State of every topology at its start (set, arm, framework, slow, fast)
```
set arm fw slow fast
a_big ctl udp 1 4
a_big o200 udp 1 4
a_big o60 udp 2 3
a_ctrl_fix54 ctl udp 3 2
a_ctrl_wire ctl udp 2 3
a_fresh ctl udp 9 6
a_fresh o200 udp 7 8
a_fresh o60 udp 9 6
a_grid ctl udp 3 2
a_grid o200 udp 2 3
a_grid o60 udp 1 4
a_prime ctl udp 4 8
b_128k ctl cyclonedds 2 0
b_128k ctl fastdds 1 1
b_128k ctl tickle 1 1
b_128k o200 cyclonedds 0 2
b_128k o200 fastdds 2 0
b_128k o200 tickle 1 1
b_128k o60 cyclonedds 1 1
b_128k o60 fastdds 1 1
b_128k o60 tickle 1 1
b_1m ctl cyclonedds 0 2
b_1m ctl fastdds 1 1
b_1m ctl tickle 0 2
b_1m o200 cyclonedds 1 1
b_1m o200 fastdds 1 1
b_1m o200 tickle 1 1
b_1m o60 cyclonedds 0 2
b_1m o60 fastdds 1 1
b_1m o60 tickle 1 1
c_drip ctl tickle 7 5
c_drip o200 tickle 5 7
TOTAL slow 72 fast 85
```

### Capacity excerpts (`table_a_grid.txt`, 1472 B; full files in the raw directory)
```
arm state size B R n (n_notx) | steady/s [min..max] | del/s | loss | took/dur | frac | wm% | maxcore% | sta rate start->end
ctl  fast 1472   1   20 2(0) |   20.0 [ 20.0.. 20.0] |   20.2 |  0.00 |  0.99 | 1.00 | 0 | 2 | MCS8>MCS8
ctl  fast 1472   1   40 2(0) |   40.0 [ 40.0.. 40.0] |   40.2 |  0.00 |  1.00 | 1.00 | 0 | 3 | MCS8>MCS8
ctl  fast 1472   1   80 2(0) |   80.0 [ 80.0.. 80.0] |   80.2 |  0.00 |  1.00 | 1.00 | 0 | 5 | MCS8>MCS8
ctl  fast 1472   1  120 2(0) |  120.0 [120.0..120.0] |  120.2 |  0.00 |  1.00 | 1.00 | 1 | 11 | MCS8>MCS8
ctl  fast 1472   1  200 2(0) |  200.0 [200.0..200.0] |  200.2 |  0.00 |  1.00 | 1.00 | 1 | 16 | MCS8>MCS8
ctl  fast 1472   1  400 2(0) |  400.0 [400.0..400.0] |  400.2 |  0.00 |  1.00 | 1.00 | 2 | 98 | MCS8>MCS8
ctl  fast 1472   1  800 2(0) |  800.0 [800.0..800.0] |  800.2 |  0.00 |  1.00 | 1.00 | 3 | 98 | MCS8>MCS8
ctl  fast 1472  16   20 2(0) |   15.5 [ 15.0.. 16.0] |   23.6 |  0.02 |  0.83 | 0.98 | 0 | 1 | MCS8>MCS8
ctl  fast 1472  16   40 2(0) |   40.0 [ 32.0.. 48.0] |   43.6 |  0.00 |  0.92 | 1.00 | 0 | 2 | MCS8>MCS8
ctl  fast 1472  16   80 2(0) |   77.5 [ 75.0.. 80.0] |   80.6 |  0.03 |  0.96 | 0.97 | 0 | 2 | MCS8>MCS8
ctl  fast 1472  16  120 2(0) |  117.5 [115.0..120.0] |  123.2 |  0.00 |  0.97 | 1.00 | 1 | 2 | MCS8>MCS8
ctl  fast 1472  16  200 2(0) |  198.0 [192.0..204.0] |  203.3 |  0.00 |  0.98 | 1.00 | 1 | 4 | MCS8>MCS8
ctl  fast 1472  16  400 2(0) |  400.0 [400.0..400.0] |  403.2 |  0.00 |  0.99 | 1.00 | 2 | 3 | MCS8>MCS8
ctl  fast 1472  16  800 2(0) |  800.0 [800.0..800.0] |  803.0 |  0.00 |  1.00 | 1.00 | 4 | 12 | MCS8>MCS8
ctl  fast 1472  91   23 2(0) |   91.0 [ 91.0.. 91.0] | 45169.1 |  0.00 |  0.00 | 1.00 | - | 0 | MCS8>MCS8
ctl  fast 1472  91   91 2(0) |   91.5 [ 91.0.. 92.0] |  113.7 |  0.00 |  0.80 | 1.00 | 1 | 1 | MCS8>MCS8
ctl  fast 1472  91  182 2(0) |  182.0 [182.0..182.0] |  202.2 |  0.00 |  0.90 | 1.00 | 1 | 1 | MCS8>MCS8
ctl  slow 1472   1   20 3(0) |   20.0 [ 20.0.. 20.0] |   20.2 |  0.00 |  0.99 | 1.00 | 0 | 2 | 1.0M>1.0M
ctl  slow 1472   1   40 3(0) |   40.0 [ 40.0.. 40.0] |   40.2 |  0.00 |  1.00 | 1.00 | 0 | 4 | 1.0M>1.0M
ctl  slow 1472   1   80 3(0) |   82.0 [ 82.0.. 82.0] |   80.2 |  0.00 |  1.00 | 1.00 | 1 | 6 | 1.0M>1.0M
ctl  slow 1472   1  120 3(0) |   80.5 [ 80.5.. 81.0] |  103.6 |  0.04 |  1.11 | 1.00 | 0 | 7 | 1.0M>1.0M
ctl  slow 1472   1  200 3(0) |   80.0 [ 80.0.. 80.0] |   53.2 |  0.43 |  2.16 | 1.00 | 1 | 2 | 1.0M>1.0M
ctl  slow 1472   1  400 3(3) |   80.0 [ 80.0.. 80.0] |    0.0 |  0.00 |  0.00 | 1.00 | - | - | None>None
ctl  slow 1472   1  800 3(3) |   80.0 [ 80.0.. 80.0] |    0.0 |  0.00 |  0.00 | 1.00 | - | - | None>None
ctl  slow 1472  16   20 3(0) |   16.0 [ 16.0.. 16.0] |   24.0 |  0.00 |  0.83 | 1.00 | 0 | 1 | 1.0M>1.0M
ctl  slow 1472  16   40 3(0) |   39.0 [ 33.0.. 43.0] |   43.6 |  0.00 |  0.92 | 1.00 | 0 | 1 | 1.0M>1.0M
ctl  slow 1472  16   80 3(0) |   80.0 [ 80.0.. 80.0] |   83.3 |  0.00 |  0.96 | 1.00 | 0 | 2 | 1.0M>1.0M
ctl  slow 1472  16  120 3(0) |   80.0 [ 79.0.. 80.0] |  106.0 |  0.03 |  1.10 | 1.00 | 1 | 2 | 1.0M>1.0M
ctl  slow 1472  16  200 3(0) |   80.0 [ 80.0.. 80.0] |   53.5 |  0.42 |  2.17 | 1.00 | 1 | 1 | 1.0M>1.0M
ctl  slow 1472  16  400 3(3) |   80.0 [ 80.0.. 80.0] |    0.0 |  0.00 |  0.00 | 1.00 | - | - | None>None
ctl  slow 1472  16  800 3(3) |   80.0 [ 80.0.. 80.0] |    0.0 |  0.00 |  0.00 | 0.99 | - | - | None>None
ctl  slow 1472  91   23 3(0) |   45.5 [ 45.5.. 67.0] | 87396.5 |  0.00 |  0.00 | 1.00 | - | 0 | 1.0M>1.0M
ctl  slow 1472  91   91 3(0) |   80.0 [ 25.0.. 80.0] |  113.7 |  0.00 |  0.80 | 1.00 | 0 | 1 | 1.0M>1.0M
ctl  slow 1472  91  182 3(0) |   80.0 [ 80.0.. 80.0] |   62.8 |  0.37 |  1.83 | 0.90 | 1 | 1 | 1.0M>1.0M
o200 fast 1472   1   20 3(0) |   20.0 [ 20.0.. 20.0] |   20.2 |  0.00 |  0.99 | 1.00 | 0 | 2 | MCS8>MCS8
o200 fast 1472   1   40 3(0) |   40.0 [ 40.0.. 40.0] |   40.2 |  0.00 |  1.00 | 1.00 | 0 | 3 | MCS8>MCS8
o200 fast 1472   1   80 3(0) |   80.0 [ 80.0.. 80.0] |   80.2 |  0.00 |  1.00 | 1.00 | 1 | 5 | MCS8>MCS8
o200 fast 1472   1  120 3(0) |   75.0 [ 17.0..120.0] |  115.6 |  0.04 |  1.00 | 1.00 | 1 | 10 | MCS8>MCS8
o200 fast 1472   1  200 3(0) |  184.0 [ 49.0..200.0] |  196.8 |  0.02 |  1.00 | 1.00 | 1 | 15 | MCS8>MCS8
o200 fast 1472   1  400 3(0) |   59.0 [ 24.0..400.0] |  290.3 |  0.27 |  1.00 | 1.00 | 1 | 43 | MCS8>MCS8
o200 fast 1472   1  800 3(0) |  800.0 [ 17.0..800.0] |  800.2 |  0.00 |  1.00 | 1.00 | 3 | 100 | MCS8>MCS8
o200 fast 1472  16   20 3(0) |   16.0 [ 16.0.. 16.0] |   24.0 |  0.00 |  0.83 | 1.00 | 0 | 1 | MCS8>MCS8
o200 fast 1472  16   40 3(0) |   40.0 [ 32.0.. 43.0] |   43.6 |  0.00 |  0.92 | 1.00 | 0 | 2 | MCS8>MCS8
o200 fast 1472  16   80 3(0) |   75.0 [ 66.0.. 80.0] |   81.0 |  0.03 |  0.96 | 0.97 | 0 | 3 | MCS8>MCS8
o200 fast 1472  16  120 3(0) |  116.0 [112.0..117.0] |  123.3 |  0.00 |  0.97 | 1.00 | 1 | 3 | MCS8>MCS8
o200 fast 1472  16  200 3(0) |  183.0 [108.0..206.0] |  189.7 |  0.07 |  0.98 | 0.97 | 1 | 3 | MCS8>MCS8
o200 fast 1472  16  400 3(0) |   85.0 [ 17.0..400.0] |  100.2 |  0.40 |  1.70 | 0.93 | 1 | 2 | MCS8>MCS8
o200 fast 1472  16  800 3(0) |  120.0 [111.0..254.5] |  333.8 |  0.30 |  1.68 | 0.97 | 2 | 4 | MCS8>MCS8
o200 fast 1472  91   23 3(0) |   15.0 [ 15.0.. 91.0] | 17525.7 |  0.84 |  0.00 | 0.16 | - | 0 | MCS8>MCS8
o200 fast 1472  91   91 3(0) |   89.0 [ 13.0.. 91.0] |  113.7 |  0.00 |  0.80 | 1.00 | 0 | 1 | MCS8>MCS8
o200 fast 1472  91  182 3(0) |  133.5 [ 76.5..181.0] |  172.2 |  0.03 |  0.90 | 0.97 | 1 | 1 | MCS8>MCS8
o200 slow 1472   1   20 2(0) |   20.5 [ 20.0.. 21.0] |   19.0 |  0.06 |  0.99 | 1.00 | 0 | 3 | 1.0M>1.0M
o200 slow 1472   1   40 2(0) |   40.0 [ 40.0.. 40.0] |   40.1 |  0.00 |  1.00 | 1.00 | 0 | 4 | 1.0M>1.0M
o200 slow 1472   1   80 2(0) |   79.8 [ 78.5.. 81.0] |   80.2 |  0.00 |  1.00 | 1.00 | 1 | 6 | 1.0M>1.0M
o200 slow 1472   1  120 2(0) |   80.2 [ 80.0.. 80.5] |   84.2 |  0.17 |  1.20 | 1.00 | 1 | 8 | 1.0M>1.0M
o200 slow 1472   1  200 2(0) |   79.8 [ 79.5.. 80.0] |   52.4 |  0.53 |  1.81 | 1.00 | 1 | 6 | 1.0M>1.0M
o200 slow 1472   1  400 2(2) |   78.8 [ 77.5.. 80.0] |    0.0 |  0.00 |  0.00 | 1.00 | - | - | None>None
o200 slow 1472   1  800 2(2) |   79.2 [ 78.5.. 80.0] |    0.0 |  0.00 |  0.00 | 1.00 | - | - | None>None
o200 slow 1472  16   20 2(0) |   16.0 [ 16.0.. 16.0] |   24.0 |  0.00 |  0.83 | 1.00 | 0 | 1 | 1.0M>1.0M
o200 slow 1472  16   40 2(0) |   37.5 [ 36.0.. 39.0] |   43.6 |  0.00 |  0.92 | 1.00 | 0 | 1 | 1.0M>1.0M
o200 slow 1472  16   80 2(0) |   76.8 [ 73.5.. 80.0] |   82.5 |  0.01 |  0.96 | 0.99 | 1 | 2 | 1.0M>1.0M
o200 slow 1472  16  120 2(0) |   79.2 [ 79.0.. 79.5] |   77.2 |  0.20 |  1.30 | 0.97 | 1 | 2 | 1.0M>1.0M
o200 slow 1472  16  200 2(0) |   79.2 [ 79.0.. 79.5] |   52.7 |  0.42 |  2.19 | 0.99 | 1 | 1 | 1.0M>1.0M
o200 slow 1472  16  400 2(2) |   79.0 [ 78.0.. 80.0] |    0.0 |  0.00 |  0.00 | 0.99 | - | - | None>None
o200 slow 1472  16  800 2(2) |   55.5 [ 31.0.. 80.0] |    0.0 |  0.00 |  0.00 | 0.94 | - | - | None>None
o200 slow 1472  91   23 2(0) |   45.5 [ 45.5.. 45.5] | 80979.5 |  0.00 |  0.00 | 1.00 | - | 0 | 1.0M>1.0M
o200 slow 1472  91   91 2(0) |   78.8 [ 78.0.. 79.5] |  108.8 |  0.04 |  0.80 | 0.96 | 0 | 1 | 1.0M>1.0M
o200 slow 1472  91  182 2(0) |   79.5 [ 79.0.. 80.0] |   49.2 |  0.39 |  2.37 | 0.94 | 1 | 1 | 1.0M>1.0M
o60  fast 1472   1   20 4(0) |   20.0 [ 20.0.. 20.0] |   20.2 |  0.00 |  0.99 | 1.00 | 0 | 2 | MCS8>MCS8
o60  fast 1472   1   40 4(0) |   40.0 [ 40.0.. 40.0] |   39.9 |  0.01 |  1.00 | 1.00 | 0 | 3 | MCS8>MCS8
o60  fast 1472   1   80 4(0) |   80.0 [ 64.0.. 80.0] |   79.7 |  0.01 |  1.00 | 1.00 | 1 | 6 | MCS8>MCS8
o60  fast 1472   1  120 4(0) |  120.0 [116.0..120.0] |  120.1 |  0.00 |  1.00 | 1.00 | 1 | 9 | MCS8>MCS8
o60  fast 1472   1  200 4(0) |  200.0 [200.0..200.0] |  200.2 |  0.00 |  1.00 | 1.00 | 1 | 16 | MCS8>MCS8
o60  fast 1472   1  400 4(0) |  400.0 [ 18.0..400.0] |  398.0 |  0.01 |  1.00 | 1.00 | 2 | 82 | MCS8>MCS8
o60  fast 1472   1  800 4(0) |  420.0 [119.5..599.5] |  542.3 |  0.17 |  1.23 | 1.00 | 2 | 54 | MCS8>MCS8
o60  fast 1472  16   20 4(0) |   16.0 [ 16.0.. 16.0] |   24.0 |  0.00 |  0.83 | 1.00 | 0 | 1 | MCS8>MCS8
o60  fast 1472  16   40 4(0) |   35.0 [ 30.0.. 48.0] |   43.5 |  0.00 |  0.92 | 1.00 | 0 | 1 | MCS8>MCS8
o60  fast 1472  16   80 4(0) |   80.0 [ 76.0.. 81.0] |   82.9 |  0.01 |  0.96 | 0.99 | 0 | 2 | MCS8>MCS8
o60  fast 1472  16  120 4(0) |  114.0 [104.0..123.0] |  120.7 |  0.02 |  0.97 | 0.98 | 0 | 3 | MCS8>MCS8
o60  fast 1472  16  200 4(0) |  202.0 [180.0..207.0] |  202.3 |  0.00 |  0.98 | 1.00 | 1 | 4 | MCS8>MCS8
o60  fast 1472  16  400 4(0) |  400.0 [153.0..400.5] |  400.0 |  0.01 |  0.99 | 0.99 | 2 | 4 | MCS8>MCS8
o60  fast 1472  16  800 4(0) |  769.0 [724.0..800.0] |  781.2 |  0.03 |  1.00 | 0.99 | 3 | 9 | MCS8>MCS8
o60  fast 1472  91   23 4(0) |   91.0 [ 28.0.. 91.0] | 89305.3 |  0.00 |  0.00 | 1.00 | - | 0 | MCS8>MCS8
o60  fast 1472  91   91 4(0) |   91.0 [ 50.0.. 91.0] |  113.7 |  0.00 |  0.80 | 1.00 | 0 | 1 | MCS8>MCS8
o60  fast 1472  91  182 4(0) |  182.0 [182.0..182.0] |  202.2 |  0.00 |  0.90 | 1.00 | 1 | 2 | MCS8>MCS8
o60  slow 1472   1   20 1(0) |   20.0 [ 20.0.. 20.0] |   20.2 |  0.00 |  0.99 | 1.00 | 0 | 2 | 1.0M>1.0M
o60  slow 1472   1   40 1(0) |   40.0 [ 40.0.. 40.0] |   40.2 |  0.00 |  1.00 | 1.00 | 0 | 5 | 1.0M>1.0M
o60  slow 1472   1   80 1(0) |   79.0 [ 79.0.. 79.0] |   80.2 |  0.00 |  1.00 | 1.00 | 1 | 6 | 1.0M>1.0M
o60  slow 1472   1  120 1(0) |   79.5 [ 79.5.. 79.5] |   87.6 |  0.06 |  1.29 | 1.00 | 1 | 6 | 1.0M>1.0M
o60  slow 1472   1  200 1(0) |   78.5 [ 78.5.. 78.5] |   50.8 |  0.44 |  2.19 | 1.00 | 1 | 3 | 1.0M>1.0M
o60  slow 1472   1  400 1(1) |   79.0 [ 79.0.. 79.0] |    0.0 |  0.00 |  0.00 | 1.00 | - | - | None>None
o60  slow 1472   1  800 1(1) |   79.0 [ 79.0.. 79.0] |    0.0 |  0.00 |  0.00 | 1.00 | - | - | None>None
o60  slow 1472  16   20 1(0) |   16.0 [ 16.0.. 16.0] |   24.0 |  0.00 |  0.83 | 1.00 | 0 | 2 | 1.0M>1.0M
o60  slow 1472  16   40 1(0) |   33.0 [ 33.0.. 33.0] |   43.6 |  0.00 |  0.92 | 1.00 | 0 | 1 | 1.0M>1.0M
o60  slow 1472  16   80 1(0) |   79.5 [ 79.5.. 79.5] |   83.3 |  0.00 |  0.96 | 1.00 | 1 | 2 | 1.0M>1.0M
o60  slow 1472  16  120 1(0) |   78.0 [ 78.0.. 78.0] |   92.4 |  0.17 |  1.08 | 0.96 | 1 | 2 | 1.0M>1.0M
o60  slow 1472  16  200 1(0) |   80.0 [ 80.0.. 80.0] |   52.0 |  0.43 |  2.21 | 0.96 | 1 | 2 | 1.0M>1.0M
o60  slow 1472  16  400 1(1) |   45.5 [ 45.5.. 45.5] |    0.0 |  0.00 |  0.00 | 0.90 | - | - | None>None
o60  slow 1472  16  800 1(1) |   79.5 [ 79.5.. 79.5] |    0.0 |  0.00 |  0.00 | 0.97 | - | - | None>None
o60  slow 1472  91   23 1(0) |   45.5 [ 45.5.. 45.5] | 94113.1 |  0.00 |  0.00 | 1.00 | - | 0 | 1.0M>1.0M
o60  slow 1472  91   91 1(0) |   80.0 [ 80.0.. 80.0] |  113.7 |  0.00 |  0.80 | 1.00 | 1 | 1 | 1.0M>1.0M
o60  slow 1472  91  182 1(0) |   79.0 [ 79.0.. 79.0] |  107.3 |  0.47 |  0.90 | 0.53 | 1 | 3 | 1.0M>1.0M
```

### Per-run table (b)

| size | arm | fw | run | STA rate | done/seen | compl p50 / max ms | units/sample | frames/dg max | send amplification | burst frames p50/p90/max | intra-burst gap us | busy recv fps p50 | peak 100 ms fps | repair interval s (NACK p50 / burst p50) |
|---|---|---|---|---|---:|---|---:|---:|---:|---|---:|---:|---:|---|
| 1 MiB | ctl | cyclonedds | r1 | fast | 10/15 | 835 / 1090 | 781 | 10 | 1.76 | 93/250/400 | 13 | 704 | 2530 | 0.20 / 0.07 |
| 1 MiB | ctl | cyclonedds | r2 | fast | 8/15 | 1086 / 1273 | 781 | 10 | 1.87 | 93/250/430 | 13 | 605 | 2160 | 0.20 / 0.07 |
| 1 MiB | ctl | fastdds | r1 | fast | 15/15 | 995 / 2667 | 810 | 1 | 2.41 | 82/159/235 | 23 | 982 | 2430 | 0.11 / 0.06 |
| 1 MiB | ctl | fastdds | r2 | fast | 15/15 | 610 / 714 | 810 | 1 | 2.03 | 92/173/226 | 23 | 780 | 3260 | 0.09 / 0.04 |
| 1 MiB | ctl | tickle | r1 | fast | 15/15 | 977 / 2028 | 723 | 1 | 1.71 | 185/297/470 | 13 | 688 | 2220 | 0.13 / 0.12 |
| 1 MiB | ctl | tickle | r2 | fast | 15/15 | 1061 / 2323 | 723 | 1 | 1.69 | 185/307/519 | 13 | 401 | 2420 | 0.10 / 0.10 |
| 1 MiB | o200 | cyclonedds | r1 | fast | 6/15 | 1187 / 4498 | 781 | 10 | 1.91 | 93/211/390 | 13 | 200 | 2330 | 0.18 / 0.07 |
| 1 MiB | o200 | cyclonedds | r2 | slow | 0/10 | - / - | 781 | 10 | 1.03 | 50/80/430 | 9 | 85 | 220 | 0.59 / 0.60 |
| 1 MiB | o200 | fastdds | r1 | fast | 15/15 | 1024 / 4990 | 810 | 1 | 2.05 | 67/156/235 | 23 | 211 | 2160 | 0.11 / 0.06 |
| 1 MiB | o200 | fastdds | r2 | fast | 15/15 | 866 / 5205 | 810 | 1 | 1.92 | 85/157/233 | 23 | 970 | 2330 | 0.11 / 0.06 |
| 1 MiB | o200 | tickle | r1 | fast | 14/15 | 1123 / 2106 | 723 | 1 | 1.68 | 171/292/395 | 13 | 329 | 2150 | 0.10 / 0.11 |
| 1 MiB | o200 | tickle | r2 | slow | 6/7 | 19095 / 27902 | 723 | 1 | 1.06 | 93/119/185 | 12 | 78 | 170 | 1.23 / 1.21 |
| 1 MiB | o60 | cyclonedds | r1 | fast | 5/15 | 1151 / 3960 | 781 | 10 | 1.86 | 93/240/430 | 13 | 148 | 2480 | 0.26 / 0.07 |
| 1 MiB | o60 | cyclonedds | r2 | fast | 9/15 | 952 / 1884 | 781 | 10 | 1.85 | 93/250/430 | 13 | 155 | 2330 | 0.20 / 0.07 |
| 1 MiB | o60 | fastdds | r1 | fast | 15/15 | 1114 / 6843 | 810 | 1 | 2.04 | 78/171/236 | 24 | 76 | 2420 | 0.11 / 0.06 |
| 1 MiB | o60 | fastdds | r2 | fast | 15/15 | 977 / 3831 | 810 | 1 | 1.87 | 71/144/183 | 31 | 93 | 2450 | 0.11 / 0.06 |
| 1 MiB | o60 | tickle | r1 | fast | 15/15 | 1003 / 9071 | 723 | 1 | 1.89 | 185/370/681 | 13 | 367 | 2250 | 0.14 / 0.11 |
| 1 MiB | o60 | tickle | r2 | slow | 6/7 | 11221 / 17949 | 723 | 3 | 1.04 | 92/114/185 | 12 | 79 | 170 | 1.21 / 1.17 |
| 128 KiB | ctl | cyclonedds | r1 | fast | 15/15 | 1021 / 1196 | 98 | 10 | 1.02 | 97/97/97 | 8 | 8 | 160 | 2.81 / 4.00 |
| 128 KiB | ctl | cyclonedds | r2 | fast | 15/15 | -41 / 788 | 98 | 10 | 1.02 | 97/97/97 | 8 | 97 | 970 | 3.87 / 4.00 |
| 128 KiB | ctl | fastdds | r1 | fast | 15/15 | 175 / 208 | 102 | 1 | 1.31 | 9/93/93 | 25 | 108 | 1170 | 0.17 / 0.07 |
| 128 KiB | ctl | fastdds | r2 | fast | 15/15 | 1281 / 1339 | 102 | 1 | 2.39 | 43/93/93 | 25 | 80 | 150 | 0.61 / 0.61 |
| 128 KiB | ctl | tickle | r1 | slow | 15/15 | 1032 / 1691 | 91 | 45 | 1.07 | 91/91/91 | 11 | 76 | 130 | 4.00 / 4.00 |
| 128 KiB | ctl | tickle | r2 | fast | 15/15 | 1 / 235 | 91 | 45 | 1.13 | 91/91/91 | 11 | 78 | 910 | 4.00 / 4.00 |
| 128 KiB | o200 | cyclonedds | r1 | fast | 15/15 | 33 / 208 | 98 | 10 | 1.07 | 97/97/97 | 8 | 97 | 970 | 3.81 / 4.00 |
| 128 KiB | o200 | cyclonedds | r2 | fast | 15/15 | -7 / 209 | 98 | 10 | 1.07 | 97/97/97 | 8 | 97 | 970 | 3.81 / 4.00 |
| 128 KiB | o200 | fastdds | r1 | fast | 15/15 | 173 / 1174 | 102 | 1 | 1.41 | 18/93/93 | 24 | 116 | 930 | 1.30 / 0.07 |
| 128 KiB | o200 | fastdds | r2 | fast | 15/15 | 16 / 1434 | 102 | 1 | 2.37 | 78/102/102 | 23 | 73 | 900 | 0.13 / 0.13 |
| 128 KiB | o200 | tickle | r1 | fast | 15/15 | 33 / 265 | 91 | 45 | 1.10 | 91/91/91 | 10 | 91 | 910 | 4.00 / 4.00 |
| 128 KiB | o200 | tickle | r2 | slow | 15/15 | 1113 / 1714 | 91 | 45 | 1.11 | 91/91/91 | 10 | 75 | 150 | 3.38 / 4.00 |
| 128 KiB | o60 | cyclonedds | r1 | slow | 15/15 | 1260 / 1279 | 98 | 10 | 1.96 | 88/97/97 | 15 | 27 | 140 | 1.09 / 1.08 |
| 128 KiB | o60 | cyclonedds | r2 | fast | 15/15 | 528 / 553 | 98 | 10 | 1.07 | 97/97/97 | 9 | 89 | 1010 | 3.39 / 4.00 |
| 128 KiB | o60 | fastdds | r1 | fast | 15/15 | 1280 / 1331 | 102 | 1 | 2.36 | 46/93/93 | 24 | 77 | 150 | 0.59 / 0.62 |
| 128 KiB | o60 | fastdds | r2 | fast | 15/15 | 123 / 244 | 102 | 1 | 1.32 | 13/93/93 | 26 | 71 | 1020 | 0.71 / 0.07 |
| 128 KiB | o60 | tickle | r1 | slow | 15/15 | 1090 / 1812 | 91 | 45 | 1.05 | 91/91/91 | 10 | 53 | 140 | 3.99 / 4.00 |
| 128 KiB | o60 | tickle | r2 | fast | 15/15 | 2 / 317 | 91 | 45 | 1.13 | 91/91/91 | 10 | 62 | 910 | 4.00 / 4.00 |



# T1 framework comparison: plan and reading rules (registered 2026-11-14, before any measured run)

**Simulation only.** T1 = `mac80211_hwsim` + patched `wmediumd` on the dev PC (`t1/README.md`). No real Wi-Fi hardware is
used, now or later (the user, 2026-11-08). Results are relative comparisons inside this model; no number is a Wi-Fi
performance figure. The Raspberry Pis are not involved.

Request (user, 2026-11-14): the wireless results must also compare TickLE, CycloneDDS, Fast DDS and "zephyr-pico".
The repository has no Zephyr build of anything; the only pico-class stack in it is **zenoh-pico 1.10.2**
(`examples/perf_hil/zenohpico`), which RESULTS.md section 2 keeps **reference only, never scored**. "zephyr-pico" is
read as that. TickLE, CycloneDDS and Fast DDS are scored; zenoh-pico is listed and never scored.

## 1. Environment (same for every run)

- Topology `t1_topo.sh up 1`: namespaces `t1_0_0` (AP, no process), `t1_0_1` (ping station, 10.80.0.2, `wlan7`),
  `t1_0_2` (echo station, 10.80.0.3, `wlan8`); `PHY_BASE=6`, `RETRY=1` (per-attempt MAC retries), `SEED=7` (reproducible
  loss schedule), wmediumd pinned to core 2. One lane, one run at a time (rule L1). Node processes are NOT pinned
  (repo rule, 2026-10-05: pinning is not how software is run); only wmediumd (infrastructure) is.
- Unicast data frames get MAC retries; broadcast and multicast frames (discovery for every framework, zenoh-pico's best-effort
  data) get one attempt and no ACK (T1 limit). TickLE with 2 peers sends unicast (`tt_UNICAST_PEER_THRESHOLD` 2), DDS sends data
  unicast to the matched reader, so the data path is unicast for the three scored frameworks; discovery is multicast/broadcast.
- Arms (loss profiles, `name:bad_p:good_ms,bad_ms,good_p` of `t1_ge2.sh` / `t1_large.sh`):
  `ctl` 0.0:1000000,1,0.0 (no loss); `o60` 0.9:1140,60,0.01; `o200` 0.9:3800,200,0.01. These are the profiles the T1 notes found to
  produce application-visible loss under the retry model (`ug`, `library`, `psu_cs` are fully masked by MAC retries for unicast). The T0
  names `ge1b2`/`ge5b4` are netem profiles with no T1 counterpart and are not used.
- Machine: dev PC VM, 16 vCPU. Fast DDS 3.6 and CycloneDDS 11.0.1 as shipped in `/opt/ros/lyrical` (the rig runs Fast DDS 2.14 / Cyclone
  0.10.5/jazzy: versions differ from the rig tables, so these are not comparable to section 2). zenoh-pico 1.10.2 (commit 6a881c21).

## 2. Cells (identical workload for all frameworks)

| cell | QoS | payload | rate | run |
|---|---|---|---|---|
| `be_small` | BEST_EFFORT, VOLATILE, KEEP_LAST 100 | 100 B | 100 Hz | 60 s, ping/echo |
| `rel_small` | RELIABLE, VOLATILE, KEEP_LAST 100 (DDS max_blocking 1 s) | 100 B | 100 Hz | 60 s, ping/echo |
| `rel_large` | RELIABLE, VOLATILE, KEEP_LAST 10 (large_sample_bench default) | 1 MiB (see 2.1) | 0.25 Hz | 15 samples, pub/sub |

KEEP_LAST 100 on the small cells: T1 found history 10 evicts gaps before repair (`T1_PLAN.md` 2026-11-07, rel_small cause check); every
framework gets the same depth, so none is handed a depth handicap. The payload is 16 header bytes (send_ns, seq, reserved) plus zeros to
the size, one sample per topic message, topics `wifi_ping` / `wifi_pong` (pong = the 16 header bytes, same reliability). No ROS 2 layer.

Small cells = `wifi_link_node` semantics, re-implemented per framework (`experiments/t1fw/`): `ping` publishes at the rate for 60 s after its
endpoints matched (seq 0..), the first second (100 samples) is warm-up and not scored; `echo` answers the first copy of each sample from inside
the receive callback; `ping` takes the round trip from its own send stamp to the first pong naming the sample; 3 s drain. TickLE runs its own
`wifi_link_node` (`node_d100`, depth 100) unchanged. Both DDS ping waits for BOTH of its endpoints (writer and reader) to match; TickLE's node
waits for its Publisher only (recorded, favours TickLE nothing measurable: the pong direction is discovered in the same exchange).

2.1 `rel_large`: nominal 1 MiB at 0.25 Hz (the AP rebroadcast path carries ~0.6 Mbit/s; 1 MiB x 0.25 Hz = 2.1 Mbit/s offered, probably above it).
Pilot rule, fixed now: one TickLE and one each of the other frameworks at `ctl`, 1 rep, unscored. If TickLE delivers >= 12 of 15 samples within
a 40 s drain, `rel_large` stays 1 MiB; otherwise it is run at 128 KiB / 0.25 Hz (the cell T1 W11 shows working) and the 1 MiB pilot outcome of
every framework is recorded in the results as is. Large cells use `pub`/`sub` roles (one-way, send stamp to take time, CLOCK_MONOTONIC shared by
all namespaces); TickLE runs `large_sample_bench pub|sub -R` (`TT_UDP_OFFLOAD=0`, as T1 does).

## 3. Per-framework configuration (every knob recorded)

| framework | config | knob that differs from the product default |
|---|---|---|
| TickLE | `-p wifi` link profile, broadcast 10.80.0.255, `node_d100` / `large_sample_bench`, built from this tree | WIFI profile (the T1 convention; its discovery treatment) |
| CycloneDDS | `CYCLONEDDS_URI`: `Interfaces/NetworkInterface address=<own ip>`, `Discovery/SPDPInterval 1s` (the rig's URI, eth0 -> address) | SPDP 1 s (default 30 s), as the rig campaign |
| Fast DDS | `fastdds_eth0_only_mms1472.xml` adapted: UDPv4 user transport only, `interfaceWhiteList` = the two T1 addresses, `maxMessageSize 1472` | **UNFAIR-LOOKING KNOB, kept on purpose:** `maxMessageSize` 1472 (default 65500) so it fragments in RTPS, like TickLE and Cyclone (rig campaign `T`, user decision 2026-09-26). Pilot also records the default-size arm for `rel_large` at `ctl`, unscored. |
| zenoh-pico | peer mode; be: UDP multicast `udp/224.0.0.225:7447#iface=wlan`, rel: TCP peer-to-peer (listen on echo, connect from ping) | transport differs by design (RELIABLE = TCP, the only real reliability in 1.10.2); its be data crosses the single-attempt multicast leg, the others cross the unicast retry leg. Not scored. |

QoS mapping: DDS `RELIABLE`/`BEST_EFFORT`, `KEEP_LAST` depth, `VOLATILE`, no deadline/lifespan/liveliness; participants in domain 0 inside private
namespaces (no other traffic). No other tuning on any framework. Known asymmetry: DDS shared memory and Cyclone iceoryx are not available in
distinct namespaces; zenoh-pico and DDS are threaded, TickLE single-threaded poll; nothing is pinned.

## 4. Runs, order, noise control

Per (cell, arm): 3 rounds. Each round runs every framework twice (copies A and A2), in a rotated order of the list
`[tickle.A, cyclone.A, fastdds.A, zenoh.A, tickle.A2, cyclone.A2, fastdds.A2, zenoh.A2]` rotated left by 3*(round-1). The ordering
is fixed by the script and recorded in each run's file. One run = fresh topology (`t1_topo.sh down; up`), fresh processes, raw output kept under
`~/rig_results_safe/t1_fwcmp_20261114/<cell>/<arm>/r<round>_<fw>_<copy>.{topo,ping,echo,...}`.
So each (framework, cell, arm) has 6 runs. Matrix: 3 cells x 3 arms x 3 rounds x 8 = 216 runs (about 5 h at ~85 s per run).

A run with a harness fault (topology did not come up, process missing, binary crash before the match) is repeated up to 2 times and listed; a run in which the
endpoints never match within 15 s, or match and deliver nothing, is a RESULT of that framework (delivery 0, latency "none") and is kept as such.
Never a silent zero: the reader prints every run's `scored/sent` and match state.

## 5. Metrics

Small cells (ping side, round trip): **delivery %** = scored / (published - 100 warm-up) x 100; RTT **p50, p90, p99** in ms over scored samples
(the harness definition: index floor(n*p/100)). Also recorded, not scored: echo-side one-way delivery and p50/p90/p99.
`rel_large`: **delivery %** = delivered / published at the subscriber (15 expected), one-way latency **p50, p99** (15 samples: p90 not read).
A run with no scored sample has latency = infinity (worst) for the comparison.

## 6. Verdicts (same win rule as RESULTS.md "How to read these numbers")

Per cell, arm and metric, TickLE against each scored vendor (CycloneDDS, Fast DDS) over the 6 runs per framework:
**WIN** when every TickLE run beats every run of that vendor, **LOSE** when every run of that vendor beats every TickLE run, else **TIE**
(the repo's DRAW). "Beats" is strict: higher delivery, lower latency; equal values (e.g. 100% against 100%) are not a win. 
**VOID** for a (framework, cell, arm, metric) when |median(A2 runs) - median(A runs)| > 10 % of median(A runs) (the T1 noise rule; for delivery
the 10 % is of the delivery percentage, so 100 -> 89 voids); a pair involving a VOID framework-metric is VOID for that metric and is not counted. The
verdict table also prints the 6 values so a reader can see the spread. zenoh-pico gets the same measurements and the same noise flag, is printed beside
the scored columns, and has **no verdict and no count**. A run set where TickLE was VOID by a harness fault in more than 1 of 6 is VOID.
Summary counts WIN/TIE/LOSE/VOID are per metric-cell-arm (small: 4 metrics, large: 3) and kept separate from every number in section 2 of RESULTS.md.

Nothing here is changed after the first measured run except by a dated amendment below that says what was seen first. A LOSE is reported as found; the
libraries and TickLE are not modified. Pilot runs (section 7) are harness checks, unscored, kept in the raw directory under `pilot/`.

## 7. Pilot

One cell/arm first (`be_small`, `ctl`, one run per framework) and then `rel_small` `ctl`: every framework must show `scored > 0` and a matched pair; a framework
that shows 0 is debugged in the harness (interface binding, multicast route, QoS) before the matrix, never in the library. A framework that cannot
run in T1 at all after harness fixes (e.g. DDS discovery over the hwsim AP, zenoh-pico TCP under loss) is recorded as such in the results, with its raw output.

## 8. Amendments
### 2026-11-14 (after the pilot, before any scored run)
Pilot raw: `~/rig_results_safe/t1_fwcmp_20261114/pilot*` (be_small and rel_small `ctl`, 20 s; rel_large `ctl`, 1 MiB and 128 KiB, 1 rep each, unscored).
- Harness fixes found by the pilot (harness only, no library touched): `lo` is brought up in both stations for every framework (Fast DDS sets
  IP_MULTICAST_IF to 127.0.0.1 and found no peer without it); the Fast DDS profile has **no `interfaceWhiteList`** (with it Fast DDS 3.6 never
  discovered a peer here; each namespace only has its wlan and `lo` anyway); Cyclone's URI is a per-run file. `maxMessageSize 1472` is kept.
- `rel_large` drain: TickLE's `large_sample_bench` pub stays 3 s (DRAIN_NS) after its publishing loop and its sub runs `-d` 68 s. Every framework
  gets the same window (pub: 60 s + 3 s from its first publish, sub: 68 s). The "40 s drain" of section 2.1 is replaced by this.
- Section 2.1 pilot rule applied: 1 MiB at 0.25 Hz, TickLE delivered 0 of 15 at `ctl` (the pilot table in the results lists every framework), so
  `rel_large` is run at **128 KiB / 0.25 Hz** and the 1 MiB pilot is reported as a recorded result, not scored.
- Delivery denominator for the small cells: published - 100 (published counted by the ping); a run that never matched has published 0 and delivery 0.

# T1 on the Pi rig (T1-Pi): topology, rules and plan (registered 2026-11-14, before any measured run)

**Simulation only.** No real wireless hardware is used, now or later (the user, 2026-11-08). The Wi-Fi hop is
`mac80211_hwsim` + the patched `wmediumd` (`examples/perf_hil/experiments/t1/README.md`). Nothing here is a Wi-Fi
performance figure; every number is a relative figure inside this model.

Request (the user, 2026-11-14): run the Wi-Fi (T1 simulation) comparison TickLE vs CycloneDDS vs Fast DDS
(zenoh-pico reference only, never scored) also on the two Raspberry Pi 5 of the rig, because they are dedicated and
quieter than the dev PC. Rig results are the reference ("all experiment data is rig based"). A PC-side session runs the
same comparison on the PC (`T1_FRAMEWORK_CMP_PLAN.md`); it is not touched.

## 1. Topology: one simulated Wi-Fi hop, wired server

```
 rpi-1 (client, 192.168.10.3)                                                    rpi-0 (server, 192.168.10.2)
 +-----------------------------------------------------------+                   +------------------------+
 | netns t1p_sta        netns t1p_ap                         |                   | netns t1p_srv          |
 |  hwsta 10.90.0.2 ~~~ hwap --- br0 --- t1pgt (gretap)======|== eth0 (wired) ===|== t1pgs (gretap)       |
 |  (hwsim phy,STA)      (hwsim phy,AP)                      |   GRE over        |   10.90.0.3  echo/sub  |
 |  ping / pub           wpa_supplicant mode=2               |   192.168.10.x    +------------------------+
 |  wmediumd (init ns, root, pinned to core 2) joins the two hwsim radios        |
 +-----------------------------------------------------------+
```

* **Client side (rpi-1):** a station namespace `t1p_sta` holds one hwsim radio (STA, 10.90.0.2/24, runs `ping` / `pub`).
  An AP namespace `t1p_ap` holds the second hwsim radio (AP, `wpa_supplicant` mode=2) in a bridge `br0` together with a
  `gretap` tunnel `t1pgt`. wmediumd (patched, `/usr/local/bin/wmediumd`) decides, per frame, whether the STA<->AP
  frame arrives (retry model `RETRY=1`, `SEED=7`, Gilbert-Elliott profile or steady error per run).
* **Wire:** the tunnel is carried by the physical Ethernet (`eth0`, 192.168.10.3 <-> 192.168.10.2) as GRE (IP protocol 47)
  between the two existing eth0 addresses. `eth0`'s address, routes and flags are never changed: the tunnel devices are
  created in the init namespace (`local`/`remote` bind them to eth0's addresses), then moved into namespaces. The tunnel is
  L2, so broadcast and multicast discovery (TickLE broadcast 10.90.0.255, DDS SPDP multicast) cross like on a bridged
  Wi-Fi AP. `nopmtudisc` and MTU 1500 keep the L2 MTU at the Wi-Fi 1500 (a full frame is outer-fragmented on the wire:
  lossless, a little CPU); see amendment 1 for the DF bit.
* **Server side (rpi-0):** namespace `t1p_srv` with the other tunnel end (`t1pgs`, 10.90.0.3/24). The server process runs
  natively on rpi-0 (not in a VM or container); the namespace keeps it off rpi-0's `wlan0` and `eth0` so DDS and TickLE
  cannot see or use them, as the PC's private namespaces do.
* **The Wi-Fi hop is simulated once.** A wireless client talking to a wired server crosses the medium once per direction
  (STA->AP uplink, AP->STA downlink). The old PC T1 is STA-AP-STA: two simulated hops each way, so loss compounds
  (the T1 notes: realized be loss about 4.2 x p over the two-hop path). **PC and Pi numbers are therefore NOT directly
  comparable**: same loss profile names, about half the exposure here, an extra wire hop (about 0.2 ms RTT), a different
  CPU. The Pi tables stand alone; the PC tables are cited only for the noise comparison of section 5, with that caveat.
* **Data path:** TickLE with 2 peers and DDS send unicast data, so the data path is unicast STA<->AP with MAC retries;
  discovery is broadcast/multicast with one attempt and no ACK (T1 limit, unchanged).

### Safety (the Pis have a real `wlan0`)
* `wlan0` (brcmfmac, phy0) carries management and ssh and is never named, moved, scanned or reloaded by any script.
  Only phys created by `modprobe mac80211_hwsim radios=2` are used: the new phys are found as the set difference of
  `/sys/class/ieee80211` before and after, and every phy touched must resolve to `/sys/devices/virtual/mac80211_hwsim/*`
  and have exactly 2 members; otherwise the script refuses.
* `wpa_supplicant` and `wmediumd` are started with pid files and stopped by pid after checking `/proc/<pid>/exe`;
  `pkill` by name is never used (the system `wpa_supplicant.service` runs the real wlan0).
* `mac80211_hwsim` must not be loaded when `up` starts (it is not on either Pi now); `down` removes it again with
  `modprobe -r` only if `up` loaded it. `cfg80211` stays (brcmfmac uses it).
* The runner takes a snapshot of `wlan0`/`eth0` (addresses, routes, flags, `iw dev wlan0 info`, loaded modules, phy list,
  namespaces, other GRE links) before the first run and compares it after every run; any difference stops the run list.
* The rig lock is taken first (`flock /tmp/rig.lock` on the PC around `rig_lock.sh`, which holds the CI lock
  `/tmp/tickle-hil.lock`), inside the detached process.

## 2. Cells and profiles (same definitions as the PC plan)

| cell | QoS | payload | rate | run |
|---|---|---|---|---|
| `be_small` | BEST_EFFORT, KEEP_LAST 100 (`node_d100`) | 100 B | 100 Hz | 60 s (V1, calibration: 20 s), ping on the STA, echo on the server |
| `rel_small` | RELIABLE, KEEP_LAST 100 | 100 B | 100 Hz | 60 s |
| `rel_large` | RELIABLE | 1 MiB (pilot rule as the PC plan 2.1) | 0.25 Hz | 15 samples, pub on the STA, sub on the server |

Profiles (loss arms, `name:bad_p:good_ms,bad_ms,good_p` of `t1_ge2.sh`): `ctl` 0.0:1000000,1,0.0; `o60` 0.9:1140,60,0.01;
`o200` 0.9:3800,200,0.01; `RETRY=1 SEED=7`, wmediumd pinned to core 2, nodes not pinned. Steady link error p for the
validation: `LOSS=p`, no GE.

Measured on the **STA side (ping)** only: round trip. The one-way latency of the PC T1 needs one clock for both ends;
the Pis do not share one, so there is no one-way figure here (echo-side one-way is recorded but never read).
**Delivery** = scored / (published - warm-up 100) at the ping. RTT p50/p90/p99 from the harness. rel: `retransmitted`.

## 3. Rules (adapted from T1_PLAN.md; fixed now)

* **V1** (gate): p = 0, `be_small` 20 s, 3 reps: delivery >= 99.9% and **RTT p99 < 40 ms** (the PC rule is one-way p99 < 20 ms;
  a round trip crosses the hop twice) in 3 of 3 reps. Fail = stop, no matrix, report.
* **V2** (report, not a gate): at p = 0.1 and 0.3 the realized loss is above the p = 0 run, rises with p, and the 3 reps'
  delivery differs by <= 10% of its median; otherwise that arm is VOID.
* **Noise:** a comparison is VOID when |A2 - A| > 10% of A for its metric (median of the A copies against median of the A2
  copies; for delivery the 10% is of the delivery percentage).
* **A/A floor** (section 5): same node, same profile, same seed, alternating copies A / A2, spread = (max - min) / median of
  the delivered count over the reps. Pi is called **quieter** than the PC for a cell+profile when its spread is smaller than
  the PC's N1 figure for the same cell and profile (be o60 1.6%, be o200 0.4%, rel o60 2.1%, rel o200 1.2%; the PC's whole
  range for be is 0.4-1.6%, for reliable 2-3%); a difference below 0.3 pt is a TIE. Latency (p50/p99) and `retransmitted`
  spreads are reported but, as on the PC, p90 and retx are read only for >3x differences.
* **Gate to the matrix:** V1 passes in 3 of 3 AND the Pi delivered spread is <= 3% in all four A/A sets (the PC's usability
  limit); otherwise the cross-framework matrix is not run and the finding is reported.

## 4. Calibration of the link loss for the one-hop topology (before any A/A)

Purpose: the loss profiles were found on the two-hop PC topology; with one hop the same profile exposes the flow about half
as much. Measure, not assume, what each profile does here: `be_small` 20 s x 3 reps at steady p = 0, 0.1, 0.3 (V1/V2), and
60 s x 2 reps of `be_small` and `rel_small` at `o60` and `o200`: realized delivery loss, RTT p50/p99, retx. Reading rule: the
arms stay `ctl/o60/o200` (identical to the PC plan) when `o60` and `o200` realize a `be_small` loss between 0.5% and 25%
and `rel_small` shows retransmissions > 0; otherwise the shortfall is reported and an amendment (below) names stronger profiles
before any comparison run. No profile is tuned to favour a framework.

## 5. A/A noise floor, Pi vs PC

`be_small` and `rel_small`, profiles `o60` and `o200`, 120 s runs (the PC N1 length), 5 reps each, copies alternate A, A2, A,
A2, A (fresh topology and fresh processes per run). 20 runs, about 50 minutes. Reported with the PC N1 numbers beside them
and the one-hop/two-hop caveat of section 1.

## 6. Cross-framework matrix (only if the gate of section 3 passes)

`T1_FRAMEWORK_CMP_PLAN.md` (PC, registered 2026-11-14 16:47) exists. If the gate passes, its cells, arms, QoS mapping, run
order (rotated, A and A2 copies, 3 rounds), metrics and WIN/TIE/LOSE/VOID rules are ported **unchanged** to this topology;
the only changes are the ones this document states: the topology of section 1 (client on the STA, server on rpi-0), the
round-trip-only latency, aarch64 builds of every framework, and the DDS versions installed on the Pis (Fast DDS 2.14 /
CycloneDDS 0.10.5 as the rig uses, against the PC's 3.6 / 11.0.1). TickLE stays on `-p wifi` and `node_d100`. Each framework
is piloted once (`be_small` ctl, then `rel_small` ctl; scored > 0 and a matched pair, no silent zero) before the matrix; a
framework that cannot run is reported with its raw output, never fixed in the library. zenoh-pico gets the same measurements
and no verdict. Raw data: `~/rig_results_safe/t1pi_fwcmp_<date>/`.

## 7. Deliverables and clean-up
Scripts under `examples/perf_hil/experiments/t1pi/` (`t1pi_topo.sh` up/down, runner, readout, node build); raw data under
`~/rig_results_safe/t1pi_*`; results in `~/claude_reports/`. After every run and at the end: no `t1p_*` namespace, no hwsim
phy, no `t1pg*` link, `mac80211_hwsim` unloaded, `wlan0`/`eth0` as found.

## 8. Amendments

1. (2026-11-14, after the topology bring-up and before any V1 run.) The GRE tap rejects an inner packet that has the DF bit
   set and does not fit the tunnel MTU, and UDP sockets set DF, so a 1500-byte datagram was dropped while the segment MTU was
   1500 (seen with `ping -M do -s 1472`: 100% loss; with a 1462 MTU instead TickLE warns that its configured link MTU 1500 is
   larger than the link). Fix, a property of the simulated segment and not of any framework: two nftables rules, in the AP
   namespace (bridge `forward` hook) and in the server namespace (ip `output` hook), clear DF on inner packets whose frag-off is
   exactly DF (`ip frag-off == 0x4000 ip frag-off set 0`); the outer GRE packet is fragmented on the wire. After it
   `ping -M do -s 1472` works in both directions. nf_tables is loaded for the session and removed at the end with the other
   modules the session loaded (nfnetlink was already loaded and in use on both Pis and stays).

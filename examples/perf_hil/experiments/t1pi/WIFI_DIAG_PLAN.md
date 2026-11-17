# Wi-Fi diagnosis plan (phase 1 of WIFI_STRATEGY_2026-11-17.md), registered 2026-11-17 before any measured run

**Simulation only** (mac80211_hwsim + patched wmediumd on the two rig Pis, T1pi topology of `examples/perf_hil/experiments/t1pi/PLAN.md`). Relative figures, never Wi-Fi performance.
**Diagnosis only**: no library, wire, API or default change. Branch `ab/wifi-diag` (from `origin/ab/release-2.2.0`). The Pis' real `wlan0`/`eth0` are never touched; the runners snapshot
state before/after every run (existing `check_state`) and the Pis are left as found (no hwsim, no `t1p_*`).

## 0. What an exploratory probe showed before this plan was written (one topology up/down, no traffic measured)
The simulated STA and AP run mac80211's **minstrel rate control** (debugfs `.../stations/<mac>/rc_stats`): a fresh link starts on CCK 1.0M (airtime 9.8 ms for a 1472 B frame, about 100 frames/s)
and HT rates are only reached after probing. So a link capacity that depends on the rate control state (and so on traffic history) is a **hypothesis H-RC** for the 80 datagrams/s "drip" level
(1472 B x 8 / 1 Mbit/s = 11.8 ms = 85/s) and for the run-to-run bimodality. This plan tests it; it is not assumed. The mac80211 txq (fq, `fq_limit` 8192, per-station aqm counters in debugfs)
is the other candidate for the burst tail-drop (hwsim's own queue). Both are sampled.

## 1. Instruments (new, harness only)
* `t1pi_sampler.py` (on the client Pi, root for debugfs): once per second, for the STA and the AP station entries: minstrel best-rate rows (name, probability, successes/attempts), aqm
  drops/overlimit/backlog of both phys, wmediumd utime+stime ticks, and the per-core /proc/stat counters. Output `<run>.rc`. Read-only.
* `t1pi_fw.sh` gains `SAMPLE=1` (start the sampler beside the run) and `PCAP_SNAP` (tcpdump snaplen, default 96 as before; 160 for the DDS timeline so RTPS submessage ids are visible). Defaults unchanged.
* `t1pi_udpcap.py` (tx / rx, python3 on the Pis) and `t1pi_cap.sh` (driver on the PC, same lock/lib/safety as `t1pi_fw.sh`).
* `t1pi_wdiag.py` (PC): own pcap reader (no scapy), timelines, burst/gap statistics, control-message cadence, classification; `t1pi_capread.py` for (a).

## 2. (a) Capacity curve, plain UDP, no TickLE
Arms ctl / o60 / o200 (the GE profiles of `t1pi_lib.sh`, `RETRY=1 SEED=7`, wmediumd pinned to core 2), sender in `t1p_sta`, receiver in `t1p_srv`, unicast UDP, 5 reps.
* **a1 grid** (one topology per arm and rep, the cell order randomised per rep with a recorded seed; each cell 5 s; minstrel state at the start and end of every cell is in the `.rc` log):
  payload 1472 B; burst size B in {1, 4, 16, 64} datagrams sent back to back; offered rate R in {20, 40, 80, 120, 200, 400, 800} datagrams/s (so the inter-burst gap is B/R minus the send time);
  plus B = 91 (one 128 KiB sample) at R in {23, 91, 182}; plus payload {100, 700} B at B in {1, 16}, R in {80, 400}. 39 cells per topology. Metrics: delivered datagrams/s (receiver, whole cell and the
  last 3 s), loss fraction, burst-internal arrival fraction. 15 topologies, about 70 min.
* **a2 fresh-link** (fresh topology per rep, one cell each): (B=16,R=400), (B=91,R=91), (B=1,R=200), 5 reps per arm; shows the capacity at a link with no history (bimodality).
* **a3 controls** (ctl arm, 5 reps each, same cells as a2): (i) wire-only path (AP-namespace bridge to the server, bypasses wmediumd and hwsim: capacity of the GRE path and the Pis' CPU);
  (ii) STA rate fixed with `iw dev hwsta set bitrates legacy-2.4 54` if the kernel accepts it (rc_stats must show it): does the limit move with the rate; (iii) wmediumd CPU use at the limit (from the ticks).
* **Decision rule "hwsim/wmediumd or Pi CPU":** a limit is a CPU limit if the wire-only control saturates at the same datagram rate or wmediumd's core is >= 90% busy at the knee, and
  the limit does not move when the rate or the frame size changes. It is a simulator/airtime property if the knee in bytes/s is constant over frame sizes, moves with the minstrel rate, and no
  core is saturated. Reported with the numbers whichever way.

## 3. (b) Timeline against DDS
Cell `rel_large` of `t1pi_fw.sh` at 1 MiB and 128 KiB, 0.25 Hz, 60 s, arms ctl/o60/o200, frameworks `tickle` (default profile WIFI, unpaced), `fastdds` 2.14.6, `cyclonedds` 0.10.5, binaries and XML
exactly as the 11-14 / 11-15 matrices; 2 rounds, copy A only (36 runs), order rotated; `PCAP=1 PCAP_SNAP=160 SAMPLE=1`. Capture: STA radio (sender side) and server tap (receiver side).
Read per run: datagrams/s per 100 ms and 1 s bins both ends; burst size (a burst = datagrams less than 5 ms apart) distribution; inter-datagram gap quantiles; control traffic cadence
(TickLE ACKNACK/HEARTBEAT submessage types from the 4-byte single header; RTPS HEARTBEAT/ACKNACK/NACK_FRAG/DATA_FRAG submessage ids), the interval between successive repair rounds;
total data datagrams sent for the 15 samples, the share that are repeats; the fragment/unit size and datagrams per sample; completion time per sample from the subscriber's latency file; the sampler's
minstrel rate over time. The statement of what Fast DDS does is limited to what these show (rate, burst, gap, fragments, cadence, flow control); anything not visible is marked "not shown".
Caveat: capture drops of tcpdump are possible; the receiver-side count is cross-checked with the node counters.

## 4. (c) Drip entry
At least 12 unpaced TickLE runs at 1 MiB, arm o200, `PCAP=1 SAMPLE=1` (in addition to the rounds of (b)), plus the counters of the 36+ unpaced 1 MiB runs of the earlier sweep
(`t1pi_pace_20261115/stage1_1m`, no pcaps). **Classification (fixed now):** a run is *drip* if it delivers <= 12 of 15 samples; *good* if >= 14; 13 is *mixed*. The pcap criterion for the regime is
the arrival rate of big datagrams at the receiver over the run's busiest 20 s window being within 60-100 dg/s and flat (coefficient of variation of the 1 s bins < 0.5). The two criteria are
compared and a disagreement is reported. Correlates read per run: peer registrations (`Publisher peer registered` lines), `retransmitted`, ACKNACK received/sent, `tx_udp_broadcast`,
`addressed_with_discovery`, `summary_copies`, `gaps_opened`, minstrel rate at the start of the first sample and its trajectory, discovery/announce datagrams in the first 10 s of the pcap, and the offered load
before the first sample. A factor counts as a candidate cause when it separates good from drip with no overlap in the 12 runs (or in the earlier counter set where it exists); with these n only
separation is claimed, not a p-value. The minstrel hypothesis is judged by whether the rate state differs between classes before the backlog forms.

## 5. Deliverable and criteria
`~/claude_reports/WIFI_DIAG_RESULTS_2026-11-17.md`: data, tables, answers (a)(b)(c), ranked levers L1 repair cadence, L2 adaptive repair-burst limiting, L3 slow start, L4 drip-state fix, each with the effect
size the data supports and the wired metric it could touch. No criterion of a lever is "validated" here: levers are hypotheses with expected effect from the timeline, to be tested in phase 2.
Raw data `~/rig_results_safe/wifi_diag_20261117/`. Scripts under `examples/perf_hil/experiments/t1pi/`. A run that loses its RESULT after 3 attempts is reported as lost, never replaced silently.
All Pi jobs run under `flock /tmp/rig.lock` + `rig_lock.sh`, detached, one at a time.

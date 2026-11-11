# T1: wireless simulation scripts

T1 is the wireless stage's test environment: `mac80211_hwsim` (virtual Wi-Fi radios in the kernel) plus a
patched `wmediumd` (the simulated medium that decides, per frame, whether it arrives). It runs on the dev PC.

**What T1 is.** A simulation used as a relative A/B instrument: arm A, a noise-control repeat A2 (= A) and
arm B are run in the same environment, rotated per round, and compared with each other.

**What T1 is NOT.** It is not Wi-Fi, and no T1 number is a Wi-Fi performance figure. hwsim has no real airtime
contention and no congestion control, and no real wireless hardware is used, now or later (decision 2026-11-08).
Absolute latencies and loss rates here describe the model, nothing else. See "Known limits" below.

## Requirements

- Linux with root: the scripts call `sudo -n` for `ip`, `iw`, `wpa_supplicant`, `wpa_cli`, `wmediumd`,
  `pkill -x wmediumd`. Everything runs in private network namespaces (`t1_L_K`); `t1_topo.sh` refuses any phy that
  is not a `mac80211_hwsim` device, so a real Wi-Fi device is never touched.
- Kernel module `mac80211_hwsim` loaded with enough radios (a lane uses 3 phys; the scripts default to
  `PHY_BASE=6` because phys 0-5 were held by leftover processes on the original PC), `wpa_supplicant`, `iw`,
  `taskset`, `python3`.
- Build of the patched wmediumd: libnl-3 and libnl-genl-3 headers (`/usr/include/libnl3`), cmake, make, gcc.
  libconfig 1.7.3 is built statically by the script.
- Never run this alongside a rig campaign (it loads the PC's CPUs).

## Build

```
./t1_build_wmediumd.sh      # libconfig + wmediumd (upstream 717e5d7, "0.3.1") + wmediumd-0.3.1-t1.patch
./t1_build_node.sh          # node_d100, node_base, large from this tree, into $T1_DIR
```

`wmediumd-0.3.1-t1.patch` is `git diff` of upstream bcopeland/wmediumd commit
`717e5d7fcc23eecbc8e32bd897a8fd4b1e3ba640` (the pristine tree the PC's clone was at, no 0.3.1 tag exists in it);
it changes `wmediumd/config.c` and `wmediumd/wmediumd.c`. It adds, per directed link, a Gilbert-Elliott good/bad
overlay with an optional slow quiet/busy envelope (`model.ge`), per-attempt MAC retries (`model.retry`), a
reproducible schedule (`model.seed`) and a frame trace (`model.trace`). Config keys are exported to the
environment variables `WM_GE`, `WM_RETRY`, `WM_SEED`, `WM_TRACE`.

Environment overrides (all scripts): `T1_DIR` work dir (node binaries, `run/`; default `$HOME/tmp_build/wm/t1`),
`WM` wmediumd binary (default `$HOME/tmp_build/wm/wmediumd/wmediumd/wmediumd`), `RESULTS` output root (default
`$HOME/rig_results_safe`), `W`/`REPO` the source tree to build nodes from (default: this repository),
`T1_TMPDIR`, `PHY_BASE`, `RETRY`, `SEED`, `TRACE`, `PIN_WM`/`PIN_ECHO`/`PIN_PING`, `NODE`, `REPS`, `DUR`, `CELL`.

## Scripts and one example each

| Script | Purpose | Example |
|---|---|---|
| `t1_topo.sh` | Lanes of 3 nodes (AP, ping STA, echo STA), wmediumd, wpa_supplicant | `RETRY=1 BAD_P=0.9 ./t1_topo.sh up 1 "1140,60,0.01"`; `./t1_topo.sh down` |
| `t1_v1.sh` | Test 1, rules V1/V2: be_small at link error 0, 0.1, 0.3 | `P_LIST="0.0 0.1" ./t1_v1.sh` |
| `t1_c1rel.sh` | same for the rel cell | `./t1_c1rel.sh` |
| `t1_ge.sh` | Gilbert-Elliott overlay calibration (be_small) | `REPS=2 ./t1_ge.sh` |
| `t1_lanes.sh` | Test 7, rule L1: N lanes at once | `LANE_LIST="1 1 2" REPS=3 ./t1_lanes.sh` |
| `t1_ge2.sh` | Smoke of the PSU-fitted slow-envelope profiles (prints `T1_GE2_MODEL`; `RETRY` defaults to 1) | `CELL=be REPS=2 DUR=60 ./t1_ge2.sh` |
| `t1_ge2m.sh` | the same with a third idle node | `CELL=be ./t1_ge2m.sh` |
| `t1_noisepin.sh` | noise check, same node N times, CPU pinned | `./t1_noisepin.sh` |
| `t1_abs.sh` | candidate A/B (A, A2, B rotated) on a stress profile, per-attempt retry | `GE=1140,60,0.01 BADP=0.9 ROUNDS=6 ./t1_abs.sh` |
| `t1_abd.sh` | candidate A/B on a steady link error | `CELL=rel P=0.012 ./t1_abd.sh` |
| `t1_depth.sh` | diagnostic: writer history depth 10 vs 100 | `./t1_depth.sh` |
| `t1_large.sh` | rel_large (large samples) with retry model | `REPS=2 DUR=30 HZ=0.25 SZ=131072 ./t1_large.sh` |
| `t1_ldiag.sh` | one no-loss rel_large run with counters sampled live | `SZ=131072 HZ=1 ./t1_ldiag.sh` |
| `t1_seedtest.sh`, `t1_seedtest2.sh` | same-seed reproducibility of the GE schedule (`ug`, `o200`) | `./t1_seedtest.sh` |
| `t1_s0.sh`, `t1_s0_all.sh` | range-NACK stage 0: ACKNACK byte share on broadcast cells | `MODE=small CELLS=rel ./t1_s0.sh`; `./t1_s0_all.sh` |
| `s0_read.py` | reads `t1_s0.sh` output | `python3 s0_read.py <outdir>` |
| `ge2_fit.py` | fits the slow-envelope loss model to the PSU statistics | `python3 ge2_fit.py` |
| `crawdad_stats.py` | per-level loss/burst statistics from the PSU wlan_pcap data | `python3 crawdad_stats.py <data-root>` |
| `pcap_retry.py` | MAC attempts per frame from a gzip 802.11 pcap | `python3 pcap_retry.py file.pcap.gz` |

The A/B scripts expect the arm binaries (`node_d100`, `node_base`, `node_evict_d100`, ...) in `$T1_DIR`; build an
arm from its own tree with `W=<tree> T1_BIN_SUFFIX=_evict_d100 ./t1_build_node.sh`. Results go to
`$RESULTS/<name>/<timestamp>/`. All scripts start `wmediumd` and `wpa_supplicant` and tear them down with
`t1_topo.sh down`; run only one at a time.

## Registered rules (from the PC-local plan, `~/claude_reports/T1_PLAN.md`, fixed 2026-11-06 before any run)

- **V1** (test 1): with no loss, be_small delivery >= 99.9% and one-way p99 < 20 ms in 3 of 3 reps.
- **V2** (test 1): at link error p = 0.1 and 0.3 the realized loss is above the p = 0 run, rises with p, and the
  delivery of 3 reps differs by <= 10% of its median; otherwise the arm is VOID. (Result: V1 pass; V2 fails at
  p = 0.3, which is VOID; a per-frame error of 0.1 is far harsher than 10% realized loss.)
- **L1** (test 7): lanes 1/2/4 running the same cell at once, against the cell alone: a lane count passes when
  every lane stays within 0.5 pt delivery and 10% of the alone p50 and p99, with the alone-vs-alone repeat inside the
  same limits. The count used afterwards is the largest passing one.
- **C1** (test 2): T1/T0 ratio (netem baseline) of delivery loss, p50 and p99 per profile and cell; no pass/fail, a
  ratio outside [0.5, 2] is listed as a difference to explain before a candidate is judged on T1.
- **Noise:** a comparison is VOID when |A2 - A| > 10% of A for its metric.
- **Candidate A/B** (test 8): rules written per candidate before its run; none is lifted on a single rep.

## Known limits

- **Broadcast frames get one attempt**, no ACK, no retry; unicast gets per-attempt MAC retries with `RETRY=1`.
  Broadcast goes station -> AP (unicast, with retries) and AP -> stations (one attempt), so a round trip crosses two
  single-attempt downlinks.
- **The AP broadcast path carries only ~55-60 datagrams/s (~0.6 Mbit/s)**; rel_large must stay below ~0.4 Mbit/s
  (128 KiB at 0.25 Hz); 1 MiB at 5 Hz is airtime saturation, not a bug.
- **Only 1 lane passes L1**: one wmediumd shares one simulated medium across lanes (2 lanes lost 1.2-4.1 pt more
  than alone, 3 lanes far more). Run one lane at a time.
- The loss models are fitted/calibrated, not measured Wi-Fi; link error is not realized loss (about 4.2 x p over the
  two-hop be path). Contention between senders and congestion control are out of scope.
- `ge2_fit.py` and `crawdad_stats.py` need the CRAWDAD data, which is not in the repository.

## Data source

The profiles `ug`, `library`, `psu_cs`, `pioneer` were fitted to the PSU (Portland State University) wlan_pcap
traces of the CRAWDAD dataset pdx/vwave by Caleb Phillips and Suresh Singh, funded in part by NSF 0435328; see
`docs/ROADMAP.md`, "Data sources", for the full citation and licence.

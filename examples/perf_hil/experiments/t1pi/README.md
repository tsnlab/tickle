# T1-Pi: the T1 wireless simulation on the two Raspberry Pi of the rig

Plan and rules: `PLAN.md` (registered before any run). **Simulation only**: `mac80211_hwsim` + the patched `wmediumd`
(`../t1/`), no real wireless hardware. One simulated Wi-Fi hop (STA -> AP) on rpi-1, a GRE tap bridged to the AP carries the
frames over the rig's Ethernet to a wired server namespace on rpi-0. Not comparable with the PC's STA-AP-STA T1.

| Script | Purpose |
|---|---|
| `t1pi_topo.sh` | on a Pi: `cli up/down` (rpi-1: 2 hwsim radios, wmediumd, wpa_supplicant, bridge + GRE tap), `srv up/down` (rpi-0), `modules load/unload`, `snap` |
| `t1pi_build.sh` | on the PC: build the node programs natively on rpi-1 from this tree, copy scripts and binaries to both Pis |
| `t1pi_run.sh` | on the PC: `v1`, `calib`, `noise`, `one` sets; takes `flock /tmp/rig.lock` + `rig_lock.sh`; snapshot of wlan0/eth0/modules before and after every run; clean teardown in a trap |
| `t1pi_read.py` | readout: run table, V1 verdict, A/A floor against the PC |

Safety: the Pis' real `wlan0` (management, ssh) and `eth0`'s configuration are never touched. Only phys created by
`modprobe mac80211_hwsim radios=2` are used (every phy is checked against `/sys/devices/virtual/mac80211_hwsim/*`),
daemons are stopped by pid file after checking `/proc/<pid>/exe`, never by name. `down` removes the namespaces and the
tunnel devices and unloads `mac80211_hwsim`; the runner's `modules unload` removes bridge / ip_gre / nf_tables if it loaded them.

Usage (on the PC, in this directory; each call is long-running: start it detached, `setsid nohup ... &`):

```
./t1pi_build.sh                       # once per tree
OUTDIR=t1pi_v1 ./t1pi_run.sh v1       # rule V1, 3 x 20 s
OUTDIR=t1pi_calib ./t1pi_run.sh calib # one-hop loss calibration
OUTDIR=t1pi_noise ./t1pi_run.sh noise # A/A floor, 4 sets x 5 x 120 s
python3 t1pi_read.py --v1 ~/rig_results_safe/t1pi_v1/<stamp>
python3 t1pi_read.py --noise ~/rig_results_safe/t1pi_noise/<stamp>
```

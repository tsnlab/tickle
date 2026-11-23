# tc-emulated Wi-Fi-like link (two Raspberry Pi)

Plan and rules: `PLAN.md` (registered before any run). An **emulation with `tc netem`**, not Wi-Fi: a rate, delay, loss and queue per profile on a
GRE tap between a namespace on each Pi. No library change.

| Script | Purpose |
|---|---|
| `wtc_topo.sh` | on a Pi: `cli up/down`, `srv up/down` (namespace + tap + netem), `modules load/unload`, `snap` |
| `wtc_lib.sh` | profiles (`profile_netem`), bring-up, state check, session start/end |
| `wtc_run.sh` | on the PC: `MODE=cal` calibration, otherwise the framework comparison runs; takes `flock /tmp/rig.lock` |
| `wtc_probe.py` | plain-UDP probe of the link (flood, burst, paced flow) for the calibration |
| `wtc_read.py` | readout: per-cell tables with WIN/TIE/LOSE and the link settings of each profile |
| `wtc_ship.sh` | copies the topology script and probe to both Pis |

```
./wtc_ship.sh
OUT=$HOME/rig_results_safe/wtc_cal MODE=cal ARMS="M1 M2 S1 S2 C1" ./wtc_run.sh
OUT=... CELLS="be_small rel_small" PACE_M1=22 PACE_M2=22 PACE_S1=3 PACE_S2=3 ./wtc_run.sh
OUT=... CELLS=rel_large LARGE_SZ=1048576 ... ./wtc_run.sh
python3 wtc_read.py <OUT>
```

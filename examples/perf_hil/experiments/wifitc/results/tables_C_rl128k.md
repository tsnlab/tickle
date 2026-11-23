# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## rel_large / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`

| metric | tickle-p95 | cyclonedds | fastdds | zenohpico | tickle-p95 vs CycloneDDS | tickle-p95 vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 43.80 [43.39..44.12] | 55.19 [41.91..55.83] | 91.18 [88.43..91.29] | n/a [n/a..n/a] V | TIE | WIN |
| p99 ms | 54.70 [52.21..77.97] | 126 [118..156] | 91.85 [91.63..92.14] | n/a [n/a..n/a] V | WIN | WIN |

- tickle-p95 scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`

| metric | tickle-p95 | cyclonedds | fastdds | zenohpico | tickle-p95 vs CycloneDDS | tickle-p95 vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 78.89 [70.58..86.82] | 99.74 [56.49..103] | 106 [104..106] | n/a [n/a..n/a] V | TIE | WIN |
| p99 ms | 135 [98.36..554] V | 257 [212..462] | 119 [107..1152] | n/a [n/a..n/a] V | VOID | VOID |

- tickle-p95 scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`

| metric | tickle-p95 | cyclonedds | fastdds | zenohpico | tickle-p95 vs CycloneDDS | tickle-p95 vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 292 [289..293] | 307 [306..336] | 3562 [3521..3620] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 304 [296..627] | 439 [406..460] | 3848 [3703..4317] | n/a [n/a..n/a] V | TIE | WIN |

- tickle-p95 scored/offered: 15/15, 15/15, 16/16, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`

| metric | tickle-p95 | cyclonedds | fastdds | zenohpico | tickle-p95 vs CycloneDDS | tickle-p95 vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 325 [308..361] | 439 [411..470] | 2327 [2109..2478] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 570 [340..1044] | 944 [634..1165] | 2749 [2681..4263] | n/a [n/a..n/a] V | TIE | WIN |

- tickle-p95 scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 10, TIE 12, LOSE 0, VOID 2

- tickle-p95: WIN 10, TIE 12, LOSE 0, VOID 2

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

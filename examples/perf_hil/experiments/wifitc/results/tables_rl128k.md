# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## rel_large / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE | TIE | TIE |
| p50 ms | 41.90 [41.70..42.43] | 54.64 [54.46..55.06] | 55.19 [41.91..55.83] | 91.18 [88.43..91.29] | n/a [n/a..n/a] V | TIE | WIN | TIE | WIN |
| p99 ms | 51.74 [47.34..85.73] V | 62.95 [58.49..96.17] | 126 [118..156] | 91.85 [91.63..92.14] | n/a [n/a..n/a] V | VOID | VOID | WIN | TIE |

- tickle scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- tickle-pace scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE | TIE | TIE |
| p50 ms | 72.28 [56.94..84.49] V | 77.14 [69.15..105] | 99.74 [56.49..103] | 106 [104..106] | n/a [n/a..n/a] V | VOID | VOID | TIE | TIE |
| p99 ms | 151 [93.32..290] | 191 [106..267] V | 257 [212..462] | 119 [107..1152] | n/a [n/a..n/a] V | TIE | TIE | VOID | VOID |

- tickle scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- tickle-pace scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE | TIE | TIE |
| p50 ms | 349 [325..443] | 362 [361..365] | 307 [306..336] | 3562 [3521..3620] | n/a [n/a..n/a] V | TIE | WIN | LOSE | WIN |
| p99 ms | 452 [353..751] V | 383 [375..765] | 439 [406..460] | 3848 [3703..4317] | n/a [n/a..n/a] V | VOID | VOID | TIE | WIN |

- tickle scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- tickle-pace scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE | TIE | TIE |
| p50 ms | 538 [511..611] | 401 [385..417] | 439 [411..470] | 2327 [2109..2478] | n/a [n/a..n/a] V | LOSE | WIN | TIE | WIN |
| p99 ms | 1282 [728..1755] V | 601 [571..1568] V | 944 [634..1165] | 2749 [2681..4263] | n/a [n/a..n/a] V | VOID | VOID | VOID | VOID |

- tickle scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- tickle-pace scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 8, TIE 26, LOSE 2, VOID 12

- tickle: WIN 3, TIE 12, LOSE 1, VOID 8

- tickle-pace: WIN 5, TIE 14, LOSE 1, VOID 4

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## rel_large / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 45.82 [42.34..72.28] | 55.19 [41.91..55.83] | 91.18 [88.43..91.29] | n/a [n/a..n/a] V | TIE | WIN |
| p99 ms | 402 [118..1387] | 126 [118..156] | 91.85 [91.63..92.14] | n/a [n/a..n/a] V | TIE | LOSE |

- tickle-ad scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [33.3..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 169 [97.26..860] V | 99.74 [56.49..103] | 106 [104..106] | n/a [n/a..n/a] V | VOID | VOID |
| p99 ms | 1973 [801..3559] V | 257 [212..462] | 119 [107..1152] | n/a [n/a..n/a] V | VOID | VOID |

- tickle-ad scored/offered: 15/15, 15/15, 14/15, 5/15, 12/15, 15/15, 15/15, 15/15, 15/15, 7/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [26.7..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 541 [426..11424] V | 307 [306..336] | 3562 [3521..3620] | n/a [n/a..n/a] V | VOID | VOID |
| p99 ms | 1491 [1035..12616] V | 439 [406..460] | 3848 [3703..4317] | n/a [n/a..n/a] V | VOID | VOID |

- tickle-ad scored/offered: 15/15, 15/15, 4/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 20.0 [13.3..66.7] V | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | VOID | VOID |
| p50 ms | 1250 [481..8686] V | 439 [411..470] | 2327 [2109..2478] | n/a [n/a..n/a] V | VOID | VOID |
| p99 ms | 5092 [1427..18594] V | 944 [634..1165] | 2749 [2681..4263] | n/a [n/a..n/a] V | VOID | VOID |

- tickle-ad scored/offered: 2/15, 9/15, 3/15, 3/15, 3/15, 4/15, 2/15, 10/15, 6/15, 3/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 1, TIE 8, LOSE 1, VOID 14

- tickle-ad: WIN 1, TIE 8, LOSE 1, VOID 14

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

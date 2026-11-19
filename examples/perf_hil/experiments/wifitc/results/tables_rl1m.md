# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## rel_large / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE | TIE | TIE |
| p50 ms | 1127 [496..1993] V | 413 [412..415] | 818 [753..821] | 1082 [975..1086] | n/a [n/a..n/a] V | VOID | VOID | WIN | WIN |
| p99 ms | 2337 [1761..3893] | 423 [419..646] | 919 [918..1013] | 1101 [1000..1107] | n/a [n/a..n/a] V | LOSE | LOSE | WIN | WIN |

- tickle scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- tickle-pace scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | 100.0 [93.3..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE | TIE | TIE |
| p50 ms | 3255 [2773..3576] | 453 [436..468] | 866 [863..870] | 1220 [1191..1238] | n/a [n/a..n/a] V | LOSE | LOSE | WIN | WIN |
| p99 ms | 4653 [4070..6173] V | 596 [470..992] V | 1313 [963..1811] | 1272 [1263..1285] | n/a [n/a..n/a] V | VOID | VOID | VOID | VOID |

- tickle scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 14/15, 15/15, 15/15, 15/15

- tickle-pace scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | 93.3 [86.7..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 53.3 [53.3..60.0] | n/a [n/a..n/a] V | TIE | WIN | TIE | WIN |
| p50 ms | 5907 [4920..9184] | 2979 [2976..2982] | 2514 [2513..2567] | 31069 [26425..32511] | n/a [n/a..n/a] V | LOSE | WIN | LOSE | WIN |
| p99 ms | 8807 [6699..13799] | 3018 [2994..3219] | 2662 [2587..2719] | 32906 [30211..34317] | n/a [n/a..n/a] V | LOSE | WIN | LOSE | WIN |

- tickle scored/offered: 14/15, 15/15, 14/15, 15/15, 14/15, 13/15, 15/15, 14/15, 14/15, 15/15

- tickle-pace scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 16/16, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 9/15, 8/15, 8/15, 8/15, 8/15, 8/15, 9/15, 8/15, 8/15, 8/15

- zenohpico scored/offered: 

## rel_large / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | 70.0 [40.0..86.7] V | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 53.3 [53.3..60.0] | n/a [n/a..n/a] V | VOID | VOID | TIE | WIN |
| p50 ms | 14108 [9079..30411] V | 3160 [3110..3215] | 3803 [3696..5456] V | 29326 [22769..31696] | n/a [n/a..n/a] V | VOID | VOID | VOID | WIN |
| p99 ms | 23099 [14818..42937] V | 3511 [3195..3866] | 4658 [3846..6726] | 31829 [28293..34256] | n/a [n/a..n/a] V | VOID | VOID | TIE | WIN |

- tickle scored/offered: 11/15, 12/15, 12/15, 13/15, 8/15, 12/15, 6/15, 7/15, 6/15, 10/15

- tickle-pace scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 8/15, 9/15, 8/15, 8/15, 8/15, 8/15, 8/15, 8/15, 9/15, 8/15

- zenohpico scored/offered: 

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 15, TIE 12, LOSE 8, VOID 13

- tickle: WIN 3, TIE 5, LOSE 6, VOID 10

- tickle-pace: WIN 12, TIE 7, LOSE 2, VOID 3

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

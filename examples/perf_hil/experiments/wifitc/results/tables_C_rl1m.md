# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## rel_large / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`

| metric | tickle-p95 | cyclonedds | fastdds | zenohpico | tickle-p95 vs CycloneDDS | tickle-p95 vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 323 [322..326] | 818 [753..821] | 1082 [975..1086] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 334 [330..357] | 919 [918..1013] | 1101 [1000..1107] | n/a [n/a..n/a] V | WIN | WIN |

- tickle-p95 scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`

| metric | tickle-p95 | cyclonedds | fastdds | zenohpico | tickle-p95 vs CycloneDDS | tickle-p95 vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 367 [345..376] | 866 [863..870] | 1220 [1191..1238] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 488 [397..893] V | 1313 [963..1811] | 1272 [1263..1285] | n/a [n/a..n/a] V | VOID | VOID |

- tickle-p95 scored/offered: 15/15, 16/16, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`

| metric | tickle-p95 | cyclonedds | fastdds | zenohpico | tickle-p95 vs CycloneDDS | tickle-p95 vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 53.3 [53.3..60.0] | n/a [n/a..n/a] V | TIE | WIN |
| p50 ms | 2374 [2368..2385] | 2514 [2513..2567] | 31069 [26425..32511] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 2402 [2384..2424] | 2662 [2587..2719] | 32906 [30211..34317] | n/a [n/a..n/a] V | WIN | WIN |

- tickle-p95 scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 9/15, 8/15, 8/15, 8/15, 8/15, 8/15, 9/15, 8/15, 8/15, 8/15

- zenohpico scored/offered: 

## rel_large / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`

| metric | tickle-p95 | cyclonedds | fastdds | zenohpico | tickle-p95 vs CycloneDDS | tickle-p95 vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 53.3 [53.3..60.0] | n/a [n/a..n/a] V | TIE | WIN |
| p50 ms | 2497 [2466..2523] | 3803 [3696..5456] V | 29326 [22769..31696] | n/a [n/a..n/a] V | VOID | WIN |
| p99 ms | 2660 [2575..3381] | 4658 [3846..6726] | 31829 [28293..34256] | n/a [n/a..n/a] V | WIN | WIN |

- tickle-p95 scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 8/15, 9/15, 8/15, 8/15, 8/15, 8/15, 8/15, 8/15, 9/15, 8/15

- zenohpico scored/offered: 

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 15, TIE 6, LOSE 0, VOID 3

- tickle-p95: WIN 15, TIE 6, LOSE 0, VOID 3

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

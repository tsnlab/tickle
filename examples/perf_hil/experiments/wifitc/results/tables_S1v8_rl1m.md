# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## rel_large / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 342 [324..391] | 818 [753..821] | 1082 [975..1086] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 535 [488..1498] | 919 [918..1013] | 1101 [1000..1107] | n/a [n/a..n/a] V | TIE | TIE |

- tickle-ad scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 528 [424..773] | 866 [863..870] | 1220 [1191..1238] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 1147 [805..1950] V | 1313 [963..1811] | 1272 [1263..1285] | n/a [n/a..n/a] V | VOID | VOID |

- tickle-ad scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- zenohpico scored/offered: 

## rel_large / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 53.3 [53.3..60.0] | n/a [n/a..n/a] V | TIE | WIN |
| p50 ms | 2263 [2258..2273] | 2514 [2513..2567] | 31069 [26425..32511] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 2447 [2273..4977] | 2662 [2587..2719] | 32906 [30211..34317] | n/a [n/a..n/a] V | TIE | WIN |

- tickle-ad scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 9/15, 8/15, 8/15, 8/15, 8/15, 8/15, 9/15, 8/15, 8/15, 8/15

- zenohpico scored/offered: 

## rel_large / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [93.3..100.0] | 100.0 [100.0..100.0] | 53.3 [53.3..60.0] | n/a [n/a..n/a] V | TIE | WIN |
| p50 ms | 2797 [2354..3308] V | 3803 [3696..5456] V | 29326 [22769..31696] | n/a [n/a..n/a] V | VOID | VOID |
| p99 ms | 5944 [4144..12007] V | 4658 [3846..6726] | 31829 [28293..34256] | n/a [n/a..n/a] V | VOID | VOID |

- tickle-ad scored/offered: 15/15, 14/15, 15/15, 14/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- cyclonedds scored/offered: 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15, 15/15

- fastdds scored/offered: 8/15, 9/15, 8/15, 8/15, 8/15, 8/15, 8/15, 8/15, 9/15, 8/15

- zenohpico scored/offered: 

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 9, TIE 9, LOSE 0, VOID 6

- tickle-ad: WIN 9, TIE 9, LOSE 0, VOID 6

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

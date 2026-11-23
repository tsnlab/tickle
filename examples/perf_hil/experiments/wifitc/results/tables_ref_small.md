# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## be_small / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 97.9 [97.9..97.9] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.29 [10.29..10.29] V | n/a | n/a | n/a | n/a |
| p90 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.30 [10.30..10.30] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.31 [10.31..10.31] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 2840/2900

## be_small / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 95.8 [95.8..95.8] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 40.29 [40.29..40.29] V | n/a | n/a | n/a | n/a |
| p90 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 40.30 [40.30..40.30] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 40.31 [40.31..40.31] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 2778/2900

## be_small / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 98.4 [98.4..98.4] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.69 [10.69..10.69] V | n/a | n/a | n/a | n/a |
| p90 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.70 [10.70..10.70] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.71 [10.71..10.71] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 2854/2900

## be_small / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 90.5 [90.5..90.5] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 40.69 [40.69..40.69] V | n/a | n/a | n/a | n/a |
| p90 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 40.70 [40.70..40.70] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 40.71 [40.71..40.71] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 2625/2900

## rel_small / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 100.0 [100.0..100.0] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.30 [10.30..10.30] V | n/a | n/a | n/a | n/a |
| p90 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.31 [10.31..10.31] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 30.55 [30.55..30.55] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 2900/2900

## rel_small / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 88.3 [88.3..88.3] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 40.30 [40.30..40.30] V | n/a | n/a | n/a | n/a |
| p90 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 178 [178..178] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 1651 [1651..1651] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 2560/2900

## rel_small / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 100.0 [100.0..100.0] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.77 [10.77..10.77] V | n/a | n/a | n/a | n/a |
| p90 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 10.79 [10.79..10.79] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 31.59 [31.59..31.59] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 2900/2900

## rel_small / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 100.0 [100.0..100.0] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 90.91 [90.91..90.91] V | n/a | n/a | n/a | n/a |
| p90 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 5404 [5404..5404] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 7776 [7776..7776] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 2900/2900

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 0, TIE 0, LOSE 0, VOID 0

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## rel_large / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 0.0 [0.0..0.0] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | none [none..none] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | none [none..none] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 0/15

## rel_large / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`; tickle-pace = pacing_rate 22 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 0.0 [0.0..0.0] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | none [none..none] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | none [none..none] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 0/15

## rel_large / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 0.0 [0.0..0.0] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | none [none..none] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | none [none..none] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 0/15

## rel_large / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`; tickle-pace = pacing_rate 3 Mbit/s

| metric | tickle | tickle-pace | cyclonedds | fastdds | zenohpico | tickle vs CycloneDDS | tickle vs Fast DDS | tickle-pace vs CycloneDDS | tickle-pace vs Fast DDS |
|---|---:|---:|---:|---:|---:|---|---|---|---|
| delivery% | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | 0.0 [0.0..0.0] V | n/a | n/a | n/a | n/a |
| p50 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | none [none..none] V | n/a | n/a | n/a | n/a |
| p99 ms | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | n/a [n/a..n/a] V | none [none..none] V | n/a | n/a | n/a | n/a |

- tickle scored/offered: 

- tickle-pace scored/offered: 

- cyclonedds scored/offered: 

- fastdds scored/offered: 

- zenohpico scored/offered: 0/15

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 0, TIE 0, LOSE 0, VOID 0

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

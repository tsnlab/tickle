# tc-emulated Wi-Fi-like link, framework comparison, read by wtc_read.py (an emulation with tc netem on two Raspberry Pi, relative figures, not Wi-Fi performance)


## be_small / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 98.2 [97.6..98.4] | 97.9 [97.8..98.0] | 98.2 [97.8..98.3] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 10.28 [10.28..10.29] | 10.34 [10.34..10.34] | 10.41 [10.41..10.42] | n/a [n/a..n/a] V | WIN | WIN |
| p90 ms | 10.29 [10.28..10.29] | 10.34 [10.34..10.35] | 10.42 [10.42..10.43] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 10.30 [10.30..10.31] | 10.36 [10.36..10.37] | 10.44 [10.43..10.48] | n/a [n/a..n/a] V | WIN | WIN |

- tickle-ad scored/offered: 2842/2900, 2845/2900, 2853/2900, 2836/2900, 2841/2900, 2829/2900, 2854/2900, 2848/2900, 2849/2900, 2854/2900

- cyclonedds scored/offered: 2843/2900, 2841/2900, 2839/2900, 2835/2900

- fastdds scored/offered: 2845/2900, 2835/2900, 2852/2900, 2849/2900

- zenohpico scored/offered: 

## be_small / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 94.3 [92.3..95.5] | 94.6 [94.1..95.3] | 94.6 [93.5..94.8] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 40.28 [40.28..40.28] | 40.34 [40.34..40.34] | 40.41 [40.40..40.42] | n/a [n/a..n/a] V | WIN | WIN |
| p90 ms | 40.29 [40.28..40.29] | 40.34 [40.34..40.34] | 40.41 [40.41..40.42] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 40.30 [40.29..40.32] | 40.36 [40.36..40.36] | 40.43 [40.43..40.43] | n/a [n/a..n/a] V | WIN | WIN |

- tickle-ad scored/offered: 2757/2900, 2731/2900, 2708/2900, 2739/2900, 2718/2900, 2739/2900, 2676/2900, 2770/2900, 2710/2900, 2770/2900

- cyclonedds scored/offered: 2742/2900, 2728/2900, 2765/2900, 2745/2900

- fastdds scored/offered: 2749/2900, 2744/2900, 2711/2900, 2742/2900

- zenohpico scored/offered: 

## be_small / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 97.9 [97.8..98.7] | 98.1 [97.7..98.2] | 98.2 [98.0..98.5] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 10.70 [10.69..10.71] | 10.91 [10.91..10.91] | 11.24 [11.24..11.24] | n/a [n/a..n/a] V | WIN | WIN |
| p90 ms | 10.70 [10.70..10.71] | 10.91 [10.91..10.92] | 11.25 [11.25..11.25] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 10.72 [10.71..10.72] | 10.94 [10.94..10.94] | 11.27 [11.27..11.27] | n/a [n/a..n/a] V | WIN | WIN |

- tickle-ad scored/offered: 2840/2900, 2851/2900, 2847/2900, 2836/2900, 2836/2900, 2837/2900, 2861/2900, 2851/2900, 2838/2900, 2839/2900

- cyclonedds scored/offered: 2843/2900, 2844/2900, 2834/2900, 2849/2900

- fastdds scored/offered: 2852/2900, 2857/2900, 2846/2900, 2843/2900

- zenohpico scored/offered: 

## be_small / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 90.1 [88.3..91.4] | 89.8 [88.8..91.0] | 89.6 [88.5..91.7] | n/a [n/a..n/a] V | TIE | TIE |
| p50 ms | 40.70 [40.69..40.70] | 40.91 [40.91..40.91] | 41.24 [41.23..41.24] | n/a [n/a..n/a] V | WIN | WIN |
| p90 ms | 40.70 [40.70..40.71] | 40.92 [40.91..40.92] | 41.24 [41.24..41.25] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 40.71 [40.71..40.72] | 40.94 [40.94..40.95] | 41.26 [41.26..41.27] | n/a [n/a..n/a] V | WIN | WIN |

- tickle-ad scored/offered: 2591/2900, 2612/2900, 2615/2900, 2612/2900, 2613/2900, 2614/2900, 2652/2900, 2562/2900, 2592/2900, 2585/2900

- cyclonedds scored/offered: 2576/2900, 2640/2900, 2601/2900, 2609/2900

- fastdds scored/offered: 2658/2900, 2596/2900, 2567/2900, 2601/2900

- zenohpico scored/offered: 

## rel_small / M1

Link: `limit 100 delay 5ms rate 30mbit loss 1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 99.8 [99.4..99.9] | n/a [n/a..n/a] V | TIE | WIN |
| p50 ms | 10.28 [10.28..10.28] | 10.36 [10.36..10.36] | 98.71 [31.04..151] V | n/a [n/a..n/a] V | WIN | VOID |
| p90 ms | 10.28 [10.28..10.29] | 10.37 [10.37..10.38] | 728 [561..736] V | n/a [n/a..n/a] V | WIN | VOID |
| p99 ms | 30.56 [30.53..30.58] | 30.72 [30.71..30.74] | 1188 [814..1224] V | n/a [n/a..n/a] V | WIN | VOID |

- tickle-ad scored/offered: 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900

- cyclonedds scored/offered: 2900/2900, 2900/2900, 2900/2900, 2900/2900

- fastdds scored/offered: 2896/2900, 2882/2900, 2897/2900, 2895/2900

- zenohpico scored/offered: 

## rel_small / M2

Link: `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 99.5 [98.9..99.7] | n/a [n/a..n/a] V | TIE | WIN |
| p50 ms | 40.28 [40.28..40.28] | 40.36 [40.36..40.36] | 150 [40.44..211] V | n/a [n/a..n/a] V | WIN | VOID |
| p90 ms | 70.87 [50.66..80.57] V | 80.68 [70.76..80.78] | 761 [695..782] | n/a [n/a..n/a] V | VOID | VOID |
| p99 ms | 149 [111..382] | 176 [161..261] V | 1032 [961..1055] | n/a [n/a..n/a] V | VOID | WIN |

- tickle-ad scored/offered: 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900

- cyclonedds scored/offered: 2900/2900, 2900/2900, 2900/2900, 2900/2900

- fastdds scored/offered: 2892/2900, 2884/2900, 2888/2900, 2867/2900

- zenohpico scored/offered: 

## rel_small / S1

Link: `limit 40 delay 5ms rate 4mbit loss 1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 99.9 [99.7..99.9] | n/a [n/a..n/a] V | TIE | WIN |
| p50 ms | 10.70 [10.70..10.70] | 11.04 [11.04..11.04] | 99.33 [42.69..118] V | n/a [n/a..n/a] V | WIN | VOID |
| p90 ms | 10.71 [10.70..10.71] | 11.05 [11.05..11.06] | 638 [606..723] | n/a [n/a..n/a] V | WIN | WIN |
| p99 ms | 31.40 [31.24..31.41] | 32.13 [31.99..54.74] V | 1084 [911..1500] V | n/a [n/a..n/a] V | VOID | VOID |

- tickle-ad scored/offered: 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900

- cyclonedds scored/offered: 2900/2900, 2900/2900, 2900/2900, 2900/2900

- fastdds scored/offered: 2897/2900, 2892/2900, 2897/2900, 2896/2900

- zenohpico scored/offered: 

## rel_small / S2

Link: `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%`

| metric | tickle-ad | cyclonedds | fastdds | zenohpico | tickle-ad vs CycloneDDS | tickle-ad vs Fast DDS |
|---|---:|---:|---:|---:|---|---|
| delivery% | 100.0 [100.0..100.0] | 100.0 [100.0..100.0] | 98.4 [96.8..99.7] | n/a [n/a..n/a] V | TIE | WIN |
| p50 ms | 40.70 [40.70..40.70] | 41.04 [41.04..41.04] | 319 [313..405] V | n/a [n/a..n/a] V | WIN | VOID |
| p90 ms | 91.65 [91.40..111] | 110 [105..124] | 855 [819..883] | n/a [n/a..n/a] V | TIE | WIN |
| p99 ms | 193 [132..385] V | 288 [260..402] V | 1194 [1016..1486] | n/a [n/a..n/a] V | VOID | VOID |

- tickle-ad scored/offered: 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900, 2900/2900

- cyclonedds scored/offered: 2900/2900, 2900/2900, 2900/2900, 2900/2900

- fastdds scored/offered: 2890/2900, 2837/2900, 2869/2900, 2806/2900

- zenohpico scored/offered: 

## Counts (TickLE vs CycloneDDS and Fast DDS, per metric-cell-arm; zenoh-pico never scored)

WIN 38, TIE 13, LOSE 0, VOID 13

- tickle-ad: WIN 38, TIE 13, LOSE 0, VOID 13

Columns: median [min..max] over the runs of a framework (A and A2 pooled); `V` = noise-VOID (|median A2 - median A| > 10% of median A).

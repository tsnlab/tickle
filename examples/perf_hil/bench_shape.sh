#!/usr/bin/env bash
# The payload array size of a HIL Bench shape, read the same way everywhere it is read (2026-09-25).
# Sourced by cyclonedds/build.sh, fastdds/build.sh and .github/scripts/check_bench_shapes.sh.
#
# Why this exists. The P2 resize (1388 -> 1292 bytes) never reached either DDS harness on the rig:
# each build.sh regenerated its binding only when the generated file was *missing*, the generated
# directories are gitignored so `git clean -fdq` kept them, and the shape gate checked TickLE's
# generated header but neither vendor's. Every P2 comparison after the resize therefore ran a
# 1292-byte TickLE against a 1388-byte CycloneDDS and FastDDS - while all three RESULT lines said
# 1292, because BENCH_SAMPLE_BYTES was read from the IDL rather than from what was compiled.
# Plan found it when FastDDS split a P2 sample the framing model said had 12.4 bytes of margin.
#
# Each function prints the array length, or nothing if the file does not have exactly one.

# idl/pN/Bench.idl: `octet payload[N];`
bench_idl_payload() {
    sed -n 's/.*octet *payload\[\([0-9]*\)\].*/\1/p' "$1" | sort -u | awk 'NR==1{v=$0} END{if (NR==1) print v}'
}

# fastddsgen's Bench.h: the payload is a std::array<uint8_t, N> in the member and every accessor.
bench_fastdds_gen_payload() {
    sed -n 's/.*std::array<uint8_t, *\([0-9]*\)>.*/\1/p' "$1" | sort -u | awk 'NR==1{v=$0} END{if (NR==1) print v}'
}

# idlc's Bench.h: `uint8_t payload[N];` in the struct.
bench_cyclonedds_gen_payload() {
    sed -n 's/^ *uint8_t payload\[\([0-9]*\)\];.*/\1/p' "$1" | sort -u | awk 'NR==1{v=$0} END{if (NR==1) print v}'
}

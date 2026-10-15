#!/bin/bash
# Seeds 1..N of tests/test_hybrid_send.c against one source tree: the link's loss draws (sim_rng, fixed at 11 in the
# test) are seeded per run, and every call of expect_intact()/expect_intact_or_given_up() reports what the reader got.
# Used for the KEEP_LAST repair A/B (2026-10-15): run once per tree, compare the summaries.
#
#   hybrid_seed_sweep.sh <tree> <label> <out_dir> [first_seed] [last_seed]
#
# Writes <out_dir>/<label>.tsv, one row per expect call:
#   seed  kind (intact | given_up)  test function  want  delivered  gap_abandoned  gap_evicted
# and <out_dir>/<label>.verdicts: seed rc. Each run is a separate binary built with -DHYB_SEED; built once per seed
# from a copy, never from the tree's own file (a running script or a source file being edited is not read twice).
#
# Reading rules (pre-registered): a seed fails or passes by the test binary's own exit code; "delivered" sums over
# the expect calls of the test functions both trees have, never a tree's own new tests.
set -u
tree=$1
label=$2
out=$3
first=${4:-1}
last=${5:-100}
mkdir -p "$out/$label.build"
src="$out/$label.build/test_hybrid_send.c"
cp "$tree/tests/test_hybrid_send.c" "$src"
# Seed the link, and report every expectation's inputs on one machine-readable line.
python3 - "$src" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
assert s.count("    sim_rng = 11;\n") == 1, "sim_rng seed line not found"
s = s.replace("    sim_rng = 11;\n", "    sim_rng = HYB_SEED;\n")
a = "static void expect_intact(int reader, int want) {\n"
assert s.count(a) == 1
s = s.replace(a, "#define expect_intact(r, w) expect_intact_at(r, w, __func__)\nstatic void expect_intact_at(int reader, int want, const char* line) {\n"
              "    printf(\"SWEEP intact %s %d %d %u %u\\n\", line, want, readers[reader].delivered,\n"
              "           readers[reader].sub.gap_abandoned, readers[reader].sub.gap_evicted);\n")
a = "static void expect_intact_or_given_up(int reader, int want) {\n"
if s.count(a) == 1:
    s = s.replace(a, "#define expect_intact_or_given_up(r, w) expect_intact_or_given_up_at(r, w, __func__)\n"
                  "static void expect_intact_or_given_up_at(int reader, int want, const char* line) {\n"
                  "    printf(\"SWEEP given_up %s %d %d %u %u\\n\", line, want, readers[reader].delivered,\n"
                  "           readers[reader].sub.gap_abandoned, readers[reader].sub.gap_evicted);\n")
open(p, 'w').write(s)
EOF
# The source refers to its siblings by relative path (../src/tickle.c, test_common.h): build next to them.
cp "$src" "$tree/tests/.sweep_$label.c"
: > "$out/$label.tsv"
: > "$out/$label.verdicts"
for seed in $(seq "$first" "$last"); do
    bin="$out/$label.build/t_$seed"
    cc -I"$tree/include" -I"$tree/src" -O1 -g -w -DHYB_SEED="$seed" -o "$bin" "$tree/tests/.sweep_$label.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" -lm || { echo "$seed build-failed" >> "$out/$label.verdicts"; continue; }
    (ulimit -v 2097152; timeout 600 "$bin" > "$out/$label.build/run_$seed.log" 2>&1)
    rc=$?
    echo "$seed $rc" >> "$out/$label.verdicts"
    python3 "$(dirname "$0")/hybrid_seed_sweep_rows.py" "$seed" "$out/$label.build/run_$seed.log" >> "$out/$label.tsv"
    rm -f "$bin"
done
rm -f "$tree/tests/.sweep_$label.c"
echo "SWEEP_DONE $label"

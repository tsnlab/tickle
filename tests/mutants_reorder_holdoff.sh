#!/usr/bin/env bash
# The reorder hold-off (ab/reorder-holdoff, stage S3): each mutant of src/tickle.c must make tests/test_reorder_holdoff.c
# fail, and the unmutated control must pass - a test that has never been seen to fail decides nothing. Each mutation is
# applied to a copy (never in place) and must change exactly one occurrence, or the arm reports MUTATION_NOT_APPLIED and
# counts as a survivor. Every child runs under a 120 s timeout with 2 GB of address space.
#
# Usage: tests/mutants_reorder_holdoff.sh   One line per arm, then MUTANTS_REORDER_HOLDOFF_END; exits non-zero if the
#        control fails or any mutant survives.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/src" "$OUT/tests"
cp "$ROOT/tests/test_reorder_holdoff.c" "$OUT/tests/t.c"
survivors=0

# arm NAME FROM TO: FROM replaced once by TO (both empty: the control). Prints the arm's verdict.
arm() {
    local name=$1 from=$2 to=$3
    cp "$ROOT/src/tickle.c" "$OUT/src/tickle.c"
    if [ -n "$from" ]; then
        if ! python3 -I - "$OUT/src/tickle.c" "$from" "$to" <<'EOF'; then
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(1)
open(path, "w").write(text.replace(old, new, 1))
EOF
            echo "ARM $name: MUTATION_NOT_APPLIED"
            survivors=$((survivors + 1))
            return
        fi
    fi
    if ! "$CC" -O0 -g -w -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/tests" -o "$OUT/t_$name" "$OUT/tests/t.c" \
        "$ROOT/src/encoding.c" "$ROOT/src/log.c" -lpthread -lm >"$OUT/build_$name.log" 2>&1; then
        echo "ARM $name: BUILD_FAILED"
        tail -5 "$OUT/build_$name.log"
        survivors=$((survivors + 1))
        return
    fi
    (
        ulimit -v 2097152
        timeout 120 "$OUT/t_$name" >"$OUT/run_$name.log" 2>&1
    )
    local rc=$?
    local fails
    fails=$(grep -c 'FAIL' "$OUT/run_$name.log")
    if [ "$name" = control ]; then
        echo "ARM control: rc=$rc failures=$fails $( [ "$rc" -eq 0 ] && echo PASS || echo CONTROL_FAILED)"
        [ "$rc" -eq 0 ] || survivors=$((survivors + 1))
    else
        echo "ARM $name: rc=$rc failures=$fails $( [ "$rc" -ne 0 ] && echo KILLED || echo SURVIVED)"
        [ "$rc" -ne 0 ] || survivors=$((survivors + 1))
    fi
}

arm control "" ""
# (i): auto's gate held open from the start - the no-reorder logs must then differ from off.
arm gate_always_open '} else if (node->holdoff_mode == tt_HOLDOFF_MODE_AUTO && proxy->holdoff_gate) {' \
    '} else if (node->holdoff_mode == tt_HOLDOFF_MODE_AUTO) {'
# Off against main: the new-gap ACKNACK never sent - off's digest must then differ from main's.
arm off_n2_never_sent 'if (retry_already_armed && new_gap.low_bit >= 0 && !held) {' \
    'if (retry_already_armed && new_gap.low_bit >= 0 && held) {'
# N2: a held gap NACKed by the new-gap ACKNACK anyway.
arm n2_not_suppressed 'if (retry_already_armed && new_gap.low_bit >= 0 && !held) {' \
    'if (retry_already_armed && new_gap.low_bit >= 0) {'
# N3: the retry names the held gaps.
arm n3_skip_dropped '#else
    send_acknack_skipping(node, proxy, 0, highest, held, held_runs);' '#else
    send_acknack_skipping(node, proxy, 0, highest, held, 0);'
# N4: the HEARTBEAT's tail not held, and the answer naming held gaps.
arm n4_tail_not_held 'holdoff_hold_revealed(node, proxy, hb_before);' '(void)hb_before;'
arm n4_answer_skip_dropped \
    'skip_count += holdoff_skip(node, proxy, &skips[skip_count]); // nor what the reorder hold-off holds (N4)' \
    '// dropped (N4)'
# Nothing is ever held.
arm never_held 'uint64_t hold = proxy->collided ? 0U : holdoff_time_ns(node, proxy);' 'uint64_t hold = 0U;'
# The window-half rule gone: a held gap waits for its time however far the stream moved on.
arm no_early_release 'if (behind >= proxy_window_bits(proxy) / 2U || proxy->collided) {' 'if (proxy->collided) {'
# E1 at the watermark ignored: the gate never opens.
arm probe_e1_ignored 'holdoff_note_probe_original(node, proxy, seq_no); // the original' '(void)seq_no; // the original'
# The gate closes one clean recovery late.
arm gate_closes_late 'if (proxy->holdoff_clean >= HOLDOFF_GATE_CLOSE) {' 'if (proxy->holdoff_clean > HOLDOFF_GATE_CLOSE) {'
# B's distance rule gone; D's quarter gone.
arm b_distance_ignored 'due_at = node->holdoff_policy == tt_HOLDOFF_POLICY_B ? 0U : hold / 4U;' \
    'due_at = node->holdoff_policy == tt_HOLDOFF_POLICY_B ? hold : hold / 4U;'
arm d_quarter_ignored 'due_at = node->holdoff_policy == tt_HOLDOFF_POLICY_B ? 0U : hold / 4U;' \
    'due_at = node->holdoff_policy == tt_HOLDOFF_POLICY_B ? 0U : hold;'
# A held gap released a microsecond before its hold has passed.
arm released_early_by_1us 'if (age >= hold) {' 'if (age + tt_MICROSECOND >= hold) {'
# The options: TSN not off by default, the fold not preferring auto, a refusal let through.
arm tsn_default_auto 'bool ordered = link->profile == tt_LINK_PROFILE_T1S || link->profile == tt_LINK_PROFILE_TSN;' \
    'bool ordered = link->profile == tt_LINK_PROFILE_T1S;'
arm fold_ignores_auto 'if (link == tt_REORDER_HOLDOFF_AUTO) {' 'if (false) {'
arm reliable_max_not_refused 'return reject_option("reliable.reorder_holdoff",' '(void)reject_option("reliable.reorder_holdoff",'
echo "MUTANTS_REORDER_HOLDOFF_END survivors=$survivors"
[ "$survivors" -eq 0 ]

#!/usr/bin/env bash
# w1_mutants.sh - tests/test_w1_route.c against mutants of the W1 prototype (rmw_tickle/WIRE_PLAN.md 9.1). Each
# mutant removes one check the tests claim to pin; the test binary must FAIL against every mutant, and PASS against
# the unmutated control built the same way. A mutant whose sed matches nothing is reported as such, not as a pass.
#
# Usage: w1_mutants.sh     Exits 0 only if the control passes and every mutant fails.
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK="$(mktemp -d /tmp/w1_mutants.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# name|sed expression applied to src/tickle.c
MUTANTS=(
    'control|'
    'drop_on_miss|s/const struct tt_HandleEntry\* known = handle_directory_find(node, header->source, handle);/const struct tt_HandleEntry* known = NULL;/'
    'no_generation|s/if (route->generation == node->route_generation \&\& route->source/if (route->source/'
    'no_tag|s/ \&\& route->source == header->source \&\& route->handle == handle) {/) {/'
    'fill_multi|s/if (ctx.fill_count == 1 \&\& ctx.fill_proxy != NULL) {/if (ctx.fill_count >= 1 \&\& ctx.fill_proxy != NULL) {/'
    'start_short|s/if (pub->w1_since_match < tt_W1_LONG_EVERY) {/if (false) {/'
    'ignore_acks|s/if (!pub->peer_acks\[i\].w1_acked) {/if (false) {/'
)

verdict=0
for entry in "${MUTANTS[@]}"; do
    name="${entry%%|*}"
    expr="${entry#*|}"
    tree="$WORK/$name"
    mkdir -p "$tree/examples"
    cp -r "$REPO/include" "$REPO/src" "$REPO/tests" "$tree/"
    for d in perf ping_pong set_bool uint64; do cp -r "$REPO/examples/$d" "$tree/examples/"; done
    if [ -n "$expr" ]; then
        sed -i "$expr" "$tree/src/tickle.c"
        if cmp -s "$REPO/src/tickle.c" "$tree/src/tickle.c"; then
            echo "$name: the sed matched nothing - NOT A TEST"
            verdict=1
            continue
        fi
    fi
    if ! cc -I"$tree/include" -I"$tree/src" -I"$tree/examples/perf" -I"$tree/examples/ping_pong" \
        -I"$tree/examples/set_bool" -I"$tree/examples/uint64" -O0 -g -o "$tree/test_w1_route" \
        "$tree/tests/test_w1_route.c" "$tree/src/encoding.c" "$tree/src/log.c" -lpthread -lm 2>"$tree/build.log"; then
        echo "$name: did not build - NOT A TEST"
        verdict=1
        continue
    fi
    if "$tree/test_w1_route" >"$tree/run.log" 2>&1; then
        result=pass
    else
        result=fail
    fi
    failed=$(grep -c 'expected\|FAIL' "$tree/run.log")
    if [ "$name" = control ]; then
        [ "$result" = pass ] || verdict=1
        echo "control: $result"
    else
        [ "$result" = fail ] || verdict=1
        echo "$name: $result ($failed failed check(s))"
    fi
done
echo "VERDICT=$verdict"
exit "$verdict"

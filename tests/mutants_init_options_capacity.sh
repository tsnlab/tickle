#!/usr/bin/env bash
# tt_InitOptions bundle 9 (ROADMAP "Context options"), the capacity group: every check tt_Context_create_with() makes on
# struct tt_Capacity is decided by a test, and every field is read where a table is filled.
#   - check mutants turn one refusal into a log line that lets creation go on (its `return reject_option(...)` becomes
#     `(void)reject_option(...)`);
#   - reader mutants put the compiled size back where a table is filled up to the context's field (or an endpoint's
#     peer back on the path that ignores it), so the field is written and never read there.
# Each rebuilds the test that covers it against the mutant - tests/test_init_options_capacity.c, or for
# publisher_claims tests/test_publish_claim.c and for large_retained tests/test_large_sample.c - and expects it to
# FAIL. A mutant that survives is a check, or a reader, no test can tell from its absence.
#
# The controls first: each test built from the unmutated tickle.c must pass, or a failure below says nothing. The source
# is mutated in a copy, never in place; every child runs with 2 GB of address space, 10 MB of output and 5 minutes.
#
# Usage: tests/mutants_init_options_capacity.sh     Prints one line per mutant and exits non-zero if any survives.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# run TEST TICKLE_C: builds tests/TEST.c against the given tickle.c and runs it; its exit status.
run() {
    local test=$1 tickle_c=$2 bin="$TMP/$1"
    "$CC" -O0 -Wall -Wextra -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/tests" -DTICKLE_C="\"$tickle_c\"" \
        -o "$bin" "$ROOT/tests/$test.c" "$ROOT/src/encoding.c" "$ROOT/src/log.c" -lpthread -lm 2> "$TMP/build.log" || {
        cat "$TMP/build.log" >&2
        return 2
    }
    (
        cd "$ROOT/platform/linux"
        ulimit -v 2097152
        ulimit -f 10240
        timeout 300 "$bin" > "$TMP/run.log" 2>&1
    )
}

failed=0
for test in test_init_options_capacity test_publish_claim test_large_sample; do
    if run "$test" "$ROOT/src/tickle.c"; then
        printf 'PASS  control   %s passes on the unmutated source\n' "$test"
    else
        printf 'FAIL  control   %s fails on the unmutated source\n' "$test"
        failed=1
    fi
done
[ "$failed" = 0 ] || exit 1

# mutant TEST NAME OLD NEW: OLD (which must occur in tickle.c exactly once) becomes NEW.
mutant() {
    local test=$1 name=$2 old=$3 new=$4
    python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" "$old" "$new" <<'EOF'
import sys
text = open(sys.argv[1]).read()
old, new = sys.argv[3], sys.argv[4]
if text.count(old) != 1:
    sys.exit(f"anchor {old!r} matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, new))
EOF
    local status=0
    run "$test" "$TMP/mutant_tickle.c" || status=$?
    if [ "$status" = 1 ]; then
        printf 'PASS  killed    %s\n' "$name"
    else
        printf 'FAIL  survived  %s (%s exit %s)\n' "$name" "$test" "$status"
        failed=1
    fi
}

# check ANCHOR: the refusal starting `reject_option(ANCHOR` no longer returns.
check() {
    mutant test_init_options_capacity "check $1" "return reject_option($1" "(void)reject_option($1"
}

check '"capacity.ack_entries"'
check '"capacity.peers", "exceeds'
check '"capacity.peers", "is not above'
check '"capacity.request_history"'
check '"capacity.publisher_claims"'
check '"capacity.large_retained"'

C=test_init_options_capacity
mutant $C "resolve in node_init_locks()" \
    "    resolve_hybrid_options(&node->opts.hybrid_transport);
    resolve_capacity_options(&node->opts.capacity);" \
    "    resolve_hybrid_options(&node->opts.hybrid_transport);"
mutant $C "reader ack_entries" \
    "int limit = (int)pub->node->opts.capacity.ack_entries;" \
    "int limit = tt_MAX_ACK_ENTRIES;"
mutant $C "reader peers (endpoint peers)" \
    "uint8_t limit = node->opts.capacity.peers;" \
    "uint8_t limit = tt_MAX_PEER_COUNT;"
mutant $C "reader peers (a Subscriber announced)" \
    "if (upsert_endpoint_peer(node, pub->peers, ctx->header->source, ctx->sender_ip, ctx->sender_port)) {" \
    "if (upsert_peer(pub->peers, ctx->header->source, ctx->sender_ip, ctx->sender_port)) {"
mutant $C "reader peers (an ACKNACK)" \
    "(void)upsert_endpoint_peer(node, pub->peers, source, sender_ip, sender_port);" \
    "(void)upsert_peer(pub->peers, source, sender_ip, sender_port);"
mutant $C "reader peers (a Server announced)" \
    "(void)upsert_endpoint_peer(node, client->peers, ctx->header->source, ctx->sender_ip, ctx->sender_port);" \
    "(void)node;
    (void)upsert_peer(client->peers, ctx->header->source, ctx->sender_ip, ctx->sender_port);"
mutant $C "reader peers (writer proxies)" \
    "int limit = sub->node != NULL ? (int)sub->node->opts.capacity.peers : tt_MAX_PEER_COUNT;" \
    "int limit = tt_MAX_PEER_COUNT;"
mutant $C "reader request_history" \
    "proxy->request_next = next < node->opts.capacity.request_history ? next : 0U;" \
    "proxy->request_next = next < tt_RELIABLE_REQUEST_HISTORY ? next : 0U;"
mutant test_publish_claim "reader publisher_claims" \
    "if (pub->claims_out >= node->opts.capacity.publisher_claims) {" \
    "if (pub->claims_out >= tt_PUBLISHER_CLAIMS) {"
mutant test_large_sample "reader large_retained (records kept)" \
    "uint32_t limit = pub->node->opts.capacity.large_retained;" \
    "uint32_t limit = tt_LARGE_RETAINED;"
mutant test_large_sample "reader large_retained (KEEP_ALL admits)" \
    "if (pub->large_count >= pub->node->opts.capacity.large_retained) {" \
    "if (pub->large_count >= tt_LARGE_RETAINED) {"

exit "$failed"

#!/usr/bin/env bash
# tt_InitOptions bundle 6, the socket group (struct tt_SocketOptions): every check tt_Context_create_with() makes on it
# is decided by a test, and so is every read of a field in the Linux HAL - and the environment variables' precedence.
#
#   check mutants   one refusal in tickle.c turned into a log line that lets creation go on (`return reject_option(...)`
#                   becomes `(void)reject_option(...)`);
#   reader mutants  one place in hal_linux.c that reads a field (or the environment) made to ignore it.
# Each rebuilds the covering test - tests/test_init_options_socket.c on the real HAL, or
# tests/test_init_options_socket_noring.c for the io_uring-less build - against the mutant and expects it to FAIL
# (exit 1). A mutant that survives is a check, or a read, that no test can tell from its absence.
#
# The controls first: both tests built from the unmutated sources must pass, or a failure below says nothing. The
# drain's mutants need io_uring: on a system that refuses it the socket test skips the drain (and says so), and those
# mutants are reported SKIP here, not passed.
#
# Usage: tests/mutants_init_options_socket.sh     Prints one line per mutant and exits non-zero if any survives.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# run TEST TICKLE_C HAL_C: builds tests/TEST.c against the given sources and runs it; its exit status (2: no build).
run() {
    local test=$1 tickle_c=$2 hal_c=$3 bin="$TMP/$1"
    "$CC" -O0 -Wall -Wextra -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DHAL_LINUX_C="\"$hal_c\"" -o "$bin" "$ROOT/tests/$test.c" "$ROOT/src/encoding.c" "$ROOT/src/log.c" -lm \
        2> "$TMP/build.log" || {
        cat "$TMP/build.log" >&2
        return 2
    }
    (cd "$ROOT/platform/linux" && "$bin" > "$TMP/run.log" 2>&1)
}

failed=0
for test in test_init_options_socket test_init_options_socket_noring; do
    if run "$test" "$ROOT/src/tickle.c" "$ROOT/src/hal_linux.c"; then
        printf 'PASS  control   %s passes on the unmutated source\n' "$test"
    else
        printf 'FAIL  control   %s fails on the unmutated source\n' "$test"
        failed=1
    fi
done
[ "$failed" = 0 ] || exit 1
# Whether this system gave the socket test a drain: its control run again, its own log read.
drain=1
run test_init_options_socket "$ROOT/src/tickle.c" "$ROOT/src/hal_linux.c" || true
grep -q 'SKIP drain' "$TMP/run.log" && drain=0

# mutate FILE TEST NAME OLD NEW: src/FILE with OLD (exactly once) replaced by NEW; TEST must fail.
mutate() {
    local file=$1 test=$2 name=$3
    python3 - "$ROOT/src/$file" "$TMP/mutant_$file" "$4" "$5" <<'EOF'
import sys
text = open(sys.argv[1]).read()
if text.count(sys.argv[3]) != 1:
    sys.exit(f"anchor {sys.argv[3]!r} matched {text.count(sys.argv[3])} times, not 1")
open(sys.argv[2], "w").write(text.replace(sys.argv[3], sys.argv[4]))
EOF
    local tickle_c="$ROOT/src/tickle.c" hal_c="$ROOT/src/hal_linux.c" status=0
    [ "$file" = tickle.c ] && tickle_c="$TMP/mutant_tickle.c"
    [ "$file" = hal_linux.c ] && hal_c="$TMP/mutant_hal_linux.c"
    run "$test" "$tickle_c" "$hal_c" || status=$?
    if [ "$status" = 1 ]; then
        printf 'PASS  killed    %s\n' "$name"
    else
        printf 'FAIL  survived  %s (%s exit %s)\n' "$name" "$test" "$status"
        failed=1
    fi
}

check() { # check TEST ANCHOR
    mutate tickle.c "$1" "check $2" "return reject_option($2" "(void)reject_option($2"
}

check test_init_options_socket '"socket.buffer_size"'
check test_init_options_socket '"socket.udp_offload"'
check test_init_options_socket '"socket.udp_csum0"'
check test_init_options_socket '"socket.rx_direct"'
check test_init_options_socket '"socket.rx_lazy_timer"'
check test_init_options_socket '"socket.rx_lazy_adapt"'
check test_init_options_socket '"socket.rx_nonblock"'
check test_init_options_socket '"socket.rx_hint", "is not'
check test_init_options_socket '"socket.drain_buffers"'
check test_init_options_socket '"socket.drain_bytes_max"'
check test_init_options_socket '"socket.drain_holdoff_ns"'
check test_init_options_socket_noring '"socket.rx_hint", "needs a build'

hal() { # hal NAME OLD NEW
    mutate hal_linux.c test_init_options_socket "$1" "$2" "$3"
}
hal "switch: the option ignored, always on" "    return option != tt_SWITCH_OFF;" "    return true;"
hal "switch: the environment ignored" "    if (setting != NULL) {
        return strcmp(setting, \"0\") != 0;" "    if (false) {
        return strcmp(setting, \"0\") != 0;"
hal "switch: an environment variable set to 1 reads off" "        return strcmp(setting, \"0\") != 0;" \
    "        return strcmp(setting, \"1\") == 0 && false;"
for pair in TT_UDP_OFFLOAD:udp_offload TT_UDP_CSUM0:udp_csum0 TT_RX_DIRECT:rx_direct TT_RX_LAZY_TIMER:rx_lazy_timer \
    TT_RX_LAZY_ADAPT:rx_lazy_adapt TT_RX_NONBLOCK:rx_nonblock; do
    env=${pair%%:*}
    field=${pair#*:}
    hal "read: $field ignored" "socket_switch(\"$env\", node->opts.socket.$field)" \
        "socket_switch(\"$env\", tt_SWITCH_DEFAULT)"
done
hal "read: buffer_size ignored on the well-known socket" "int buffer_size = socket_buffer_size(node);" \
    "int buffer_size = tt_SOCKET_BUFFER_SIZE;"
hal "read: buffer_size ignored on the data socket" "    buffer_size = socket_buffer_size(node);
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.data_sock" "    buffer_size = tt_SOCKET_BUFFER_SIZE;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.data_sock"
hal "read: rx_hint URING does not fail creation" \
    "if (!uring_setup(node) && node->opts.socket.rx_hint == tt_SOCKET_RX_HINT_URING) {" \
    "if (!uring_setup(node) && false) {"
hal "read: poll.receive_timeout ignored by the idle recheck" \
    "node->opts.poll.receive_timeout != 0 ? node->opts.poll.receive_timeout : TT_RX_IDLE_RECHECK_NS;" \
    "TT_RX_IDLE_RECHECK_NS;"
if [ "$drain" = 1 ]; then
    hal "read: drain_bytes ignored" "node->hal.drain_budget = drain_bytes(node);" \
        "node->hal.drain_budget = tt_RX_DRAIN_BYTES;"
    hal "read: drain_buffers ignored" "uint32_t count = drain_buffers(node);" "uint32_t count = tt_RX_DRAIN_BUFFERS;"
    hal "read: drain_bytes_max ignored" "const uint32_t bytes_max = drain_bytes_max(node);" \
        "const uint32_t bytes_max = tt_RX_DRAIN_BYTES_MAX;"
    hal "read: drain_holdoff_ns ignored" "tt_get_ns() + drain_holdoff_ns(node);" \
        "tt_get_ns() + tt_RX_DRAIN_HOLDOFF_NS;"
else
    printf 'SKIP  drain     io_uring refused here: the drain mutants were not run\n'
fi

exit "$failed"

#!/bin/bash
# Mutants for tests/test_segment_skip_hybrid.c: each removes one half of "a superseded sample is not a missed one"
# (hybrid_rx_superseded() / hybrid_rx_missed(), src/tickle.c), and the test binary must fail on every one. A control
# run on the unmodified source must pass. Restores the source on exit.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/tickle.c"
BIN="$ROOT/platform/linux/obj/debug/tests/test_segment_skip_hybrid"
BACKUP=$(mktemp)
cp -p "$SRC" "$BACKUP"
trap 'cp "$BACKUP" "$SRC"; touch "$SRC"; rm -f "$BACKUP"' EXIT

run() {
    touch "$SRC" # a restored copy keeps its mtime; make must not reuse the mutant's binary
    make -s -C "$ROOT/platform/linux" "obj/debug/tests/test_segment_skip_hybrid" >/dev/null 2>&1 || { echo "BUILD_FAIL"; return; }
    if "$BIN" >/dev/null 2>&1; then echo "PASS"; else echo "FAIL"; fi
}

mutate() { # name, old, new
    cp "$BACKUP" "$SRC"
    python3 - "$SRC" "$2" "$3" <<'EOF'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
assert s.count(old) == 1, old
open(p, "w").write(s.replace(old, new, 1))
EOF
    printf '%-28s %s\n' "$1" "$(run)"
}

cp "$BACKUP" "$SRC"
printf '%-28s %s\n' "control" "$(run)"
mutate "no-subtraction" "    unseen = unseen > passed ? unseen - passed : 0U;" "    (void)passed;"
mutate "no-credit" "            entry->superseded += samples;" "            (void)samples;"
mutate "credit-kept-across-jumps" "    writer->superseded = 0; // they lie" "    (void)0; // they lie"
mutate "limit-before-subtraction" "    if (count != writer->count || jump % count != 0) {" \
    "    if (count != writer->count || jump % count != 0 || jump / count > HYBRID_MISSED_MAX) {"

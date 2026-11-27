#!/bin/bash
# Builds algosim from the unmodified library header plus two knobs (the start rate ceiling and the round trip sample
# decimation of the Pi). Usage: build.sh <repo root> <out dir>
set -euo pipefail
ROOT=$1
OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/inc/tickle"
python3 - "$ROOT/include/tickle/pacing_adaptive.h" "$OUT/inc/tickle/pacing_adaptive.h" <<'PY'
import sys
s = open(sys.argv[1]).read()
def sub(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)
sub("struct ap_filter {", "static uint64_t g_ap_start_max = 8000000ULL; // algosim: the start rate ceiling (AP_START_MAX_BPS)\nstatic unsigned g_ap_sparse; // algosim: accept a round trip sample in 1 of this many rounds (0 = all)\nstatic unsigned g_ap_sparse_n;\n\nstruct ap_filter {")
sub("ctl->cap_bps / AP_START_CAP_DIV), AP_START_MAX_BPS)", "ctl->cap_bps / AP_START_CAP_DIV), g_ap_start_max)")
sub("    uint64_t rtt_ns = now - sent_ns;\n", "    if (g_ap_sparse != 0U && (g_ap_sparse_n++ % g_ap_sparse) != 0U) {\n        return;\n    }\n    uint64_t rtt_ns = now - sent_ns;\n")
open(sys.argv[2], "w").write(s)
PY
gcc -O2 -Wall -Wextra -Wno-unused-function -I"$OUT/inc" -o "$OUT/algosim" "$HERE/algosim.c" -lm

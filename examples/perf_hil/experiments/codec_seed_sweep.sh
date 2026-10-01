#!/usr/bin/env bash
# codec_seed_sweep.sh - is direct_codec_identity's check 3/4 disagreement the count-field frame-shift class?
#
# THE CLAIM UNDER TEST (Dev, 2026-10-01, from one instrumented instance). DcNested sample 38 failed because a single
# bit flip at byte 0 turned the sequence count of `string[] names` from 0x04 to 0x00. That is not local corruption:
# every byte after it is reparsed as a different field, so the two decoders are no longer looking at the same fields
# and trip different validity checks. Both stayed within IDL bounds and inside the struct, so nothing unsafe
# happened - the test's contract ("both decoders accept or refuse alike on arbitrary bit-flipped bytes") is simply
# stronger than two independent parsers can honour once a length field is damaged.
#
# WHAT WOULD FALSIFY IT, written before running. A bit flip lands on a uniformly random byte, so if the frame-shift
# explanation is right, a type's chance of failing should track HOW MUCH OF IT IS LENGTH FIELDS - the number of
# variable-length members (sequences and strings), each of which carries a count that a flip can corrupt.
#
#   supports it  failures concentrate on the variable-length-heavy types and are absent, or near absent, on the
#                types built only from fixed scalars (Simple, DcStamp, DcOuter, Branch, Primitives)
#   refutes it   failures appear at a similar rate on fixed-scalar-only types, where there is no count to damage
#                and a flip can only change a value. Then the disagreement is about something else and Dev's
#                proposed fix to the test's contract would be aimed at the wrong thing.
#
# The sweep cannot see WHICH byte was flipped - the tool does not print it, and instrumenting it to find out would
# be changing the thing under test. The per-type rate is what this run decides; the byte offset is Dev's single
# instrumented instance and stays that.
#
# Usage: codec_seed_sweep.sh [SEEDS] [PKG]      Output: $OUT (default ~/rig_results_safe/codec_seed_sweep.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SEEDS=${1:-200}
PKG=${2:-rosidl_typesupport_tickle_c_tests}
BIN=$REPO/build/rosidl_typesupport_tickle_c_tests/direct_codec_identity
OUT=${OUT:-$HOME/rig_results_safe/codec_seed_sweep.txt}
mkdir -p "$(dirname "$OUT")"; : >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

[ -x "$BIN" ] || { say "FATAL no $BIN - build rmw_tickle's tests first"; exit 1; }
set +u
# ROS 2's setup scripts live outside this repository, so shellcheck cannot follow them.
# shellcheck source=/dev/null
. /opt/ros/jazzy/setup.bash >/dev/null 2>&1
if [ -f "$REPO/install/setup.bash" ]; then
    # shellcheck source=/dev/null
    . "$REPO/install/setup.bash" >/dev/null 2>&1
fi
set -u
"$BIN" -s 1 "$PKG" >/dev/null 2>&1 || true
say "=== codec seed sweep $(date -Is) seeds=$SEEDS pkg=$PKG ==="

# The seed that found it, first, so a sweep that reproduces nothing else still shows the instrument works.
say "--- control: seed 3991436827, the one CI drew ---"
"$BIN" -s 3991436827 "$PKG" 2>&1 | grep '^FAIL' | sed 's/^/  /' | tee -a "$OUT"

say "--- sweep ---"
for i in $(seq 1 "$SEEDS"); do
    seed=$(( (i * 2654435761) % 4294967291 ))
    "$BIN" -s "$seed" "$PKG" 2>&1 | grep '^FAIL' | sed "s/^/seed=$seed /" >>"$OUT"
done
say "=== done $(date -Is) ==="

python3 - "$OUT" "$REPO/rmw_tickle/rosidl_typesupport_tickle_c_tests/msg" <<'PYEOF' | tee -a "$OUT"
import re, sys, os, collections
out, msgdir = sys.argv[1], sys.argv[2]
fails = collections.Counter(); checks = collections.Counter(); seeds = set()
for line in open(out):
    m = re.match(r"^seed=(\d+) FAIL \S+/msg/(\w+) check (\d+) sample (\d+)", line)
    if m:
        seeds.add(m.group(1)); fails[m.group(2)] += 1; checks[m.group(3)] += 1
# How much of each type is length fields, counted from its own .msg rather than from memory.
varlen = {}
for fn in sorted(os.listdir(msgdir)):
    if not fn.endswith(".msg"): continue
    n = 0
    for raw in open(os.path.join(msgdir, fn)):
        s = raw.split("#")[0].strip()
        if not s: continue
        t = s.split()[0]
        if t.startswith("string") or "[]" in t or "[<=" in t: n += 1
    varlen[fn[:-4]] = n
print()
print("=== per type: variable-length members (each carries a count a flip can damage) vs failures ===")
print(f"{'type':>22} {'var-len members':>16} {'failures':>9}")
for t in sorted(varlen, key=lambda x: (-varlen[x], x)):
    print(f"{t:>22} {varlen[t]:>16} {fails.get(t,0):>9}")
tot = sum(fails.values())
heavy = sum(v for t, v in fails.items() if varlen.get(t, 0) >= 2)
fixed = sum(v for t, v in fails.items() if varlen.get(t, 0) == 0)
print()
print(f"failures {tot} over {len(seeds)} seeds that produced any; checks: " +
      ", ".join(f"{k}:{v}" for k, v in sorted(checks.items())))
print(f"on types with >=2 variable-length members: {heavy}")
print(f"on types with NO variable-length member:   {fixed}")
print()
if tot == 0:
    print("VERDICT: the sweep found nothing. That neither supports nor refutes the explanation - it says the")
    print("         rate is below what this many seeds can see, and the next question is how many are needed.")
elif fixed == 0 and heavy > 0:
    print("VERDICT: supports the frame-shift explanation. Every failure is on a type that has a count to damage,")
    print("         and the fixed-scalar-only types - where a flip can change a value but not a frame - have none.")
elif fixed > 0:
    print("VERDICT: REFUTES it, or at least is not explained by it. Failures appear on types with no length field")
    print("         at all, where there is no frame to shift. The disagreement has another cause too.")
PYEOF

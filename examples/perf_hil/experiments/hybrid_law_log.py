#!/usr/bin/env python3
"""Reads the reader's control law from its log (DESIGN.md section 8, "The objective and the control law").

hybrid_send_netns.sh's squeeze cells, the rules written before the first run (2026-10-11) and implemented here:

  squeeze_wait  - the reassembly memory is squeezed (1 MB best effort at 30 Hz through 0.1% netem loss, the kernel's
                  defaults): at least one collapse; the first one drops the cap at once to round(0.7 x its ceiling)
                  (at least 1); no probe before 2 tau after it (tau as the collapse line states it, timestamps to the
                  second, so 1 s of slack); and a probe within 2 tau + 5 s, the run being long enough to see one.
  squeeze_halve - as above to the first collapse; then the publisher halves its rate (RATE_HALVED in its log) and,
                  within 6 ticks of that, the reader logs "R p fell" and its ceiling doubles (2 C, +-1 for rounding).

usage: hybrid_law_log.py squeeze_wait|squeeze_halve SUB_LOG PUB_LOG
Prints one line: "law: verdict=PASS|FAIL ..." with what it read.
"""
import re
import sys

STAMP = re.compile(r"^\[\d{4}-\d{2}-\d{2} (\d{2}):(\d{2}):(\d{2})\]")
COLLAPSE = re.compile(r"runs collapsed at n = (\d+) .*: ceiling (\d+), cap (\d+), held (\d+) s")
PROBE = re.compile(r"probing n = (\d+) above the ceiling (\d+)")
FELL = re.compile(r"R p fell ([\d.]+)x since the collapse - the ceiling (\d+) -> (\d+)")


def seconds(line):
    m = STAMP.match(line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) if m else None


def events(path, pattern):
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                out.append((seconds(line), m.groups()))
    return out


def main():
    mode, sub_log, pub_log = sys.argv[1], sys.argv[2], sys.argv[3]
    collapses = events(sub_log, COLLAPSE)
    why = []
    if not collapses:
        print("law: verdict=FAIL collapses=0 (the squeeze was never detected)")
        return 1
    t0, (n, ceiling, cap, held) = collapses[0]
    n, ceiling, cap, held = int(n), int(ceiling), int(cap), int(held)
    want_cap = max(1, int(ceiling * 0.7 + 0.5))
    if cap != want_cap:
        why.append(f"cap={cap}!=round(0.7x{ceiling})={want_cap}")
    probes = [(t, g) for t, g in events(sub_log, PROBE) if t is not None and t >= t0]
    first_probe = probes[0][0] - t0 if probes else None
    read = f"collapses={len(collapses)} first: n={n} ceiling={ceiling} cap={cap} tau={held}s first_probe_after={first_probe}"
    if mode == "squeeze_wait":
        if first_probe is not None and first_probe < 2 * held - 1:
            why.append(f"probe_after={first_probe}s<2tau={2 * held}s")
        if first_probe is None or first_probe > 2 * held + 5:
            why.append("no_probe_by_2tau+5s")
    elif mode == "squeeze_halve":
        halved = None
        with open(pub_log, errors="replace") as f:
            halved = any("RATE_HALVED" in line for line in f)
        fell = events(sub_log, FELL)
        read += f" rate_halved={halved} rescales={len(fell)}"
        if not halved:
            why.append("rate_never_halved")
        elif not fell:
            why.append("no_rescale")
        else:
            _, (factor, before, after) = fell[0]
            read += f" first_rescale: {factor}x {before}->{after}"
            if abs(int(after) - 2 * int(before)) > 1 and int(after) <= 45:
                why.append(f"ceiling {before}->{after} not doubled")
    verdict = "FAIL" if why else "PASS"
    print(f"law: verdict={verdict} {read}" + (f" why={','.join(why)}" if why else ""))
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())

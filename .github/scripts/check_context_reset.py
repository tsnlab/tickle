#!/usr/bin/env python3
"""Every field of struct tt_Context must be initialised by reset_node_state(), or be listed here with a reason.

WHY THIS EXISTS. tt_Context is caller-owned: a caller is not required to zero it, so a field the reset function does
not assign begins life holding whatever was on the caller's stack. This has now shipped twice.

  2026-09-29, morning: the per-transport counters were added to the struct and not to the function. A 949k-datagram
                       run reported tx_udp=140723338891185. The counters only lied.
  2026-09-29, night:   segment_peers[] and own_segment were added to the struct and not to the function, and the new
                       teardown walked all 256 entries calling munmap() on each non-NULL one. It unmapped parts of its
                       own process - mapping=0x3, mapping=0x40 - and every node in make test-linux segfaulted at exit.

SHM_PLAN.md already recorded the lesson after the first one, in as many words: "a field added to this struct and not
to this function is never zero". It was written about the counters, and the next two fields added were still missed.
A lesson in a document did not prevent the recurrence, so this is the version the build asks instead of the version
a person has to remember. The behavioural counterpart is tests/test_*.c's 0xAA reset test, which catches it at
runtime; this catches it before anything runs, and fails closed on a field nobody has classified.

A field that legitimately survives a reset goes in SURVIVES_RESET below WITH A REASON. That is the point: adding a
field then becomes a decision someone has to write down, rather than an omission nobody sees.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]

# Fields that deliberately outlive reset_node_state(), each with the reason it does.
SURVIVES_RESET = {}

# Fields that predate this check and have NOT yet been classified. They are NOT endorsed: each one is either a
# deliberate survivor that belongs in SURVIVES_RESET with its reason, or a live instance of the bug above. The list
# exists so the check can be switched on for NEW fields today rather than waiting for all of them to be reviewed.
# Shrinking it to empty is the job; nothing may be added to it.
UNREVIEWED = {
    "collision_last_ns", "collision_logged_ip", "collision_logged_port", "collision_since_ns", "created_ns",
    "default_node_name", "endpoint_index", "hal", "id_explicit", "id_muted", "id_muted_drops", "ids_seen",
    "local_scratch", "poller_active", "poller_thread", "rx_clock_ns", "rx_malformed_drops", "rx_out_of_range",
    "rx_targeted", "sched_inbox", "sched_inbox_pending", "sched_inbox_state", "state_depth", "state_lock",
    "state_lock_stats", "state_owner", "version_mismatch_drops", "version_mismatch_logged", "wait_seq",
    "wait_until_hi", "wait_until_lo",
}


def block(lines, start):
    """The brace-balanced block beginning at `start`."""
    depth, out = 0, []
    for line in lines[start:]:
        depth += line.count("{") - line.count("}")
        out.append(line)
        if depth == 0 and len(out) > 1:
            return out
    return out


def context_fields(header):
    lines = header.split("\n")
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("struct tt_Context {"))
    except StopIteration:
        sys.exit("check_context_reset: struct tt_Context not found - has it been renamed?")
    body = block(lines, start)[1:-1]
    fields, depth = [], 0
    for line in body:
        code = re.sub(r"//.*$", "", line)
        if depth == 0:
            # An ordinary declaration: <type> name; / name[N];
            for m in re.finditer(r"(?:^|;)\s*(?:[A-Za-z_][\w ]*?[\s*]+)\*?([A-Za-z_]\w*)\s*(?:\[[^;]*\])?\s*;", code):
                fields.append(m.group(1))
        # A nested/anonymous struct or union closes as `} name[N];` - the form that hid segment_peers[] from the
        # first version of this script, and segment_peers[] is one of the two fields that caused the munmap crash.
        for m in re.finditer(r"\}\s*\*?([A-Za-z_]\w*)\s*(?:\[[^;]*\])?\s*;", code):
            fields.append(m.group(1))
        depth += code.count("{") - code.count("}")
    return sorted(set(fields))


def reset_touches(source):
    lines = source.split("\n")
    try:
        start = next(i for i, l in enumerate(lines) if "static void reset_node_state(" in l)
    except StopIteration:
        sys.exit("check_context_reset: reset_node_state() not found - has it been renamed?")
    body = "\n".join(block(lines, start))
    touched = set(re.findall(r"node->(\w+)", body))
    # One level of helper: a field reset inside a function reset_node_state() hands the context to is still reset.
    for helper in set(re.findall(r"\b(\w+)\s*\(\s*node\s*[,)]", body)):
        for i, l in enumerate(lines):
            if re.match(r"^(static\s+)?[\w ]*[\w*]\s+%s\s*\(" % re.escape(helper), l):
                touched |= set(re.findall(r"node->(\w+)", "\n".join(block(lines, i))))
    return touched


def main():
    fields = context_fields((ROOT / "include/tickle/tickle.h").read_text())
    touched = reset_touches((ROOT / "src/tickle.c").read_text())
    known = set(SURVIVES_RESET) | UNREVIEWED
    missing = [f for f in fields if f not in touched and f not in known]
    stale = sorted((set(SURVIVES_RESET) | UNREVIEWED) & touched)

    print("check-context-reset: %d fields in struct tt_Context, %d assigned by reset_node_state()"
          % (len(fields), len([f for f in fields if f in touched])))
    if stale:
        print("  note: now reset, so they can leave the lists: " + ", ".join(stale))
    if not missing:
        print("  OK - every field is reset or classified")
        return 0
    print("\ncheck-context-reset: FAIL - %d field(s) of struct tt_Context are never assigned by reset_node_state():"
          % len(missing))
    for f in missing:
        print("    %s" % f)
    print("""
tt_Context is caller-owned and a caller need not zero it, so each of these begins life holding whatever was on the
caller's stack. That has shipped twice already: once as tx_udp=140723338891185, and once as munmap() called on
0x3 and 0x40, which segfaulted every node in make test-linux at exit.

Either assign it in reset_node_state(), or add it to SURVIVES_RESET in this script with the reason it survives.""")
    return 1


if __name__ == "__main__":
    sys.exit(main())

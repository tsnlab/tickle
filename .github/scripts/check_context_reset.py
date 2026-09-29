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

# Fields not assigned by reset_node_state(), each with WHAT SETS IT INSTEAD - not "why it is exempt". Dev's
# distinction, and it is the right one: "exempt" invites the next person to add a line, "set by X" invites them to
# check that X still runs.
SURVIVES_RESET = {
    "id_explicit": "claim_initial_id(), under tt_CONTEXT_ID_CLAIM; every read is under the same guard",
    "id_muted": "claim_initial_id(), under tt_CONTEXT_ID_CLAIM; every read is under the same guard",
    "id_muted_drops": "claim_initial_id(), under tt_CONTEXT_ID_CLAIM; every read is under the same guard",
    "created_ns": "claim_initial_id(), under tt_CONTEXT_ID_CLAIM; every read is under the same guard",
    "collision_since_ns": "claim_initial_id(), under tt_CONTEXT_ID_CLAIM; every read is under the same guard",
    "collision_last_ns": "claim_initial_id(), under tt_CONTEXT_ID_CLAIM; every read is under the same guard",
    "ids_seen": "claim_initial_id(), under tt_CONTEXT_ID_CLAIM; every read is under the same guard",
}

# Fields that predate this check and have NOT yet been classified. They are NOT endorsed: each one is either a
# deliberate survivor that belongs in SURVIVES_RESET with its reason, or a live instance of the bug above. The list
# exists so the check can be switched on for NEW fields today rather than waiting for all of them to be reviewed.
# Shrinking it to empty is the job; nothing may be added to it.
# Confirmed by Dev 2026-09-29 as live instances of the bug, awaiting the fix in reset_node_state(): each is a
# uint64_t counter that is only ever incremented, so a caller-owned context reports whatever was on the stack. Two of
# them are what a reader consults when a node delivered nothing, and "how many did we throw away and why" answered
# from garbage is worse than not answering. They are NOT allowlisted; the check reports them until they are reset.
UNREVIEWED = {
    "collision_logged_ip", "collision_logged_port",     "default_node_name", "endpoint_index", "hal",     "local_scratch", "poller_active", "poller_thread", "rx_clock_ns",     "rx_targeted", "sched_inbox", "sched_inbox_pending", "sched_inbox_state", "state_depth", "state_lock",
    "state_lock_stats", "state_owner", "version_mismatch_logged", "wait_seq",
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


GUARD_RE = re.compile(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)")


def guard_map(lines):
    """For each line index, the stack of preprocessor conditions it sits under."""
    stack, out = [], []
    for line in lines:
        st = line.strip()
        m = GUARD_RE.match(st)
        if m:
            kind = m.group(1)
            if kind in ("if", "ifdef", "ifndef"):
                out.append(tuple(stack))
                stack.append(st)
                continue
            if kind in ("elif", "else"):
                if stack:
                    stack[-1] = st
                out.append(tuple(stack[:-1]) if stack else ())
                continue
            if kind == "endif":
                out.append(tuple(stack[:-1]) if stack else ())
                if stack:
                    stack.pop()
                continue
        out.append(tuple(stack))
    return out


def guard_mismatches(source, fields):
    """Fields assigned ONLY under some #if, but read when that #if is off.

    Dev asked for "fail on a field assigned in a flag-gated branch only". That exact rule fires on seven fields that
    are perfectly correct - the tt_CONTEXT_ID_CLAIM family is written by claim_initial_id() under the flag and every
    one of its reads is under the same flag, so with the flag off the fields are neither written nor read. The
    property that actually matters is narrower, and this is it: a field is a bug when it can be READ under weaker
    conditions than it is ever WRITTEN under. That is the shape that leaves a live read of memory nobody set.
    """
    lines = source.split("\n")
    guards = guard_map(lines)
    writes, reads = {}, {}
    for i, line in enumerate(lines):
        code = re.sub(r"//.*$", "", line)
        for m in re.finditer(r"(?:\w+)->(\w+)\s*(\+\+|--|[-+|&^]?=(?!=))", code):
            writes.setdefault(m.group(1), []).append(guards[i])
        for m in re.finditer(r"(?:\w+)->(\w+)", code):
            reads.setdefault(m.group(1), []).append(guards[i])
        for m in re.finditer(r"memset\s*\(\s*(?:&\s*)?\w+->(\w+)", code):
            writes.setdefault(m.group(1), []).append(guards[i])
    bad = []
    for field, wg in writes.items():
        # Only struct tt_Context's own fields: the file is full of other structs reached the same way
        # (tt_SegmentHeader's slots/read_index, tt_SegmentPeer's ip/port), and they are not what this checks.
        if field not in fields:
            continue
        if not all(g for g in wg):
            continue  # at least one unguarded write: fine
        needed = set(wg[0])
        for g in wg[1:]:
            needed &= set(g)
        if not needed:
            continue
        for g in reads.get(field, []):
            if not needed & set(g):
                bad.append((field, sorted(needed)))
                break
    return bad


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
    mism = guard_mismatches((ROOT / "src/tickle.c").read_text(), set(fields))
    if mism:
        print("\ncheck-context-reset: FAIL - field(s) written only under a condition, but read when it is off:")
        for field, needed in mism:
            print("    %s  (written only under %s)" % (field, ", ".join(needed)))
        print("\nA read of a field nobody wrote in that build configuration. This is the shape that cost 2026-09-29:")
        print("a field whose initialiser is compiled out while its reader is not.")
        return 1
    if not missing:
        print("  OK - every field is reset or classified, and none is read under weaker conditions than it is written")
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

#!/usr/bin/env python3
"""TSN links and send windows (docs/DESIGN.md section 2 "Send windows"): every check tt_Context_create_with() makes on
a link's window is decided by a test, and every value a TSN link or a window fills is read where it changes the
context.

  - check mutants turn one refusal into a log line that lets creation go on (its `return reject_option(...)` becomes
    `(void)reject_option(...)`), skip a check's call, or drop one comparison of the links' agreement;
  - reader mutants drop a fill (the DATA class's priority, the lead, SO_TXTIME on) or the window's copy into the
    context, or let a fill override an explicit value.

Each rebuilds tests/test_link_tsn.c against a mutated COPY of src/tickle.c (the tree is never edited) and expects it to
FAIL. A mutant that survives is a check, or a reader, no test can tell from its absence. The HAL's half - each
datagram's send time under the window - is tests/mutants_socket_marks.py's. Every anchor must occur exactly once
(mutant_guard.check_anchors(), before anything runs), the test must pass on the unmutated source first (the control),
and every child runs bounded (mutant_guard.run_capped(): 2 GB, 5 minutes, its output capped).

Usage: tests/mutants_link_tsn.py     Prints one line per mutant and exits non-zero if any survives.
"""
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutant_guard  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
CC = os.environ.get("CC", "gcc")
T = "test_link_tsn"


def check(field_and_why):
    return (f"check {field_and_why}", f"return reject_option({field_and_why}", f"(void)reject_option({field_and_why}")


MUTANTS = [
    check('"net.links[].window_cycle_ns", "is 0, and'),
    check('"net.links[].window_offset_ns", "is not below'),
    check('"net.links[].window_length_ns", "is 0 with'),
    check('"net.links[].window_length_ns", "with window_offset_ns'),
    check('"net.links[].window_cycle_ns",\n                                 "with its offset'),
    ("check: a link's window not checked", "    tt_ret_t window = validate_link_window(link);",
     "    tt_ret_t window = tt_RET_OK;"),
    ("check: the links' windows not compared", "    tt_ret_t windows = validate_link_windows(net);",
     "    tt_ret_t windows = tt_RET_OK;"),
    ("agree: the cycle not compared", "        } else if (link->window_cycle_ns != first->window_cycle_ns ||",
     "        } else if (false ||"),
    ("agree: the offset not compared", "                   link->window_offset_ns != first->window_offset_ns ||",
     "                   false ||"),
    ("agree: the length not compared", "                   link->window_length_ns != first->window_length_ns) {",
     "                   false) {"),
    ("agree: no first window", "        if (first == NULL) {\n            first = link;",
     "        if (first == NULL) {\n            (void)0;"),
    ("resolve: not called", "    resolve_link_tx(opts);\n", ""),
    ("resolve: no TSN link seen", "        tsn = tsn || link->profile == tt_LINK_PROFILE_TSN;", "        (void)tsn;"),
    ("resolve: no DATA priority", "        data->priority = tt_LINK_TSN_DATA_PRIORITY;", "        (void)data;"),
    ("resolve: the DATA priority over an explicit one", "    if (tsn && data->priority == 0) {", "    if (tsn) {"),
    ("resolve: no default lead", "                link->window_lead_ns = tt_LINK_WINDOW_LEAD_NS;", "                (void)0;"),
    ("resolve: no window seen", "            window = true;\n", ""),
    ("resolve: SO_TXTIME not turned on", "        opts->socket.txtime = tt_SWITCH_ON;\n    }\n}",
     "        (void)0;\n    }\n}"),
    ("resolve: SO_TXTIME's resolved OFF taken as set",
     "    if (window && !context_value_set(opts->socket.txtime, tt_SWITCH_OFF)) {",
     "    if (window && opts->socket.txtime == tt_SWITCH_DEFAULT) {"),
    ("apply: not called", "    link_tx_window_apply(node);\n", ""),
    ("apply: a link without a window copied too",
     "        if (link->window_cycle_ns == 0) {\n            continue;\n        }\n"
     "        node->tx_window.cycle_ns = link->window_cycle_ns;",
     "        node->tx_window.cycle_ns = link->window_cycle_ns;"),
    ("apply: no cycle", "        node->tx_window.cycle_ns = link->window_cycle_ns;", "        (void)0;"),
    ("apply: no offset", "        node->tx_window.offset_ns = link->window_offset_ns;", "        (void)0;"),
    ("apply: no length", "        node->tx_window.length_ns = link->window_length_ns;", "        (void)0;"),
    ("apply: the last lead, not the longest", "        if (link->window_lead_ns > node->tx_window.lead_ns) {",
     "        if (true) {"),
    ("reset: the window kept", "    node->tx_window = (struct tt_TxWindow) {0};", "    (void)0;"),
]


def run(tickle_c, tmp):
    """Builds tests/test_link_tsn.c against tickle_c and runs it: 0 passed, 1 failed, 2 did not build, 3 crashed or
    timed out."""
    binary = os.path.join(tmp, T)
    built = mutant_guard.run_capped(
        [CC, "-O0", "-Wall", "-Wextra", f"-I{ROOT}/include", f"-I{ROOT}/src", f"-I{ROOT}/tests",
         f'-DTICKLE_C="{tickle_c}"', "-o", binary, f"{ROOT}/tests/{T}.c", f"{ROOT}/src/encoding.c",
         f"{ROOT}/src/log.c", "-lpthread", "-lm"], timeout=300)
    if built.returncode != 0:
        print(built.output[-2000:])
        return 2
    ran = mutant_guard.run_capped([binary], timeout=300, cwd=str(ROOT / "platform" / "linux"))
    if ran.returncode in (0, 1):
        return ran.returncode
    return 3


def main():
    mutant_guard.be_nice()
    mutant_guard.check_anchors(ROOT, [(name, "src/tickle.c", old, new) for name, old, new in MUTANTS])
    pristine = (ROOT / "src" / "tickle.c").read_text()
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        status = run(str(ROOT / "src" / "tickle.c"), tmp)
        print(f"{'PASS' if status == 0 else 'FAIL'}  control   {T} "
              f"{'passes' if status == 0 else 'fails'} on the unmutated source")
        if status != 0:
            return 1
        mutant_path = os.path.join(tmp, "mutant_tickle.c")
        killed = 0
        for name, old, new in MUTANTS:
            pathlib.Path(mutant_path).write_text(pristine.replace(old, new))
            status = run(mutant_path, tmp)
            if status == 1:
                killed += 1
                print(f"PASS  killed    {name}")
            else:
                failed = True
                print(f"FAIL  survived  {name} (exit {status})")
        print(f"{killed} of {len(MUTANTS)} mutants killed; {mutant_guard.usage_line()}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

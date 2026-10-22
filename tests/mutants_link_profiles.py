#!/usr/bin/env python3
"""Link profiles (docs/DESIGN.md "Link profiles"): every check tt_Context_create_with() makes on a link's profile fields
is decided by a test, and every field is read where it changes what the context does.

  - check mutants turn one refusal into a log line that lets creation go on (its `return reject_option(...)` becomes
    `(void)reject_option(...)`), skip the conflict check, or drop the Publisher's batching check;
  - reader mutants put the default back where a profile's value, or a link's, is applied - so the field is written and
    never read there.

Each rebuilds the test that covers it against a mutated COPY of src/tickle.c (the tree is never edited) - tests/
test_link_profiles.c, for the fragments a smaller MTU cuts tests/test_data_frag.c, and for the fragment-count bound
tests/test_link_mtu_fragments.c - and expects it to FAIL. A mutant that survives is a check, or a reader, no test can
tell from its absence. Every anchor must occur exactly once (mutant_guard.check_anchors(), before anything runs), each
test must pass on the unmutated source first (the controls), and every child runs bounded (mutant_guard.run_capped():
2 GB, 5 minutes, its output capped).

Usage: tests/mutants_link_profiles.py     Prints one line per mutant and exits non-zero if any survives.
"""
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutant_guard  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
CC = os.environ.get("CC", "gcc")
P = "test_link_profiles"
F = "test_data_frag"
M = "test_link_mtu_fragments"


def check(name, field_and_why):
    return (P, f"check {name}", f"return reject_option({field_and_why}", f"(void)reject_option({field_and_why}")


MUTANTS = [
    check("profile", '"net.links[].profile"'),
    check("batch", '"net.links[].batch"'),
    check("large_fragmentation", '"net.links[].large_fragmentation", "is not'),
    check("mtu minimum", '"net.links[].mtu", "is below'),
    (M, "check mtu fragment count", 'return reject_option("net.links[].mtu", "cuts',
     '(void)reject_option("net.links[].mtu", "cuts'),
    check("link_rate", '"net.links[].link_rate"'),
    check("retry_floor_ns", '"net.links[].retry_floor_ns"'),
    check("piggyback_divisor", '"net.links[].piggyback_divisor"'),
    check("conflict batch_tick_ns", '"net.links[].batch_tick_ns"'),
    check("conflict idle_heartbeat_ns", '"net.links[].idle_heartbeat_ns",'),
    check("conflict large_fragmentation", '"net.links[].large_fragmentation",\n                                 "differs'),
    (P, "check conflicts called", "tt_ret_t conflict = validate_link_conflicts(opts);", "tt_ret_t conflict = tt_RET_OK;"),
    (P, "check Publisher batching", "publisher_large_options_valid(opts) || opts->batching > tt_SWITCH_OFF",
     "publisher_large_options_valid(opts)"),
    (P, "profile T1S", "bool t1s = link->profile == tt_LINK_PROFILE_T1S;", "bool t1s = false;"),
    (P, "profile TSN takes the interface MTU", "bool own_mtu = t1s || link->profile == tt_LINK_PROFILE_TSN;",
     "bool own_mtu = t1s;"),
    (P, "T1S unicast_threshold", "link->unicast_threshold = tt_UNICAST_ALWAYS_BROADCAST; // a frame", "(void)0; // a frame"),
    (P, "T1S batch", "link->batch = t1s ? (uint8_t)tt_SWITCH_ON : (uint8_t)tt_SWITCH_OFF;",
     "link->batch = (uint8_t)tt_SWITCH_OFF;"),
    (P, "T1S large_fragmentation", "        link->large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;", "        (void)0;"),
    (P, "T1S piggyback_divisor", "link->piggyback_divisor = LINK_T1S_PIGGYBACK_DIVISOR;", "(void)0;"),
    (P, "T1S link_rate", "link->link_rate = t1s ? LINK_T1S_RATE : LINK_RATE_DEFAULT;",
     "link->link_rate = LINK_RATE_DEFAULT;"),
    (P, "T1S idle_heartbeat_ns", "link->idle_heartbeat_ns = LINK_T1S_IDLE_HEARTBEAT;", "(void)0;"),
    (P, "reader batch_tick_ns", "opts->discovery.tx_interval_ns = tick;", "(void)tick;"),
    (P, "reader idle_heartbeat_ns", "opts->reliable.idle_heartbeat_period = idle;", "(void)idle;"),
    (P, "reader large_fragmentation",
     "net->large_fragmentation = mode != tt_LARGE_FRAGMENTATION_INHERIT ? mode : net->large_fragmentation;",
     "(void)mode;"),
    (P, "a resolved default is not set", "return value != 0 && value != default_value;", "return value != 0;"),
    (P, "reader interface MTU", "link->mtu = link_interface_mtu(resolved);", "link->mtu = (uint16_t)LINK_ETHERNET_MTU;"),
    (P, "T1S retry floor", "link->retry_floor_ns = link_t1s_retry_floor(link->mtu, link->link_rate);",
     "link->retry_floor_ns = 0;"),
    (P, "reader link_rate", "uint64_t floor = (bits * tt_SECOND) / link_rate;",
     "uint64_t floor = (bits * tt_SECOND) / LINK_T1S_RATE;"),
    (P, "reader retry_floor_ns", "floor = link->retry_floor_ns > floor ? link->retry_floor_ns : floor;",
     "(void)floor;"),
    (P, "floor under the measured G", "node->timer_resolution_ns = node->retry_floor_ns;", "(void)0;"),
    (P, "floor under the cold G", "node->opts.reliable.timer_lateness_initial = floor;", "(void)0;"),
    (P, "reader piggyback_divisor", "divisor = link->piggyback_divisor > divisor ? link->piggyback_divisor : divisor;",
     "(void)divisor;"),
    (P, "reader batch", "batch = batch && link->batch == tt_SWITCH_ON;", "(void)batch;"),
    (P, "reader mtu", "control = link_control < control ? link_control : control;", "(void)link_control;"),
    (P, "piggyback period", "node->piggyback_every = divisor != 0 ? (uint32_t)tt_RELIABLE_BITMAP_BITS / divisor : 0U;",
     "node->piggyback_every = 0U;"),
    (P, "narrow link MTU units", "        node->large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;", "        (void)0;"),
    (P, "hand-built control_max", "node->control_max = (uint32_t)tt_CONTROL_MAX_LENGTH;", "(void)0;"),
    (P, "applied at creation", "    link_profiles_apply(node);", "    (void)0;"),
    (P, "Publisher batch default", "pub->batch = node->batch_default;", "pub->batch = false;"),
    (P, "Publisher piggyback default", "pub->heartbeat_piggyback_every = node->piggyback_every;",
     "pub->heartbeat_piggyback_every = 0;"),
    (P, "Publisher batching switch", "pub->batch = opts->batch || opts->batching == tt_SWITCH_ON;",
     "pub->batch = opts->batch;"),
    (P, "Publisher piggyback none",
     "opts->heartbeat_piggyback_every == tt_HEARTBEAT_PIGGYBACK_NONE ? 0U : opts->heartbeat_piggyback_every;",
     "opts->heartbeat_piggyback_every;"),
    (P, "batch limit", "uint32_t limit = node->control_max;", "uint32_t limit = tt_CONTROL_MAX_LENGTH;"),
    (P, "announce limit", "ROUNDUP(bytes) <= node->control_max;", "ROUNDUP(bytes) <= tt_CONTROL_MAX_LENGTH;"),
    (F, "whole limit", "    return node->control_max;\n#else", "    return (uint32_t)FRAG_WHOLE_DATA_LIMIT;\n#else"),
    (F, "fragment cut", "    return node->frag_cont;", "    return (uint32_t)FRAG_CONT_PAYLOAD;"),
    (F, "no runs on a narrow link", "    if (node->control_max < (uint32_t)tt_CONTROL_MAX_LENGTH) {", "    if (false) {"),
]


def run(test, tickle_c, tmp):
    """Builds tests/TEST.c against tickle_c and runs it: 0 passed, 1 failed, 2 did not build, 3 crashed or timed out."""
    binary = os.path.join(tmp, test)
    built = mutant_guard.run_capped(
        [CC, "-O0", "-Wall", "-Wextra", f"-I{ROOT}/include", f"-I{ROOT}/src", f"-I{ROOT}/tests",
         f'-DTICKLE_C="{tickle_c}"', "-o", binary, f"{ROOT}/tests/{test}.c", f"{ROOT}/src/encoding.c",
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
    mutant_guard.check_anchors(ROOT, [(name, "src/tickle.c", old, new) for _, name, old, new in MUTANTS])
    pristine = (ROOT / "src" / "tickle.c").read_text()
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        for test in (P, F, M):
            status = run(test, str(ROOT / "src" / "tickle.c"), tmp)
            print(f"{'PASS' if status == 0 else 'FAIL'}  control   {test} "
                  f"{'passes' if status == 0 else 'fails'} on the unmutated source")
            failed = failed or status != 0
        if failed:
            return 1
        mutant_path = os.path.join(tmp, "mutant_tickle.c")
        killed = 0
        for test, name, old, new in MUTANTS:
            pathlib.Path(mutant_path).write_text(pristine.replace(old, new))
            status = run(test, mutant_path, tmp)
            if status == 1:
                killed += 1
                print(f"PASS  killed    {name} ({test})")
            else:
                failed = True
                print(f"FAIL  survived  {name} ({test} exit {status})")
        print(f"{killed} of {len(MUTANTS)} mutants killed; {mutant_guard.usage_line()}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

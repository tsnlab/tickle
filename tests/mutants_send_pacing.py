#!/usr/bin/env python3
"""Send pacing (docs/DESIGN.md "Send pacing"): every check tt_Context_create_with() makes on a link's pacing_rate and
pacing_burst is decided by a test, and every field and every place the bucket is filled or read changes what is sent.

  - check mutants turn one refusal into a log line that lets creation go on (its `return reject_option(...)` becomes
    `(void)reject_option(...)`), or skip the pacing checks altogether;
  - reader mutants put back what an unpaced build does where a value is applied - the profile's rate, the link_rate it
    follows, the MTU the burst and the frame count follow, a datagram counted into the bucket, the cursor waiting in
    the scheduler, a retransmission held - so the value is written and never read there.

Each rebuilds tests/test_send_pacing.c against a mutated COPY of src/tickle.c (the tree is never edited) and expects it
to FAIL. A mutant that survives is a check, or a reader, no test can tell from its absence. Every anchor must occur
exactly once (mutant_guard.check_anchors(), before anything runs), the test must pass on the unmutated source first
(the control), and every child runs bounded (mutant_guard.run_capped(): 2 GB, 5 minutes, its output capped).

Not mutated here: the charge in seam_send_batch()'s two other variants (a HAL whose batch loops, and a build without
the segment), which this build does not compile.

Usage: tests/mutants_send_pacing.py     Prints one line per mutant and exits non-zero if any survives.
"""
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutant_guard  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
CC = os.environ.get("CC", "gcc")
T = "test_send_pacing"


def check(field_and_why):
    return (f"check {field_and_why}", f"return reject_option({field_and_why}", f"(void)reject_option({field_and_why}")


MUTANTS = [
    check('"net.links[].pacing_rate", "is below'),
    check('"net.links[].pacing_rate", "is above'),
    check('"net.links[].pacing_burst", "is set'),
    check('"net.links[].pacing_burst", "is below'),
    ("checks called", "    return validate_link_pacing(link);", "    return tt_RET_OK;"),
    ("T1S rate", "link->pacing_rate = t1s ? link_t1s_pacing_rate(link->link_rate) : tt_PACING_OFF;",
     "link->pacing_rate = tt_PACING_OFF;"),
    ("T1S rate follows link_rate", "uint64_t rate = (link_rate / 100U) * tt_LINK_T1S_PACING_PCT;",
     "uint64_t rate = (LINK_T1S_RATE / 100U) * tt_LINK_T1S_PACING_PCT;"),
    ("T1S rate floor", "return rate >= tt_LINK_RATE_MIN || link_rate < tt_LINK_RATE_MIN ? rate : (uint64_t)tt_LINK_RATE_MIN;",
     "return rate;"),
    ("burst follows the MTU", "link->pacing_burst = tt_PACING_BURST_FRAMES * ((uint32_t)link->mtu + LINK_FRAME_OVERHEAD);",
     "link->pacing_burst = tt_PACING_BURST_FRAMES * (1500U + LINK_FRAME_OVERHEAD);"),
    ("pacing off configures nothing", "    if (link->pacing_rate == tt_PACING_OFF) {\n        return;\n    }\n    uint64_t q16",
     "    if (false) {\n        return;\n    }\n    uint64_t q16"),
    ("burst read", "pacer->burst_ns = ((uint64_t)(link->pacing_burst >> 16U) * pacer->ns_per_byte_q16) +",
     "pacer->burst_ns = (((uint64_t)6152U * pacer->ns_per_byte_q16) >> 16U) + 0 *"),
    ("frame payload follows the MTU", "pacer->frame_payload = ((uint32_t)link->mtu - LINK_IP_HEADER) & ~7U;",
     "pacer->frame_payload = 1480U;"),
    ("context paced", "    node->pacing = pacing;", "    node->pacing = false;"),
    ("frames counted", "uint32_t frames = 1U + ((ip_payload - 1U) / pacer->frame_payload);", "uint32_t frames = 1U;"),
    ("link of a destination", "return link_count(node) <= 1 ? 0U : link_of_ip(node, ip);", "return 0U;"),
    ("a backlog continues", "uint64_t start = pacer->until_ns > now ? pacer->until_ns : now;", "uint64_t start = now;"),
    ("charge seam_send", "        pace_charge_one(node, 0, len);", "        (void)0;"),
    ("charge seam_send_to", "        pace_charge_one(node, ip, len);", "        (void)0;"),
    ("charge seam_send_iov", "        pace_charge_one(node, ip, hdr_len + body_len);", "        (void)0;"),
    ("charge seam_send_batch", "        pace_charge_batch(node, remaining, left);", "        (void)0;"),
    ("charge a large send", "            pace_charge_batch(node, batch, (uint32_t)sent);", "            (void)0;"),
    ("the bucket limits a call", "        return fit < want ? (uint32_t)fit : want;", "        return want;"),
    ("a unit past the burst goes alone", "    if (backlog == 0) {\n        return 1U;\n    }\n    *at",
     "    if (false) {\n        return 1U;\n    }\n    *at"),
    ("wait for one unit's room", "*at = now + (cost > pacer->burst_ns ? backlog : backlog + cost - pacer->burst_ns);",
     "*at = now + backlog;"),
    ("cursor waits for the bucket", "            if (cursor->pace_at_ns != 0) {", "            if (false) {"),
    ("repairs borrow one burst", "backlog + pace_cost_ns(pacer, len) <= 2U * pacer->burst_ns",
     "backlog + pace_cost_ns(pacer, len) <= pacer->burst_ns"),
    ("repairs held past two bursts", "    node->pacing_repairs_deferred++;\n    return false;",
     "    node->pacing_repairs_deferred++;\n    return true;"),
    ("large repair paced", "!pace_repair_allowed(node, target, framing_length - skip + length)",
     "!(true || pace_repair_allowed(node, target, framing_length - skip + length))"),
    ("cached repair paced", "!pace_repair_allowed(node, target, (uint32_t)sizeof(struct tt_Header) + cache_entry->len)",
     "!(true || pace_repair_allowed(node, target, (uint32_t)sizeof(struct tt_Header) + cache_entry->len))"),
]


def run(tickle_c, tmp):
    """Builds tests/test_send_pacing.c against tickle_c and runs it: 0 passed, 1 failed, 2 did not build, 3 crashed or
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
    with tempfile.TemporaryDirectory() as tmp:
        status = run(str(ROOT / "src" / "tickle.c"), tmp)
        print(f"{'PASS' if status == 0 else 'FAIL'}  control   {T} {'passes' if status == 0 else 'fails'} on the "
              "unmutated source")
        if status != 0:
            return 1
        mutant_path = os.path.join(tmp, "mutant_tickle.c")
        killed = 0
        failed = False
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

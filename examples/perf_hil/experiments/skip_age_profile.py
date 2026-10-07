#!/usr/bin/env python3
"""skip_age_profile.py <src/tickle.c> - adds TSC clock reads to the segment drain of a COPY of tickle.c (experiment
only; never applied to src/ in the repository). Prints one 'drain_profile:' line at context destroy. Every anchor must
match exactly once or the script fails, so a patch that silently instrumented nothing cannot produce a profile.

What it measures, per process (one subscriber context):
  drains        drain_own_segment() calls that found records
  plan_*        plan_segment_skips(): calls, TSC cycles, records it planned over (skip arms only)
  skiphead_*    segment_skip_head() calls that returned SKIPPED, and their cycles excluding any plan inside
  read_*        records read the ordinary way: segment_read() + process_datagram_locked(), cycles
  kl_*          (skip arms) reads that handed a KEEP_LAST Subscriber a sample: how many, cycles from the drain's start
                to that read's start (kl_pre: what the delivered sample waits for inside the drain before it is
                decoded), and that read's own cycles (kl_dec)
  tsc_hz        TSC rate measured against CLOCK_MONOTONIC over the process life, to convert cycles to ns
"""
import sys

path = sys.argv[1]
src = open(path).read()


def sub(old, new, count=1):
    global src
    n = src.count(old)
    if n != count:
        sys.exit(f"anchor matched {n} times, wanted {count}: {old[:80]!r}")
    src = src.replace(old, new)


skip_arm = "static enum segment_head segment_skip_head(" in src

PROLOGUE = r'''
#include <stdint.h>
#include <x86intrin.h>
#include <time.h>
static uint64_t prof_drains, prof_plan_calls, prof_plan_cyc, prof_plan_records, prof_skiphead_n, prof_skiphead_cyc;
static uint64_t prof_read_n, prof_read_cyc, prof_kl_n, prof_kl_pre_cyc, prof_kl_dec_cyc, prof_tsc0, prof_ns0;
static uint64_t prof_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
'''
src = PROLOGUE + src  # at the top: the plan and the drain both use it

sub("""    uint32_t delivered = 0;
    uint32_t drained = 0;
""", """    uint32_t delivered = 0;
    uint32_t drained = 0;
    uint64_t prof_d0 = __rdtsc();
    if (prof_tsc0 == 0) {
        prof_tsc0 = prof_d0;
        prof_ns0 = prof_mono_ns();
    }
    prof_drains++;
""")

if skip_arm:
    # 4c46f7cb takes one record per SKIPPED head; the leading-run commit says how many through `passed`.
    call = ("segment_skip_head(node, &passed)" if "segment_skip_head(node, &passed)" in src
            else "segment_skip_head(node)")
    records = "passed" if "&passed" in call else "1"
    sub(f"""            enum segment_head head = {call};
""", f"""            uint64_t prof_h0 = __rdtsc();
            uint64_t prof_plan_before = prof_plan_cyc;
            enum segment_head head = {call};
            if (head == SEGMENT_HEAD_SKIPPED) {{
                prof_skiphead_n += {records};
                prof_skiphead_cyc += (__rdtsc() - prof_h0) - (prof_plan_cyc - prof_plan_before);
            }}
""")
    sub("""static void plan_segment_skips(struct tt_Context* node, uint32_t read_index) {
""", """static void plan_segment_skips_body(struct tt_Context* node, uint32_t read_index);
static void plan_segment_skips(struct tt_Context* node, uint32_t read_index) {
    uint64_t prof_p0 = __rdtsc();
    plan_segment_skips_body(node, read_index);
    prof_plan_cyc += __rdtsc() - prof_p0;
    prof_plan_calls++;
    prof_plan_records += node->segment_plan_count;
}
static void plan_segment_skips_body(struct tt_Context* node, uint32_t read_index) {
""")

READ = """            if (!segment_read(node->own_segment, node->rx_buffer, (uint32_t)sizeof(node->rx_buffer), &len, &sender_ip,
                              &sender_port, &seq_span)) {"""
sub(READ, "            uint64_t prof_r0 = __rdtsc();\n" + READ)
PROC = """            (void)process_datagram_locked(node, (int32_t)len, sender_ip, sender_port, tt_TRANSPORT_SHM, seq_span);
"""
if skip_arm:
    sub(PROC, """            uint64_t prof_kl_before = node->rx_keep_last_delivered;
""" + PROC + """            uint64_t prof_r1 = __rdtsc();
            prof_read_n++;
            prof_read_cyc += prof_r1 - prof_r0;
            if (node->rx_keep_last_delivered != prof_kl_before) {
                prof_kl_n++;
                prof_kl_pre_cyc += prof_r0 - prof_d0;
                prof_kl_dec_cyc += prof_r1 - prof_r0;
            }
""")
else:
    sub(PROC, PROC + """            prof_read_n++;
            prof_read_cyc += __rdtsc() - prof_r0;
""")

sub("""    TT_LOG_INFO("Node %u traffic: """, """    {
        uint64_t prof_dt = __rdtsc() - prof_tsc0;
        uint64_t prof_dn = prof_mono_ns() - prof_ns0;
        double hz = prof_dn ? (double)prof_dt * 1e9 / (double)prof_dn : 0;
        TT_LOG_INFO("drain_profile: drains=%lu plan_calls=%lu plan_cyc=%lu plan_records=%lu skiphead_n=%lu "
                    "skiphead_cyc=%lu read_n=%lu read_cyc=%lu kl_n=%lu kl_pre_cyc=%lu kl_dec_cyc=%lu tsc_hz=%.0f",
                    (unsigned long)prof_drains, (unsigned long)prof_plan_calls, (unsigned long)prof_plan_cyc,
                    (unsigned long)prof_plan_records, (unsigned long)prof_skiphead_n, (unsigned long)prof_skiphead_cyc,
                    (unsigned long)prof_read_n, (unsigned long)prof_read_cyc, (unsigned long)prof_kl_n,
                    (unsigned long)prof_kl_pre_cyc, (unsigned long)prof_kl_dec_cyc, hz);
    }
    TT_LOG_INFO("Node %u traffic: """)
open(path, "w").write(src)
print(f"patched {path} skip_arm={skip_arm}")

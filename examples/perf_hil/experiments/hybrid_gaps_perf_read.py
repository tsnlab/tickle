#!/usr/bin/env python3
"""hybrid_gaps_perf_read.py - hybrid_gaps_perf.sh's profiles as cycles per delivered MB, by category.

Usage: hybrid_gaps_perf_read.py NSYMS:MB [NSYMS:MB ...]
  NSYMS  a .nsyms file from hybrid_gaps_perf_counts.sh; MB the megabytes the arm delivered (sub) or published (pub).

Categories (decided before reading, 2026-10-12): the harness's own work (bench_decode's pattern check, perf_test's
sync_reset spin, the ROS setup's Python and shells) is set apart and excluded from the per-MB total - rmw_tickle's
decode is one memcpy, not the bench's pattern check. The rest: kernel separate checksum pass (skb_checksum /
csum_partial reached from udp_poll's first_packet_length), kernel copy (fused copy+checksum included), kernel other,
user memcpy (large_place, memmove), core per-unit bookkeeping, user other.
"""
import re
import sys

UNIT = re.compile(r"deliver_large_fragment|process_run_submessage|route_large_fragment|drain_reorder|"
                  r"update_reliable_ack|maybe_arm_acknack|highest_relevant_bit|tt_get_ns|large_accept|"
                  r"large_assembly_find|hybrid_rx_note|find_writer_proxy|large_has_landed|large_sweep")
HARNESS_SYM = re.compile(r"bench_decode|sync_reset|^pattern|_Py|Py[A-Z]")
HARNESS_COMM = re.compile(r"^(python|bash|sh|ros2|perf$|timeout)")
CSUM_SEP = re.compile(r"^(skb_checksum|csum_partial|__skb_checksum_complete|do_csum)$")
COPY = re.compile(r"copy_to_iter|copy_to_user|skb_datagram_iter|csum_partial_copy|csum_and_copy|copy_user|"
                  r"check_heap_object|__check_object_size|copy_from_iter|copy_from_user")
MEMCPY = re.compile(r"memmove|memcpy|large_place")


def categorize(comm, dso, sym):
    # the bench's main() is its publisher filling the message before each publish (-M): harness, as perf_test's is
    if HARNESS_COMM.match(comm) or HARNESS_SYM.search(sym) or (comm == "bench" and sym == "main"):
        return "harness"
    kernel = dso.startswith("[kernel")
    if kernel:
        if CSUM_SEP.match(sym):
            return "k_csum_separate"
        if COPY.search(sym):
            return "k_copy"
        if sym.startswith("memset") or "init_pages" in sym or "clear_page" in sym:
            return "k_zeroing"
        return "k_other"
    if MEMCPY.search(sym):
        return "u_memcpy"
    if UNIT.search(sym):
        return "u_core_per_unit"
    return "u_other"


def read(path):
    total = None
    rows = []
    for line in open(path, encoding="utf-8"):
        if line.startswith("# Event count"):
            total = float(line.split(":")[1].split()[0])
            continue
        if line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        share = float(parts[0].rstrip("%")) / 100.0
        comm, dso = parts[2], parts[3]
        sym = " ".join(parts[5:]) if parts[4] in ("[.]", "[k]") else " ".join(parts[4:])
        rows.append((share, comm, dso, sym))
    return total, rows


def main():
    cats = ["k_csum_separate", "k_copy", "k_zeroing", "k_other", "u_memcpy", "u_core_per_unit", "u_other", "harness"]
    print(f"{'arm':38s} " + " ".join(f"{c:>15s}" for c in cats) + f" {'total-harness':>14s}   (Mcycles per MB)")
    for spec in sys.argv[1:]:
        path, mb = spec.rsplit(":", 1)
        total, rows = read(path)
        sums = dict.fromkeys(cats, 0.0)
        for share, comm, dso, sym in rows:
            sums[categorize(comm, dso, sym)] += share
        per_mb = {c: sums[c] * total / float(mb) / 1e6 for c in cats}
        name = path.rsplit("/", 1)[-1].replace(".nsyms", "")
        real = sum(v for c, v in per_mb.items() if c != "harness")
        print(f"{name:38s} " + " ".join(f"{per_mb[c]:15.2f}" for c in cats) + f" {real:14.2f}")


if __name__ == "__main__":
    main()

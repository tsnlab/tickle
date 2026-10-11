#!/usr/bin/env python3
"""Mutant sweep for receiving a large sample's fragments in place (DESIGN.md section 8, "Receiving in place").

Each entry breaks one condition of core's hint and check (src/tickle.c: large_direct_aim(), tt_rx_direct_expected(),
large_place(), large_assembly_free()) or the HAL's aimed read (src/hal_linux.c: direct_read() and its hand-out) and
names the check that must go red for it: tests/test_rx_direct.c for core, platform/linux/rx_direct_check.sh (end to
end between private namespaces, every byte checked) and platform/linux/rx_direct_hal_check (the HAL against crafted
reads) for the HAL. A mutant that still passes means the check does not test what it says, or the condition decides nothing and
should not be in the code.

Run from the repository root; needs passwordless `sudo ip` for the HAL entries. Restores every file it touches,
whatever happens. MUTANTS=<substring> runs only the mutants whose name contains it (the controls always run).
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LINUX = ROOT / "platform/linux"
CORE = "src/tickle.c"
HAL = "src/hal_linux.c"

MUTANTS = [
    ("core: the in-order run is not moved on", CORE,
     "        units = node->rx_direct_units - 1U;", "        units = node->rx_direct_units;",
     "unit", "test_the_run_stops_before_a_landed_fragment"),
    ("core: the run runs over a landed fragment", CORE,
     "           !large_has_landed(assembly, first + units)) {", "           true) {",
     "unit", "test_the_run_stops_before_a_landed_fragment"),
    ("core: the run runs past the sample", CORE,
     "    while (units < LARGE_DIRECT_UNITS && first + units < assembly->frag_count &&",
     "    while (units < LARGE_DIRECT_UNITS &&",
     "unit", "test_the_run_ends_with_the_sample"),
    ("core: no hint withdrawn when the run is empty", CORE,
     "    if (units == 0U) {\n        if (node->large.direct == assembly) {", "    if (units == 0U) {\n        if (false) {",
     "unit", "test_the_run_ends_with_the_sample"),
    ("core: a fragment in place copied over itself, uncounted", CORE,
     "    if (fragment->payload != place) {", "    if (true) {",
     "unit", "test_a_fragment_in_place_lands_without_a_copy"),
    ("core: expected at any unit", CORE,
     "    const uint32_t index = node->large.direct_first + unit;", "    const uint32_t index = rd16(&expanded, cont.frag_index);",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: entity not checked", CORE,
     "    return rd32(&expanded, cont.entity_id) == assembly->entity_id && ", "    return ",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: source not checked", CORE,
     " || single.source != assembly->source) {", ") {",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: seq_no not checked", CORE,
     "           rd32(&expanded, cont.seq_no) == assembly->seq_no + index && !large_has_landed(assembly, index) &&",
     "           !large_has_landed(assembly, index) &&",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: count not checked", CORE,
     "           rd16(&expanded, cont.frag_count) == assembly->frag_count &&\n", "",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: type not checked", CORE,
     "        single.type != tt_SUBMESSAGE_TYPE_FRAG_CONT_L || single.source", "        single.source",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: version not checked", CORE,
     " || single.version != tt_VERSION ||", " ||",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: the full form taken for the single", CORE,
     "    if ((single.marker != tt_SINGLE_MARKER_LE && single.marker != tt_SINGLE_MARKER_BE) ||\n", "    if (\n",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: a short fragment that is not the last expected", CORE,
     " : payload_len == LARGE_CONT_PAYLOAD);", " : payload_len <= LARGE_CONT_PAYLOAD);",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: an empty last fragment expected", CORE,
     "    (last ? payload_len > 0U && payload_len <= LARGE_CONT_PAYLOAD", "    (last ? payload_len <= LARGE_CONT_PAYLOAD",
     "unit", "test_only_the_expected_fragment_is_expected"),
    ("core: a landed fragment expected again", CORE,
     " && !large_has_landed(assembly, index) &&", " &&",
     "unit", "test_a_landed_fragment_is_not_expected"),
    ("core: a reclaimed entry taken for the hinted", CORE,
     "    if (assembly == NULL || assembly->buffer == NULL || assembly->claimed != node->large.direct_claimed ||",
     "    if (assembly == NULL || assembly->buffer == NULL ||",
     "unit", "test_a_reclaimed_entry_is_not_the_hinted_one"),
    ("core: freeing the hinted assembly leaves the hint", CORE,
     "    if (node->large.direct == assembly && large_direct_withdraw(node)) {", "    if (false) {",
     "unit", "test_freeing_the_hinted_assembly_withdraws_and_parks"),
    ("core: no cancel on a withdrawal", CORE,
     "    node->rx_direct_cancel = true;\n", "",
     "unit", "test_freeing_the_hinted_assembly_withdraws_and_parks"),
    ("core: a buffer under a read released", CORE,
     "    return __atomic_load_n(&node->rx_direct_busy, __ATOMIC_SEQ_CST) != 0;", "    return false;",
     "unit", "test_freeing_the_hinted_assembly_withdraws_and_parks"),
    ("core: a parked buffer never released", CORE,
     "    if (__atomic_load_n(&node->large.parked, __ATOMIC_SEQ_CST) != NULL) {", "    if (false) {",
     "unit", "test_freeing_the_hinted_assembly_withdraws_and_parks"),
    ("core: any free withdraws the hint", CORE,
     "    if (node->large.direct == assembly && large_direct_withdraw(node)) {",
     "    if (large_direct_withdraw(node)) {",
     "unit", "test_freeing_another_assembly_keeps_the_hint"),
    ("core: a split datagram's payload ignored", CORE,
     "    if (node->rx_split_payload != NULL && head == tail) {", "    if (false) {",
     "unit", "test_a_split_datagram_carries_its_payload"),
    # HAL
    ("hal: every read in place taken as expected", HAL,
     "            if (bytes < header || !tt_rx_direct_expected(node, split, hal->direct_headers[split], bytes - header)) {",
     "            if (bytes < header) {",
     "hal", "rx_direct_check.sh loss / reorder (torn)"),
    ("hal: a different segment size taken as the unit", HAL,
     "    if (size <= unit && (!merged || size == unit)) {", "    if (size <= unit) {",
     "hal", "rx_direct_hal_check sizes"),
    ("hal: the rest not moved up before the units", HAL,
     "        memmove(buf + (aimed - base), buf, tail);", "        (void)tail;",
     "hal", "rx_direct_check.sh merged (torn)"),
    ("hal: a gathered unit's payload not copied", HAL,
     "            memcpy(out + header, hal->direct_dest + ((size_t)k * payload), bytes);", "            (void)bytes;",
     "hal", "rx_direct_check.sh loss / reorder (torn)"),
    ("hal: a cancel ignored", HAL,
     "    if (node->rx_direct_cancel) {\n        hal->direct_next = hal->direct_split;", "    if (false) {\n        hal->direct_next = hal->direct_split;",
     "hal", "rx_direct_hal_check cancel"),
    ("hal: the split payload's place off by one fragment", HAL,
     "    node->rx_split_payload = hal->direct_dest + ((size_t)k * hal->direct_payload);",
     "    node->rx_split_payload = hal->direct_dest + ((size_t)(k + 1U) * hal->direct_payload);",
     "hal", "rx_direct_check.sh merged (direct_placed = direct_split)"),
    ("hal: framing handed out over the rest still to go", HAL,
     "        at = (hal->gro_end + 7U) & ~7U; // past what the read left there", "        at = 0; // past what the read left there",
     "hal", "rx_direct_check.sh merged (torn)"),
    ("hal: on without TT_RX_DIRECT=1", HAL,
     "    node->hal.direct_allowed = setting != NULL && strcmp(setting, \"1\") == 0;", "    node->hal.direct_allowed = true;",
     "hal", "rx_direct_check.sh control (TT_RX_DIRECT unset)"),
    ("hal: never aimed", HAL,
     "    if (!node->hal.direct_allowed || units == 0U ||", "    if (true ||",
     "hal", "rx_direct_check.sh merged (direct_split > 0)"),
    ("hal: a zero-length datagram divides by its size", HAL,
     "    if (got == 0U) {\n        return 0;", "    if (false) {\n        return 0;",
     "hal", "rx_direct_hal_check empty"),
]


def run(cmd, timeout):
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=LINUX)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1500:]
    if done.returncode < 0:
        return "CRASH", done.stdout[-1500:] + done.stderr[-1500:]
    return ("PASS" if done.returncode == 0 else "FAIL"), done.stdout + done.stderr


def build_and_run(kind):
    targets = ["obj/debug/tests/test_rx_direct"] if kind == "unit" else ["rx_direct_hal_check", "large_sample_bench"]
    build = subprocess.run(["make", "-s", *targets], capture_output=True, text=True, cwd=LINUX)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    if kind == "unit":
        return run(["./obj/debug/tests/test_rx_direct"], 60)
    return run(["./rx_direct_check.sh"], 180)


def failing_lines(output):
    return [line for line in output.splitlines() if "FAIL" in line or "test_rx_direct.c:" in line][:3]


def main():
    backup = pathlib.Path(tempfile.mkdtemp(prefix="rx_direct_mutants_"))
    for rel in (CORE, HAL):
        shutil.copy2(ROOT / rel, backup / pathlib.Path(rel).name)

    def restore():
        # copy(), then touch: copy2 would keep the original mtime, older than the mutant's objects, and make would then
        # reuse a binary built from mutated source (tests/mutants_shm_stage1.py, 2026-10-02).
        for rel in (CORE, HAL):
            shutil.copy(backup / pathlib.Path(rel).name, ROOT / rel)
            (ROOT / rel).touch()

    signal.signal(signal.SIGTERM, lambda _s, _f: sys.exit(1))
    failures = 0
    try:
        for kind in ("unit", "hal"):
            verdict, output = build_and_run(kind)
            print(f"[control]  unmutated, {kind:4s}                                        -> {verdict}", flush=True)
            if verdict != "PASS":
                print(output[-2000:])
                print("a control did not pass; the sweep cannot decide anything")
                return 1
        only = os.environ.get("MUTANTS", "")
        for name, rel, old, new, kind, guard in MUTANTS:
            if only and only not in name:
                continue
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name:55s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            verdict, output = build_and_run(kind)
            restore()
            ok = verdict in ("FAIL", "CRASH", "HANG")
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:55s} -> {verdict} (guard: {guard})", flush=True)
            for line in failing_lines(output) if ok else []:
                print(f"           {line}")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

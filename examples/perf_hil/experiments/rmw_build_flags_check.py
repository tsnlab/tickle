#!/usr/bin/env python3
"""rmw_build_flags_check.py - did the layout flags an arm asked for reach every unit of an rmw-layer colcon build?

Gate v2 A1 for the rmw harnesses (rmw_crosshost_rtt.sh, rmw_keepall_rig.sh; 2026-10-11). The rmw builds hand
TICKLE_EXTRA_CFLAGS to CMake inside an ssh line, as CMAKE_C_FLAGS / CMAKE_CXX_FLAGS, and a flag that stays in the
local shell builds the arm plain with nothing to show for it (it happened twice to the native campaign). So the
harness configures with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON, copies each build's compile_commands.json back, and this
reads what the compiler was actually given - not the variable.

Usage: rmw_build_flags_check.py <compile_commands.json> '<flags>' [--need <path suffix>]...
Prints one line saying what it saw and exits 0, or says what is wrong and exits 1. The rules, all enforced below:
  every    EVERY entry of the file (the package's own units, the TickLE core units it compiles in, its tests and
           generated code alike) carries every word of <flags> as a whole argument, and for a word -f<name>=<v> the
           LAST -f<name>= argument on the line is that word (a later -falign-functions=16 would win over ours);
  plain    with <flags> blank, no entry carries any -falign-functions= argument (the plain build is plain);
  need     every --need suffix (e.g. /src/tickle.c, the core unit rmw_tickle compiles into its .so) names at least one
           entry: a file that lists none of the units it should have compiled is "could not look", never a pass;
  present  the file exists, parses, and has at least one entry.
"""
import json
import shlex
import sys


def words_of(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry.get("command", ""))


def check(path, flags, need):
    want = flags.split()
    try:
        with open(path, encoding="utf-8") as fh:
            entries = json.load(fh)
    except (OSError, ValueError) as err:
        return False, f"cannot read {path}: {err}"
    if not isinstance(entries, list) or not entries:
        return False, f"{path} has no compile commands"
    for suffix in need:
        if not any(e.get("file", "").endswith(suffix) for e in entries):
            return False, f"{path} lists no unit ending {suffix}"
    for e in entries:
        args = words_of(e)
        unit = e.get("file", "?")
        if not want:
            bad = [a for a in args if a.startswith("-falign-functions")]
            if bad:
                return False, f"plain build, but {unit} was compiled with {bad[-1]}"
            continue
        for w in want:
            if w not in args:
                return False, f"{unit} was compiled without {w}"
            if w.startswith("-f") and "=" in w:
                key = w.split("=", 1)[0] + "="
                last = [a for a in args if a.startswith(key)][-1]
                if last != w:
                    return False, f"{unit}: {w} is overridden by a later {last}"
    shown = ", ".join(s.rsplit("/", 1)[-1] for s in need)
    what = f"carry '{flags}'" if want else "carry no -falign-functions (plain)"
    return True, f"all {len(entries)} units {what}" + (f", including {shown}" if need else "")


def main(argv):
    args = argv[1:]
    need = []
    while "--need" in args:
        i = args.index("--need")
        if i + 1 >= len(args):
            print(__doc__)
            return 2
        need.append(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    ok, line = check(args[0], args[1], need)
    print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

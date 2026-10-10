#!/usr/bin/env bash
# build_flags_check.sh - did the compiler flags an arm asked for reach the TickLE build it ran? (2026-10-11)
#
# An arm must report its treatment. TICKLE_EXTRA_CFLAGS is handed to tickle/build.sh across an ssh, and twice (2026-10-03,
# 2026-10-10) a flag stayed in the local shell and both arms of a "placement control" built plain. This reads the build's
# own record instead of the variable: build.sh run under `bash -x` traces every command it executes with its arguments
# expanded, so the log says what the compiler was actually given.
#
# Usage: build_flags_check.sh <build.log> '<flags>'
#   <build.log>  the whole output of `TICKLE_EXTRA_CFLAGS='<flags>' bash -x ./build.sh <scen> <size>`
#   <flags>      exactly the TICKLE_EXTRA_CFLAGS string that build was given (the prefix name is derived from it)
# Prints one line saying what it saw and exits 0, or says what is missing and exits 1. The rules, all enforced below:
#   bench    the last `+ <cc> ... -o <dir>/client` and `... -o <dir>/server` trace lines exist and each carries every
#            word of <flags> as a whole argument;
#   core     the install prefix the bench linked (`+ PKG_CONFIG_PATH=<prefix>/lib/pkgconfig`) is build.sh's prefix for
#            these flags: it ends in _x<slug> (slug = <flags> with every run of non-alphanumerics as one _) when <flags>
#            is not blank, and has no _x part when it is. build.sh keys the prefix on the flags and rebuilds it from
#            clean when its archive is older than the sources, so a reused prefix is the archive these flags made;
#   rebuilt  when the core was compiled in this build (a `+ make install` trace line), every core compile line
#            (`... -c -o <obj> <src>.c`) carries every word as well, and there is at least one.
# A log without the trace lines is "could not look" and fails: a build that was not traced has not shown its flags.
set -uo pipefail
log=${1:?usage: build_flags_check.sh <build.log> '<flags>'}
flags=${2-}
[ -r "$log" ] || { echo "build log $log is not readable"; exit 1; }
read -ra words <<<"$flags"

prefix=$(sed -n 's|^+ PKG_CONFIG_PATH=\(.*\)/lib/pkgconfig$|\1|p' "$log" | tail -1)
[ -n "$prefix" ] || { echo "no '+ PKG_CONFIG_PATH=' trace line in $log: the build was not traced, so it showed no flags"; exit 1; }
base=${prefix##*/}
if [ ${#words[@]} -gt 0 ]; then
    slug=$(printf '%s' "$flags" | tr -cs 'A-Za-z0-9' '_')
    case "$base" in
    *"_x$slug") ;;
    *) echo "core prefix $base is not build.sh's prefix for '$flags' (want a name ending _x$slug)"; exit 1 ;;
    esac
else
    case "$base" in
    *_x*) echo "core prefix $base carries an _x (extra flags) part, but this build was given none"; exit 1 ;;
    esac
fi

# has_all <line>: every word of the flags is a whole argument of the line.
has_all() {
    local w
    for w in "${words[@]}"; do
        case " $1 " in *" $w "*) ;; *) return 1 ;; esac
    done
    return 0
}
for bin in client server; do
    line=$(grep -E "^\+ \S*(gcc|cc|clang)\S* .* -o \S+/$bin " "$log" | tail -1)
    [ -n "$line" ] || { echo "no traced compile line for the bench $bin in $log"; exit 1; }
    has_all "$line" || { echo "the bench $bin was compiled without some of '$flags': ${line:0:200}"; exit 1; }
done

core="prefix reused (build.sh keys it on these flags)"
if grep -q '^+ make install ' "$log"; then
    n=0
    while IFS= read -r line; do
        n=$((n + 1))
        has_all "$line" || { echo "core unit compiled without some of '$flags': ${line:0:200}"; exit 1; }
    done < <(grep -E ' -c -o \S+ \S+\.c$' "$log")
    [ "$n" -gt 0 ] || { echo "'+ make install' traced but no core compile line in $log"; exit 1; }
    core="core rebuilt here, $n units carry them"
fi
echo "prefix $base, bench client and server compile lines carry them, $core"

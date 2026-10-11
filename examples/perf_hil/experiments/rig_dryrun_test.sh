#!/usr/bin/env bash
# rig_dryrun_test.sh - checks that rig_dryrun.sh cannot reach the rig (written 2026-10-11 after a dry run's scp, which
# rig_dryrun.sh did not stand in, copied files from the REAL Pis: scp execs /usr/bin/ssh itself, past the stand-in ssh
# on PATH). Every case below fails on rig_dryrun.sh as of 2709bedd (cases 2-3 are then not run: their scp would be the
# real one). Nothing here contacts any host: the one real ssh it starts has ProxyCommand=sleep and an .invalid name.
# PC only, needs `sudo -n ip`. ~10 s.
#
#   1  scp, sftp and rsync on the dry run's PATH are stand-ins, not /usr/bin's
#   2  scp <stand-in>:~/file copies the stand-in's file (and ssh starts in the stand-in's home, as on a Pi)
#   3  scp from a host rig_dryrun.sh does not map is refused (exit 255) - never handed to the real scp
#   4  a real /usr/bin/ssh started by the command in this PC's network namespace is caught: killed, and the dry run
#      exits 97 whatever the command's own status
# Prints "ok <n>" / "FAIL <n>: ..." per case and "rig_dryrun_test: PASS|FAIL"; exit 0 only on PASS.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRY=${DRY:-$HERE/rig_dryrun.sh}
T=$(mktemp -d /tmp/rig_dryrun_test.XXXXXX) || exit 2
trap 'rm -rf "$T"' EXIT
fail=0
check() { # check <n> <condition text> <0|1>
    if [ "$3" = 0 ]; then echo "ok $1 - $2"; else echo "FAIL $1: $2"; fail=1; fi
}
run() { DRYRUN_HOME=$T/home "$DRY" "$@" 2>>"$T/stderr"; }

out=$(run bash -c 'for t in scp sftp rsync; do command -v "$t"; done')
bad=$(printf '%s\n' "$out" | grep -cvF "$T/home/bin/")
check 1 "scp, sftp, rsync resolve to the stand-ins (got: $(echo "$out" | tr '\n' ' '))" "$([ "$bad" = 0 ] && [ -n "$out" ] && echo 0 || echo 1)"

if [ "$fail" != 0 ]; then
    # Never run the copies through a script whose scp is the real one: case 2 would read the real Pi.
    echo "FAIL 2: not run - scp is not stood in, so it would reach 10.1.1.214"
    echo "FAIL 3: not run - scp is not stood in"
else
    run bash -c "ssh ci@10.1.1.214 'echo stand-in-c > dryrun_test_file' && scp -q -o BatchMode=yes ci@10.1.1.214:dryrun_test_file '$T/got'" >/dev/null
    check 2 "scp copies the stand-in's ~/dryrun_test_file ($(cat "$T/got" 2>/dev/null || echo none))" \
        "$([ "$(cat "$T/got" 2>/dev/null)" = stand-in-c ] && [ -f "$T/home/home_c/dryrun_test_file" ] && echo 0 || echo 1)"

    run bash -c "scp -q ci@rig-dryrun-test.invalid:/etc/hostname '$T/x'; echo \"rc=\$?\"" >"$T/c3"
    check 3 "scp from an unmapped host is refused ($(cat "$T/c3"))" "$(grep -qx 'rc=255' "$T/c3" && [ ! -e "$T/x" ] && echo 0 || echo 1)"
fi

run bash -c '/usr/bin/ssh -o ProxyCommand="sleep 3" -o BatchMode=yes nobody@rig-dryrun-test.invalid true; exit 0' >/dev/null
rc=$?
check 4 "a real /usr/bin/ssh from the command is caught (exit $rc, want 97)" "$([ "$rc" = 97 ] && echo 0 || echo 1)"

if [ "$fail" = 0 ]; then echo "rig_dryrun_test: PASS"; else
    echo "rig_dryrun_test: FAIL (rig_dryrun.sh's stderr follows)"
    sed 's/^/    | /' "$T/stderr"
fi
exit "$fail"

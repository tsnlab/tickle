#!/usr/bin/env bash
# rig_dryrun.sh - run a rig harness end to end on this PC, against two stand-in Pis, without touching the rig.
# Written 2026-10-11 for gate v2, whose harness changes (perf stat around every bench, flags verified per build, a
# placement arm) are in the ssh command lines themselves - the part rig_preflight.sh does not run.
#
# HOW. A stand-in `ssh` first on PATH maps the rig's hosts to two private network namespaces:
#   10.1.1.214 (rpi#1, client) -> <id>c       10.1.1.213 (rpi#2, server) -> <id>s
# joined by a veth pair named eth0 in each (192.168.10.1/24 and .2/24, broadcast 192.168.10.255, the address the TickLE
# benches send to), each with lo up and a default route. Every "remote" command runs as this user (setpriv from the
# namespace's root shell), with HOME = a stand-in Pi home holding `tickle`, a clone of this repository whose origin is
# this repository's own object store (so `git fetch origin && git reset --hard <sha>` works for any local commit), and
# /dev/shm bound to that host's own directory (so the two hosts do not share segments, and one host's processes do).
# `sudo -n tc` inside a command shapes that namespace's eth0 only. RIG_LOCK_HELD_HIL=1 is set: the real lock is never
# taken or waited on, and the stand-in ssh refuses any host it does not map - it never execs the real ssh.
#
# Not a measurement: the PC is not a Pi, its numbers mean nothing, and perf counts x86 instructions. It shows that a
# harness builds, runs, counts, verifies and reads what it should, and refuses what it should.
#
# Usage: rig_dryrun.sh <command> [args...]      e.g. rig_dryrun.sh env SHA=<sha> FWS=tickle CELLS=2 REPS=1 \
#            examples/perf_hil/experiments/campaign_sweep.sh
#   DRYRUN_HOME=<dir> keeps (or reuses) the stand-in Pi homes and logs under <dir> (default: a fresh mktemp dir,
#   removed at exit unless DRYRUN_KEEP=1). Needs passwordless `sudo -n ip`.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
[ "$#" -gt 0 ] || { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
COMMON=$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir) || exit 2
if [ -n "${DRYRUN_HOME:-}" ]; then
    D=$DRYRUN_HOME
    mkdir -p "$D" || exit 2
else
    D=$(mktemp -d /tmp/rig_dryrun.XXXXXX) || exit 2
fi
ID=rd$$
NSC=${ID}c NSS=${ID}s
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() {
    sudo -n ip netns del "$NSC" 2>/dev/null
    sudo -n ip netns del "$NSS" 2>/dev/null
    if [ -z "${DRYRUN_HOME:-}" ] && [ "${DRYRUN_KEEP:-0}" != 1 ]; then rm -rf "$D"; fi
    return 0
}
trap cleanup EXIT
for h in c s; do
    mkdir -p "$D/home_$h" "$D/shm_$h" && chmod 1777 "$D/shm_$h" || exit 2
    if [ ! -d "$D/home_$h/tickle/.git" ]; then
        git clone -q --no-checkout "$COMMON" "$D/home_$h/tickle" || exit 2
    fi
    mkdir -p "$D/home_$h/.ssh"
done
setup_ns() {
    sudo -n ip netns add "$NSC" && sudo -n ip netns add "$NSS" &&
        sudo -n ip link add "${ID}a" type veth peer name "${ID}b" &&
        sudo -n ip link set "${ID}a" netns "$NSC" && sudo -n ip link set "${ID}b" netns "$NSS" &&
        sudo -n ip -n "$NSC" link set "${ID}a" name eth0 && sudo -n ip -n "$NSS" link set "${ID}b" name eth0 &&
        sudo -n ip -n "$NSC" addr add 192.168.10.1/24 brd + dev eth0 &&
        sudo -n ip -n "$NSS" addr add 192.168.10.2/24 brd + dev eth0 || return 1
    local n
    for n in "$NSC" "$NSS"; do
        sudo -n ip -n "$n" link set lo up && sudo -n ip -n "$n" link set eth0 up &&
            sudo -n ip -n "$n" route add default dev eth0 || return 1
    done
}
setup_ns || { echo "rig_dryrun: namespace setup failed (needs sudo -n ip)" >&2; exit 2; }

mkdir -p "$D/bin"
cat >"$D/bin/ssh" <<EOF
#!/usr/bin/env bash
# The stand-in ssh of rig_dryrun.sh: options are skipped, user@host picks the namespace, the rest is the command.
while [ \$# -gt 0 ]; do
    case "\$1" in
    -i | -o | -p | -l | -F) shift 2 ;;
    -*) shift ;;
    *) break ;;
    esac
done
target=\${1:?no host}
shift
case "\${target#*@}" in
10.1.1.214) ns=$NSC h=c ;;
10.1.1.213) ns=$NSS h=s ;;
*) echo "rig_dryrun ssh: refusing unmapped host \$target" >&2; exit 255 ;;
esac
cmd="\$*"
# A whole command of the form "sudo -n tc <args>" (how the harnesses shape the client's eth0; the Pis allow it, this PC
# does not) runs tc as the namespace's root, which reaches that namespace's eth0 and nothing else.
case "\$cmd" in
"sudo -n tc "*[\\;\\&\\|\\\`\\$\\<\\>]*) ;;
"sudo -n tc "*) read -ra a <<<"\${cmd#sudo -n }"; exec sudo -n ip netns exec "\$ns" "\${a[@]}" ;;
esac
# shellcheck disable=SC2016 # \$1.. are the inner sh's
exec sudo -n ip netns exec "\$ns" sh -c 'mount --bind "\$1" /dev/shm || exit 255; shift; exec "\$@"' sh "$D/shm_\$h" \\
    setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups \\
    env HOME="$D/home_\$h" PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" LANG=C bash -c "\$cmd"
EOF
chmod +x "$D/bin/ssh"
echo "rig_dryrun: hosts 10.1.1.214 -> $NSC, 10.1.1.213 -> $NSS; stand-in homes under $D" >&2
PATH="$D/bin:$PATH" RIG_LOCK_HELD_HIL=1 RIG_LOCK_HELD=1 "$@"
rc=$?
echo "rig_dryrun: command exited $rc" >&2
exit "$rc"

#!/usr/bin/env bash
# rig_dryrun.sh - run a rig harness end to end on this PC, against two stand-in Pis, without touching the rig.
# Written 2026-10-11 for gate v2, whose harness changes (perf stat around every bench, flags verified per build, a
# placement arm) are in the ssh command lines themselves - the part rig_preflight.sh does not run.
#
# HOW. A stand-in `ssh` (and `scp`; rsync is refused) first on PATH maps the rig's hosts to two private network namespaces:
#   10.1.1.214 (rpi#1, client) -> <id>c       10.1.1.213 (rpi#2, server) -> <id>s
# joined by a veth pair named eth0 in each (192.168.10.1/24 and .2/24, broadcast 192.168.10.255, the address the TickLE
# benches send to), each with lo up and a default route. Every "remote" command runs as this user (setpriv from the
# namespace's root shell), with HOME = a stand-in Pi home holding `tickle`, a clone of this repository whose origin is
# this repository's own object store (so `git fetch origin && git reset --hard <sha>` works for any local commit), and
# /dev/shm bound to that host's own directory (so the two hosts do not share segments, and one host's processes do).
# PYTHONUSERBASE stays this user's ~/.local: its empy 3 is what the Pis' ROS has, where this PC's system empy is 4.
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
#   DRYRUN_PI_HOME=/home/<name> (e.g. /home/ci, the rig's): each stand-in's home is ALSO seen at that path by its own
#   remote commands (HOME is it), as on the Pis - for harnesses that name the Pi home in their command lines (the rmw
#   harnesses' PI_HOME). Done in the remote command's private mount namespace: a tmpfs over /home holding a bind of
#   every entry of the real /home plus <name> -> that host's stand-in home. scp maps the path back. Default: unset,
#   HOME is the stand-in directory itself.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
[ "$#" -gt 0 ] || { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
COMMON=$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir) || exit 2
PIH=${DRYRUN_PI_HOME:-}
case "$PIH" in
"") ;;
/home/[A-Za-z0-9_]*) case "${PIH#/home/}" in *[!A-Za-z0-9_.-]*) echo "DRYRUN_PI_HOME must be /home/<name>" >&2; exit 2 ;; esac
    [ -e "$PIH" ] && { echo "DRYRUN_PI_HOME=$PIH exists on this PC; refusing to shadow it" >&2; exit 2; } ;;
*) echo "DRYRUN_PI_HOME must be /home/<name>, not '$PIH'" >&2; exit 2 ;;
esac
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
    [ -n "$PIH" ] && sudo -n ip netns exec "$NSC" rmdir "/tmp/.rig_dryrun_home_$ID" 2>/dev/null
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
home="$D/home_\$h" pre=""
if [ -n "$PIH" ]; then
    # In this command's own mount namespace: /home becomes a tmpfs holding every real /home entry (bound back through a
    # staging bind) and $PIH bound to this host's stand-in home. Nothing outside the namespace sees it.
    st=/tmp/.rig_dryrun_home_$ID
    pre="mkdir -p \$st && mount --rbind /home \$st && mount -t tmpfs -o mode=755 tmpfs /home &&
for e in \$st/* \$st/.[!.]*; do [ -e \"\\\$e\" ] || continue; n=\\\${e##*/}; mkdir -p \"/home/\\\$n\" && mount --rbind \"\\\$e\" \"/home/\\\$n\" || exit 255; done &&
mkdir -p $PIH && mount --bind \"\$home\" $PIH || exit 255
"
    home=$PIH
fi
exec sudo -n ip netns exec "\$ns" sh -c "\$pre"'mount --bind "\$1" /dev/shm || exit 255; shift; exec "\$@"' sh "$D/shm_\$h" \\
    setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups \\
    env HOME="\$home" PYTHONUSERBASE="$HOME/.local" PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" LANG=C bash -c "cd ~ || exit 255; \$cmd"
EOF
chmod +x "$D/bin/ssh"
# The stand-in scp: [user@]<host>:<path> of a mapped host is that stand-in's file - an absolute path is this PC's (the
# namespaces share its filesystem; only /dev/shm is per host), a relative or ~/ one is under the stand-in's home. Any
# other host is refused and the real scp is never run: before this, a dry run's scp copied from the real Pis.
# Plain copies only (no -r).
cat >"$D/bin/scp" <<EOF
#!/usr/bin/env bash
args=()
while [ \$# -gt 0 ]; do
    case "\$1" in
    -i | -o | -P | -l | -F) shift 2 ;;
    -r) echo "rig_dryrun scp: -r is not supported" >&2; exit 255 ;;
    -*) shift ;;
    *) args+=("\$1"); shift ;;
    esac
done
map() { # map <arg>: the local path an scp argument names
    case "\$1" in *:*) ;; *) printf '%s' "\$1"; return 0 ;; esac
    local t=\${1%%:*} p=\${1#*:} h
    case "\${t#*@}" in
    10.1.1.214) h=c ;;
    10.1.1.213) h=s ;;
    *) echo "rig_dryrun scp: refusing unmapped host \$t" >&2; return 1 ;;
    esac
    case "\$p" in
    ${PIH:-/nonexistent-pi-home}/*) printf '%s' "$D/home_\$h/\${p#${PIH:-/nonexistent-pi-home}/}" ;;
    /*) printf '%s' "\$p" ;;
    "~/"*) printf '%s' "$D/home_\$h/\${p#\~/}" ;;
    *) printf '%s' "$D/home_\$h/\$p" ;;
    esac
}
[ \${#args[@]} -ge 2 ] || { echo "rig_dryrun scp: want <src>... <dst>" >&2; exit 255; }
out=()
# A remote source may be a glob (the Pi's shell would expand it): expanded here, unmatched left as is (cp says so).
for a in "\${args[@]}"; do
    m=\$(map "\$a") || exit 255
    case "\$a" in
    *:*[*?[]*) for f in \$m; do out+=("\$f"); done ;;
    *) out+=("\$m") ;;
    esac
done
exec cp -- "\${out[@]}"
EOF
chmod +x "$D/bin/scp"
# rsync is not stood in: refused outright, so a harness that grows one cannot reach the rig from a dry run.
printf '%s\n' '#!/bin/sh' 'echo "rig_dryrun: rsync is not stood in; refusing" >&2' 'exit 255' >"$D/bin/rsync"
chmod +x "$D/bin/rsync"
printf '%s\n' '#!/bin/sh' 'echo "rig_dryrun: sftp is not stood in; refusing" >&2' 'exit 255' >"$D/bin/sftp"
chmod +x "$D/bin/sftp"
# Every remote-access tool a harness can name must resolve to a stand-in, or nothing runs.
for t in ssh scp sftp rsync; do
    got=$(PATH="$D/bin:$PATH" command -v "$t")
    [ "$got" = "$D/bin/$t" ] || { echo "rig_dryrun: '$t' resolves to '$got', not the stand-in; refusing" >&2; exit 2; }
done
echo "rig_dryrun: hosts 10.1.1.214 -> $NSC, 10.1.1.213 -> $NSS; stand-in homes under $D" >&2
PATH="$D/bin:$PATH" RIG_LOCK_HELD_HIL=1 RIG_LOCK_HELD=1 "$@" &
cmd_pid=$!
# THE WATCHDOG. A stand-in on PATH does not stop a tool that execs the real client by path (scp runs /usr/bin/ssh
# itself, which is how a dry run once read the real Pis). So while the command runs, every 0.1 s: any process whose
# /proc exe is a real ssh/scp/sftp/rsync, in THIS PC's network namespace (one inside a stand-in's namespace cannot reach
# 10.1.1.x), and descended from the command, is recorded and killed, and the dry run fails (exit 97) however the
# command ends. Sampling can miss a client that lives under 0.1 s; an ssh session to a Pi does not.
REAL_LOG=$D/real_remote_access.log
: >"$REAL_LOG"
my_net=$(readlink /proc/self/ns/net)
descends() { # descends <pid>: is <pid> the command or below it?
    local p=$1 n=0
    while [ "$p" -gt 1 ] && [ "$n" -lt 64 ]; do
        [ "$p" = "$cmd_pid" ] && return 0
        p=$(awk '/^PPid:/ { print $2 }' "/proc/$p/status" 2>/dev/null) || return 1
        [ -n "$p" ] || return 1
        n=$((n + 1))
    done
    return 1
}
while [ -d "/proc/$cmd_pid" ] && [ "$(awk '/^State:/ { print $2 }' "/proc/$cmd_pid/status" 2>/dev/null)" != Z ]; do
    for e in /proc/[0-9]*/exe; do
        x=$(readlink "$e" 2>/dev/null) || continue
        case "$x" in /usr/bin/ssh | /usr/bin/scp | /usr/bin/sftp | /usr/bin/rsync) ;; *) continue ;; esac
        p=${e#/proc/}
        p=${p%/exe}
        [ "$(readlink "/proc/$p/ns/net" 2>/dev/null)" = "$my_net" ] || continue
        descends "$p" || continue
        echo "$(date -Is) pid $p $x: $(tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null)" >>"$REAL_LOG"
        [ "$(readlink "/proc/$p/exe" 2>/dev/null)" = "$x" ] && kill -KILL "$p" 2>/dev/null
    done
    sleep 0.1
done
wait "$cmd_pid"
rc=$?
echo "rig_dryrun: command exited $rc" >&2
if [ -s "$REAL_LOG" ]; then
    echo "rig_dryrun: FAILED - the command ran a real remote client from this PC's network (killed):" >&2
    sed 's/^/    /' "$REAL_LOG" >&2
    exit 97
fi
exit "$rc"

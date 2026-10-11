#!/usr/bin/env bash
# hybrid_send_netns.sh - the hybrid transport over real IP fragmentation (DESIGN.md section 8, "Hybrid transport"): runs,
# MTU repairs, the reader's control law and the fragment-path rule, mixed peers - between private network namespaces on
# this PC, never the default one.
#
# TOPOLOGY. A writer namespace and three reader namespaces, each with one veth whose other end is on a bridge in a fifth
# namespace: one link, 192.168.10.0/24, the writer at .1 and the readers at .2-.4, so unicast, broadcast and IP
# fragmentation all work as on a switch. large_sample_bench.c at every end, built from this tree as rmw_tickle builds
# core (tt_MAX_BUFFER_LENGTH 65507, large samples) and with the shared-memory transport compiled out (the namespaces
# share /dev/shm). Loss is netem on the writer's egress, per frame - an IP fragment is a frame. tcpdump on each reader's
# interface records what reached it, read by hybrid_pcap.py.
#
# HOW TO READ IT, written before the first run (2026-10-11) and implemented in verdict() below, not only here:
#   Every cell: delivered = published, torn = 0, out_of_order = 0 (RELIABLE is KEEP_ALL, so nothing may be let go),
#     unless the row says otherwise; the RESULT
#     lines present; else the cell is FAIL. A cell whose realized treatment is not the one named (runs where MTU was
#     asked, none where runs were) is VOID - the arm did not apply what it says.
#   RELIABLE under loss (1%, 5%; AUTO and forced IP): every repair the readers saw is an MTU unit - repair_runs = 0,
#     repair_fragmented = 0, repair_max_bytes <= 1472 - and there were repairs (else the loss never bit: VOID).
#   AUTO at 0%: best effort sends runs from the start (the reader's slow-start cap, 2, then doubling); RELIABLE leaves
#     n = 1 once the reader announces its p and grows with it, so its n histogram must show runs (bucket 45) by the end
#     of the run. Forced IP: its readers run with the cap off (-C, as a forced arm's must:
#     _tt_CONFIG.reassembly_cap_off), and every sample must go at n_max (VOID otherwise).
#   MTU control: no run on the wire at all.
#   Mixed by unicast (two readers, one advertising 0, -X 0): the 0 reader's capture holds no run and no fragmented
#     frame; the other's holds runs. Broadcast (three readers, above the unicast threshold of 2) with one advertising 0:
#     no run to anyone; with all three default: runs, by broadcast.
#   A u32 filter dropping every IP fragment at the reader: RELIABLE KEEP_ALL still delivers everything (MTU repairs),
#     and the writer judges the path failed on the first sample whose runs are all lost while its tail arrives: at most
#     two samples go with n >= 2 (n histogram buckets 2..45). REL_1MB_FRAGDROP_MTU (forced MTU, twice: the A/A pair) is
#     the arm whose rate and CPU it must match.
#   The same filter under best effort (rule (e) on the reader's side): the reader records path_failures >= 1 and
#     delivers at least half of what was published (everything after the first samples, as MTU units).
#   The squeeze (the reader's control law, decision 3, with the kernel's own reassembly at its defaults): best effort
#     AUTO, 1 MB at 30 Hz through 0.1% netem loss - on the PC q(8) was 0.46 there. hybrid_law_log.py reads the reader's
#     log: SQUEEZE_WAIT - a collapse, the cap dropped at once to round(0.7 x the ceiling), no probe before 2 tau, one by
#     2 tau + 5 s; SQUEEZE_HALVE - after the collapse the rate halves (-H) and the ceiling doubles ("R p fell"). Both
#     with the RESULT line's collapses >= 1 and nothing torn.
#
# NOT a performance figure: one PC, veth, a software bridge. hybrid_pc_ab.sh is the cost A/B; the rig is the campaign.
#
# Usage: hybrid_send_netns.sh [DURATION_S]      results to $OUT (default ~/hybrid_netns/<stamp>/results.txt)
# CELLS="NAME ..." runs only those cells; the squeeze cells set their own, longer durations.
# Needs passwordless `sudo -n ip` (tc, tcpdump and sysctl run inside the namespaces through `ip netns exec`).
set -u
DURATION=${1:-12}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
STAMP=$(date +%Y%m%d_%H%M%S)
WORK=${WORK:-$HOME/hybrid_netns/$STAMP}
OUT=${OUT:-$WORK/results.txt}
BIN=$WORK/large_sample_bench
P=hyb$$
NS_W=$P-w
NS_SW=$P-sw
READERS=("$P-r1" "$P-r2" "$P-r3")
IPS=(192.168.10.2 192.168.10.3 192.168.10.4)

cleanup() {
    for ns in "$NS_W" "$NS_SW" "${READERS[@]}"; do
        sudo -n ip netns del "$ns" 2>/dev/null
    done
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK"
sha=$(git -C "$TREE" rev-parse --short HEAD)
dirty=$(git -C "$TREE" status --porcelain -- src include | wc -l)
cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    -I"$TREE/include" -I"$TREE/src" -o "$BIN" "$HERE/large_sample_bench.c" "$TREE/src/tickle.c" \
    "$TREE/src/encoding.c" "$TREE/src/log.c" "$TREE/src/hal_linux.c" -lpthread -lm 2>"$WORK/build.log" || {
    echo "build failed: $WORK/build.log"
    exit 1
}

cleanup
for ns in "$NS_W" "$NS_SW" "${READERS[@]}"; do
    sudo -n ip netns add "$ns" || exit 1
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NS_SW" link add br0 type bridge
sudo -n ip -n "$NS_SW" link set br0 up
attach() { # NS NAME IP
    sudo -n ip link add "$2" netns "$1" type veth peer name "$2-p" netns "$NS_SW" || exit 1
    sudo -n ip -n "$NS_SW" link set "$2-p" master br0
    sudo -n ip -n "$NS_SW" link set "$2-p" up
    sudo -n ip -n "$1" addr add "$3/24" dev "$2"
    sudo -n ip -n "$1" link set "$2" up
}
attach "$NS_W" hw 192.168.10.1
for i in 0 1 2; do
    attach "${READERS[$i]}" "hr$((i + 1))" "${IPS[$i]}"
done

echo "=== hybrid_send_netns $(date -Is) tree=$sha dirty_core_files=$dirty duration=${DURATION}s ===" | tee -a "$OUT"

field() { # LINE KEY
    printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}

# shaping LOSS_PERCENT FILTER(none|frags) - netem on the writer's egress, the filter at every reader's ingress
shaping() {
    sudo -n ip netns exec "$NS_W" tc qdisc del dev hw root 2>/dev/null
    if [ "$1" != 0 ]; then
        sudo -n ip netns exec "$NS_W" tc qdisc add dev hw root netem loss "$1%" || return 1
    fi
    for i in 0 1 2; do
        local ns=${READERS[$i]} dev=hr$((i + 1))
        sudo -n ip netns exec "$ns" tc qdisc del dev "$dev" ingress 2>/dev/null
        sudo -n ip netns exec "$ns" sysctl -qw net.ipv4.ipfrag_time=30
        case "$2" in
        frags) # pass what IP did not fragment (MF clear, offset 0), drop every fragment
            sudo -n ip netns exec "$ns" tc qdisc add dev "$dev" handle ffff: ingress
            sudo -n ip netns exec "$ns" tc filter add dev "$dev" parent ffff: prio 1 protocol ip u32 \
                match u16 0 0x3fff at 6 action ok
            sudo -n ip netns exec "$ns" tc filter add dev "$dev" parent ffff: prio 2 protocol ip u32 \
                match u32 0 0 action drop
            ;;
        esac
    done
}

# cell NAME READERS SIZE RATE LOSS FILTER "PUB_FLAGS" "SUB_FLAGS_1|SUB_FLAGS_2|SUB_FLAGS_3" CHECKS [DURATION_S]
# CHECKS (space separated): reliable best_effort runs no_runs repairs_mtu n_reaches_max n_all_max fail_fast mixed0
#   path_rx squeeze_wait squeeze_halve
cell() {
    local name=$1 count=$2 size=$3 rate=$4 loss=$5 filter=$6 pub_flags=$7 sub_flags=$8 checks=$9
    local DURATION=${10:-$DURATION}
    shaping "$loss" "$filter" || { echo "$name VOID: shaping failed" | tee -a "$OUT"; return; }
    local dir=$WORK/$name i
    mkdir -p "$dir"
    IFS='|' read -r -a flags <<<"$sub_flags"
    for ((i = 0; i < count; i++)); do
        # shellcheck disable=SC2024 # the capture is written by root inside the namespace: intended
        sudo -n ip netns exec "${READERS[$i]}" timeout $((DURATION + 12)) tcpdump -q -s 128 -i "hr$((i + 1))" \
            -w "$dir/r$((i + 1)).pcap" "src host 192.168.10.1" >"$dir/tcpdump$((i + 1)).log" 2>&1 &
    done
    sleep 1
    for ((i = 0; i < count; i++)); do
        # shellcheck disable=SC2024,SC2086 # flags are deliberately word-split
        sudo -n ip netns exec "${READERS[$i]}" "$BIN" sub -s "$size" -d $((DURATION + 7)) ${flags[$i]:-} \
            >"$dir/sub$((i + 1)).log" 2>&1 &
    done
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS_W" "$BIN" pub -s "$size" -r "$rate" -d "$DURATION" $pub_flags >"$dir/pub.log" 2>&1
    wait
    local publine subline verdict="PASS" why="" pcap
    publine=$(grep -m1 '^RESULT:' "$dir/pub.log")
    [ -z "$publine" ] && { verdict=FAIL; why="$why no_pub_result"; }
    local published
    published=$(field "$publine" published)
    for ((i = 0; i < count; i++)); do
        subline=$(grep -m1 '^RESULT:' "$dir/sub$((i + 1)).log")
        pcap=$(python3 "$HERE/hybrid_pcap.py" "$dir/r$((i + 1)).pcap" 192.168.10.1 2>/dev/null)
        echo "$name r$((i + 1)) | sub: ${subline#RESULT: } | wire: $pcap" >>"$OUT"
        if [ -z "$subline" ] || [ -z "$pcap" ]; then
            verdict=FAIL
            why="$why r$((i + 1)):no_result_or_capture"
            continue
        fi
        local delivered torn ooo runs repairs repair_runs repair_frag repair_max frag_frames path_failures collapses
        delivered=$(field "$subline" delivered)
        torn=$(field "$subline" torn)
        ooo=$(field "$subline" out_of_order)
        path_failures=$(field "$subline" hybrid_path_failures)
        collapses=$(field "$subline" hybrid_collapses)
        runs=$(field "$pcap" runs)
        repairs=$(field "$pcap" repairs)
        repair_runs=$(field "$pcap" repair_runs)
        repair_frag=$(field "$pcap" repair_fragmented)
        repair_max=$(field "$pcap" repair_max_bytes)
        frag_frames=$(field "$pcap" fragmented_frames)
        [ "${torn:-1}" != 0 ] && { verdict=FAIL; why="$why r$((i + 1)):torn=$torn"; }
        case " $checks " in *" reliable "*)
            [ "$delivered" != "$published" ] && { verdict=FAIL; why="$why r$((i + 1)):delivered=$delivered/$published"; }
            [ "${ooo:-1}" != 0 ] && { verdict=FAIL; why="$why r$((i + 1)):out_of_order=$ooo"; } ;;
        esac
        case " $checks " in *" best_effort "*)
            [ "$delivered" != "$published" ] && { verdict=FAIL; why="$why r$((i + 1)):delivered=$delivered/$published"; } ;;
        esac
        case " $checks " in *" runs "*)
            [ "${runs:-0}" = 0 ] && { [ "$verdict" = PASS ] && verdict=VOID; why="$why r$((i + 1)):no_runs"; } ;;
        esac
        case " $checks " in *" no_runs "*)
            [ "${runs:-1}" != 0 ] && { [ "$verdict" = PASS ] && verdict=VOID; why="$why r$((i + 1)):runs=$runs"; } ;;
        esac
        case " $checks " in *" mixed0 "*)
            if [ "$i" = 1 ] && { [ "$runs" != 0 ] || [ "$frag_frames" != 0 ]; }; then
                verdict=FAIL
                why="$why r2(advertises_0):runs=$runs,fragmented_frames=$frag_frames"
            fi ;;
        esac
        case " $checks " in *" repairs_mtu "*)
            [ "${repairs:-0}" = 0 ] && { [ "$verdict" = PASS ] && verdict=VOID; why="$why r$((i + 1)):no_repairs"; }
            if [ "${repair_runs:-1}" != 0 ] || [ "${repair_frag:-1}" != 0 ] || [ "${repair_max:-9999}" -gt 1472 ]; then
                verdict=FAIL
                why="$why r$((i + 1)):repair_runs=$repair_runs,repair_fragmented=$repair_frag,repair_max=$repair_max"
            fi ;;
        esac
        case " $checks " in *" path_rx "*)
            [ "${path_failures:-0}" = 0 ] && { verdict=FAIL; why="$why r$((i + 1)):path_failures=0"; }
            [ $((${delivered:-0} * 2)) -lt "${published:-1}" ] && {
                verdict=FAIL
                why="$why r$((i + 1)):delivered=$delivered/$published"
            } ;;
        esac
        local mode law
        for mode in squeeze_wait squeeze_halve; do
            case " $checks " in *" $mode "*)
                [ "${collapses:-0}" = 0 ] && { verdict=FAIL; why="$why r$((i + 1)):collapses=0"; }
                law=$(python3 "$HERE/hybrid_law_log.py" "$mode" "$dir/sub$((i + 1)).log" "$dir/pub.log")
                echo "$name r$((i + 1)) | $law" >>"$OUT"
                case "$law" in *"verdict=PASS"*) ;; *) verdict=FAIL; why="$why r$((i + 1)):${law#law: }" ;; esac ;;
            esac
        done
    done
    local hist
    hist=$(field "$publine" hybrid_n_hist)
    case " $checks " in *" n_reaches_max "*)
        [ "${hist##*/}" = 0 ] && { verdict=FAIL; why="$why n_hist=$hist:never_n_max"; } ;;
    esac
    case " $checks " in *" n_all_max "*)
        # A forced-IP arm: every sample at n_max, or the arm did not apply what it says.
        local below
        below=$(echo "$hist" | awk -F/ '{print $1+$2+$3+$4+$5+$6}')
        [ "${below:-1}" != 0 ] && { [ "$verdict" = PASS ] && verdict=VOID; why="$why n_hist=$hist:not_all_n_max"; } ;;
    esac
    case " $checks " in *" fail_fast "*)
        # The path is judged failed on the first sample whose runs are all lost: at most two samples with n >= 2.
        local tried
        tried=$(echo "$hist" | awk -F/ '{print $2+$3+$4+$5+$6+$7}')
        [ "${tried:-99}" -gt 2 ] && { verdict=FAIL; why="$why n_hist=$hist:more_than_2_samples_in_runs"; } ;;
    esac
    echo "$name: $verdict${why:+ ($why)} | pub: ${publine#RESULT: }" | tee -a "$OUT"
}

M=1048576
F=4194304
# name readers size rate loss filter pub_flags sub_flags checks
want() { [ -z "${CELLS:-}" ] || case " $CELLS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
want BE_1MB_AUTO_0 && cell BE_1MB_AUTO_0 1 $M 30 0 none "-F auto" "" "best_effort runs"
want BE_1MB_MTU_0 && cell BE_1MB_MTU_0 1 $M 30 0 none "-F mtu" "" "best_effort no_runs"
want REL_1MB_AUTO_0 && cell REL_1MB_AUTO_0 1 $M 30 0 none "-R -A -F auto" "-R" "reliable runs n_reaches_max"
want REL_1MB_AUTO_1 && cell REL_1MB_AUTO_1 1 $M 30 1 none "-R -A -F auto" "-R" "reliable repairs_mtu"
want REL_1MB_AUTO_5 && cell REL_1MB_AUTO_5 1 $M 30 5 none "-R -A -F auto" "-R" "reliable repairs_mtu"
want REL_1MB_IP_1 && cell REL_1MB_IP_1 1 $M 30 1 none "-R -A -F ip" "-R -C" "reliable runs repairs_mtu n_all_max"
want REL_1MB_IP_5 && cell REL_1MB_IP_5 1 $M 30 5 none "-R -A -F ip" "-R -C" "reliable runs repairs_mtu n_all_max"
want REL_4MB_AUTO_1 && cell REL_4MB_AUTO_1 1 $F 15 1 none "-R -A -F auto" "-R" "reliable repairs_mtu"
want REL_4MB_IP_5 && cell REL_4MB_IP_5 1 $F 15 5 none "-R -A -F ip" "-R -C" "reliable runs repairs_mtu n_all_max"
want REL_16K_IP_5 && cell REL_16K_IP_5 1 16384 100 5 none "-R -A -F ip" "-R -C" "reliable runs repairs_mtu n_all_max"
want REL_1MB_FRAGDROP && cell REL_1MB_FRAGDROP 1 $M 30 0 frags "-R -A -F auto" "-R" "reliable fail_fast"
want REL_1MB_FRAGDROP_MTU && cell REL_1MB_FRAGDROP_MTU 1 $M 30 0 frags "-R -A -F mtu" "-R" "reliable no_runs"
want REL_1MB_FRAGDROP_MTU2 && cell REL_1MB_FRAGDROP_MTU2 1 $M 30 0 frags "-R -A -F mtu" "-R" "reliable no_runs"
want MIXED_UNICAST && cell MIXED_UNICAST 2 $M 30 0 none "-R -A -F ip" "-R -C|-R -X 0" "reliable mixed0"
want MIXED_BROADCAST && cell MIXED_BROADCAST 3 $M 30 0 none "-F auto" "|-X 0|" "best_effort no_runs"
want BROADCAST_RUNS && cell BROADCAST_RUNS 3 $M 30 0 none "-F auto" "||" "best_effort runs"
want BE_FRAGDROP && cell BE_FRAGDROP 1 $M 30 0 frags "-F auto" "" "path_rx"
want SQUEEZE_WAIT && cell SQUEEZE_WAIT 1 $M 30 0.1 none "-F auto" "" "squeeze_wait" 110
want SQUEEZE_HALVE && cell SQUEEZE_HALVE 1 $M 30 0.1 none "-F auto -H 55" "" "squeeze_halve" 70
echo "=== done $(date -Is) ===" | tee -a "$OUT"

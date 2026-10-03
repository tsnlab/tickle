#!/usr/bin/env bash
# FastDDS 의 same-host 빠른 경로는 전송(SHM transport)인가 data-sharing 인가 — S6 의 fastdds 분모가 맞는지.
#
# 2026-10-03 S6 를 best_effort p4 로 돌렸을 때 fastdds 두 arm 이 **둘 다** wire_packets_per_sample=0.000
# 이었다. 공유메모리 전송을 뺀 arm(fastdds_eth0_only.xml)도 루프백에 아무것도 올리지 않았다는 뜻이다.
# 그 arm 은 S6 에서 "커널 경로 분모" 역할을 한다. 분모가 커널을 쓰지 않았다면 S6 의 fastdds 비율은
# 전부 잘못된 것으로 나눠진 것이고, RMW_GAPS_PLAN 에 이미 실린 값들도 포함된다.
#
# 가설: FastDDS data-sharing 이 샘플을 나른다. data-sharing 은 전송 계층을 우회하므로
# useBuiltinTransports=false 로 SHM *전송* 을 빼도 꺼지지 않는다.
#
# 왜 끄지 않고 관찰하는가: 벤치가 DataWriterQos 를 코드에서 만들어 넘기므로(best_effort_throughput/
# client.cpp) XML 의 <data_sharing> 이 이 writer 에 닿지 않는다. 끄려면 코드 변경이 필요하고, 그것을
# 짓기 전에 지을 가치가 있는지를 이 관찰이 정한다. data-sharing 은 /dev/shm 에 fast_datasharing* 세그먼트를
# 남기므로, 보는 것만으로 답이 나온다.
#
# 실행 전 판정 (코드로 검사한다):
#   BEFORE 에 fast_datasharing* 이 이미 있으면 -> VOID. 이전 실행의 잔재와 이번 실행을 구분할 수 없다.
#   eth0_only(OFF) arm 의 DURING 에 있으면    -> data-sharing 이 날랐다. fastdds_eth0_only.xml 은 커널 경로
#                                               분모가 아니고, S6 의 fastdds 비율은 전부 분모가 틀렸다.
#   OFF 에 없고 SHM arm 에만 있으면           -> 전송이 가른다. S6 의 분모는 유효하고 0.000 의 원인은 딴 데 있다.
#   양쪽 다 없으면                            -> 가설 사망. data-sharing 이 아니므로 .off() 를 짓지 않는다.
#                                               0.000 의 원인을 다시 찾아야 한다.
#   음성 대조군: tickle arm 을 같은 방식으로 돌려 fast_datasharing* 이 없는 것을 확인한다. 관찰이 음성으로
#     나올 수 있다는 것을 보이지 못하면, 양성은 "항상 보이는 것" 과 구분되지 않는다.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
export RIG_LOCK_HELD_HIL=1

K=$HOME/.ssh/tickle_ci_ed25519
HOST=${HOST:-10.1.1.214}
DUR=${DUR:-5}
SCEN=${SCEN:-best_effort_throughput}
SIZE=${SIZE:-p4}
FDDS_LIB_PATH=${FDDS_LIB_PATH:-/opt/ros/jazzy/lib}
OUT=${OUT:-$HOME/rig_results_safe/fastdds_datasharing_witness.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

say "=== fastdds data-sharing 증인 $(date -Is) host=$HOST scen=$SCEN size=$SIZE ==="

shm_list() { sh_ "ls -1 /dev/shm 2>/dev/null | grep -i datasharing || true" </dev/null; }

# PID 로 죽인다, 패턴이 아니라. pkill -f '<방금 내가 친 문자열>' 은 ssh 가 띄운 자기 쉘의 명령줄에도
# 그 문자열이 들어 있어서 자기 자신을 잡을 수 있다 - 이 저장소가 이미 한 번 치른 사고이고, 죽이지 않을
# 때조차 거짓말을 한다. 그래서 띄울 때 $$ 를 파일에 적고, 보내기 전에 /proc/PID/exe 로 그게 무엇인지
# 확인한다. s6_witness_check.sh 의 kill_server 와 같은 방식이다.
kill_all() {
    sh_ "for f in /tmp/dsw_srv.pid /tmp/dsw_cli.pid /tmp/dsw_t_srv.pid /tmp/dsw_t_cli.pid; do
  [ -f \"\$f\" ] || continue
  pid=\$(cat \"\$f\" 2>/dev/null) || continue
  case \"\$(readlink /proc/\$pid/exe 2>/dev/null)\" in
    */${SCEN}_${SIZE}/server|*/${SCEN}_${SIZE}/client) kill -INT \"\$pid\" 2>/dev/null ;;
  esac
  rm -f \"\$f\"
done
sleep 2; true" </dev/null >/dev/null 2>&1
}
trap 'kill_all' EXIT

before="$(shm_list)"
say "--- BEFORE: /dev/shm 의 datasharing 세그먼트 ---"
say "${before:-(없음)}"
if [ -n "$before" ]; then
    say ""
    say "VOID: 시작 전에 이미 datasharing 세그먼트가 있다. 이번 실행이 만든 것과 이전 실행의 잔재를"
    say "  구분할 수 없으므로 아무것도 결론짓지 않는다. 정리한 뒤 다시 돌릴 것."
    exit 0
fi

# fdds_arm <라벨> <프로파일>: 서버를 띄우고 클라이언트를 돌리는 동안 /dev/shm 을 본다.
fdds_arm() {
    local label=$1 prof=$2 dir=/home/ci/tickle/examples/perf_hil/fastdds
    kill_all
    say ""
    say "--- arm=$label profile=$prof ---"
    sh_ "test -f $dir/$prof" </dev/null || { say "  FATAL: $prof 가 $HOST 에 없다"; return 1; }
    local envc="BENCH_IFACE=lo LD_LIBRARY_PATH=$FDDS_LIB_PATH FASTRTPS_DEFAULT_PROFILES_FILE=$dir/$prof"
    sh_ "cd $dir/${SCEN}_${SIZE} && (setsid sh -c 'echo \$\$ >/tmp/dsw_srv.pid; exec env $envc taskset -c 1 ./server -d $((DUR + 30))' >/tmp/dsw_server.log 2>&1 </dev/null &); sleep 3; true" </dev/null >/dev/null
    sh_ "cd $dir/${SCEN}_${SIZE} && (setsid sh -c 'echo \$\$ >/tmp/dsw_cli.pid; exec env $envc taskset -c 2 ./client -d $DUR' >/tmp/dsw_client.log 2>&1 </dev/null &); sleep 3; true" </dev/null >/dev/null
    local during; during="$(shm_list)"
    say "  DURING: ${during:-(없음)}"
    sleep "$DUR"
    kill_all
    say "  클라이언트가 RESULT 를 냈는가: $(sh_ "grep -c '^RESULT' /tmp/dsw_client.log 2>/dev/null || echo 0" </dev/null)"
    printf '%s' "$during"
}

off_seen="$(fdds_arm OFF_eth0_only fastdds_eth0_only.xml)"
on_seen="$(fdds_arm ON_shm_and_eth0 fastdds_shm_and_eth0.xml)"

# 음성 대조군: tickle 은 FastDDS data-sharing 을 쓰지 않으므로 여기서 fast_datasharing* 이 나오면
# 이 관찰 자체가 arm 을 구분하지 못한다는 뜻이다.
say ""
say "--- 음성 대조군: tickle 같은 셀 ---"
kill_all
tdir=/home/ci/tickle/examples/perf_hil/tickle
sh_ "cd $tdir/${SCEN}_${SIZE} 2>/dev/null && (setsid sh -c 'echo \$\$ >/tmp/dsw_t_srv.pid; exec env BENCH_IFACE=lo taskset -c 1 ./server -d $((DUR + 20))' >/tmp/dsw_t_server.log 2>&1 </dev/null &); sleep 2; true" </dev/null >/dev/null 2>&1
sh_ "cd $tdir/${SCEN}_${SIZE} 2>/dev/null && (setsid sh -c 'echo \$\$ >/tmp/dsw_t_cli.pid; exec env BENCH_IFACE=lo taskset -c 2 ./client -d $DUR' >/tmp/dsw_t_client.log 2>&1 </dev/null &); sleep 2; true" </dev/null >/dev/null 2>&1
tickle_seen="$(shm_list)"
say "  DURING(tickle): ${tickle_seen:-(없음)}"
kill_all

say ""
say "=== 판정 ==="
if [ -n "$tickle_seen" ]; then
    say "VOID: 음성 대조군(tickle)에서도 datasharing 세그먼트가 보인다. 이 관찰은 arm 을 구분하지 못하므로"
    say "  fastdds 결과를 읽을 수 없다."
elif [ -n "$off_seen" ]; then
    say "판정: data-sharing 이 날랐다. 공유메모리 전송을 뺀 arm 에서도 세그먼트가 나타났다."
    say "  -> fastdds_eth0_only.xml 은 S6 의 커널 경로 분모가 아니다. S6 의 fastdds 비율은 전부 잘못된"
    say "     분모로 나눠진 것이고 RMW_GAPS_PLAN 에 실린 값도 포함된다. Dev 의 data_sharing().off() 가"
    say "     필요하고, 그것 없이는 fastdds 셀을 고칠 수 없다."
elif [ -n "$on_seen" ]; then
    say "판정: 전송이 가른다. SHM 전송 arm 에만 세그먼트가 있고 eth0-only arm 에는 없다."
    say "  -> S6 의 분모는 유효하다. wire_packets_per_sample=0.000 의 원인은 data-sharing 이 아니므로"
    say "     다시 찾아야 하고, .off() 를 짓지 않는다."
else
    say "판정: 가설 사망. 어느 arm 에서도 datasharing 세그먼트가 없다."
    say "  -> data-sharing 이 아니다. .off() 를 짓지 않는다. 0.000 의 원인을 다시 찾을 것."
fi
say "=== done $(date -Is) ==="

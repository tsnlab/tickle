#!/usr/bin/env bash
# 대기는 datagram 당인가 seq_no 당인가 — 조각 3 이 2배인지 12% 인지를 가른다.
#
# 오늘까지 측정된 것: p4 RELIABLE 은 샘플당 8.21 us, 슬롯 2개. 전송은 슬롯당 0.92 us(BEST_EFFORT),
# RELIABLE 이 슬롯당 3.17 us 를 더 쓴다. 용량(창 4배, 아레나 11배)도 거부(publish_refused=8)도 아니다.
#
# 이 빌드에는 seq span 이 들어 있으므로 샘플은 슬롯이 1개가 되어도 seq 를 2개 쓴다. 그래서 두 양이
# 처음으로 분리된다:
#     대기가 datagram/도착 당이면  0.92 + 3.17       = 4.09 us -> 약 244,000 샘플/s (5,473 Mbps)
#     대기가 seq_no 당이면         0.92 + 2 x 3.17   = 7.26 us -> 약 137,700 샘플/s (3,085 Mbps)
# 두 예측이 1.8배 다르므로 한 번의 측정으로 갈린다.
#
# 실행 전 판정:
#   슬롯당 1.0 근처 + 약 244k   -> datagram 당. 조각 3 은 2배짜리이고 런타임 천장을 올릴 근거가 선다.
#   슬롯당 1.0 근처 + 약 138k   -> seq_no 당. 조각 3 은 12% 짜리다. 천장은 그대로 두고 우선순위를 내린다.
#   둘 다 아님                  -> 세 번째 메커니즘이다. 숫자를 먼저 보고하고 아무것도 짓지 않는다.
#   VOID 조건 (코드에서 검사):
#     - 4096 조건의 datagrams/sample 이 1.3 을 넘으면 통짜 경로가 발동하지 않은 것 -> 처리량을 읽지 않는다
#     - tx_dropped_oversize 가 0 이 아니면 재전송 stall 이 남아 있는 것 -> 어젯밤과 같은 설정이다
#     - shm_full_dropped 가 0 이 아니거나 drained 가 acked 가 아니면 셀이 회복된 것이 아니다
#   대조군: 같은 실행에서 슬롯 1472 를 함께 돌려 오늘의 121,767 을 재현해야 한다. 재현 못 하면 리그가
#     다른 상태이고 4096 조건도 비교 대상이 없다.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-$(cd "$REPO" && git rev-parse --short origin/main)}
OUT=${OUT:-$HOME/rig_results_safe/one_slot_decides.txt}
: >"$OUT"
echo "=== 한 슬롯이 가른다 $(date -Is) sha=$SHA ===" | tee -a "$OUT"
for S in 1472 4096; do
    echo "--- tt_SEGMENT_SLOT_BYTES=$S ---" | tee -a "$OUT"
    FRAMEWORKS=tickle REPS=3 DUR=5 SCEN=reliable_throughput SIZE=p4 SHA="$SHA" \
        BUILD_FLAGS="-Dtt_SEGMENT_SLOT_BYTES=$S" OUT=/tmp/oneslot_$S.txt timeout 900 \
        "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
    grep -hE 'arm=ON .*framework=tickle' "/tmp/oneslot_$S.txt.tickle" 2>/dev/null | sed "s/^/S=$S /" >>"$OUT"
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"
python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re,sys,statistics as st
rows={}
thin={}   # 필터에 걸린 행: 버리지 말고 말한다. 빈약한 실행을
         # 막으려 넣은 필터가 붕괴를 숨겼다 - sent=167 로 무너진 조건이 '행이 없음'으로 보고됐다.
for line in open(sys.argv[1]):
    m=re.match(r'S=(\d+) ',line)
    if not m or 'framework=tickle' not in line: continue
    f=dict(kv.split('=',1) for kv in line.split() if '=' in kv)
    try:
        s=int(f['sent'])
        if s<1000:
            thin.setdefault(int(m.group(1)),[]).append(s); continue
        rows.setdefault(int(m.group(1)),[]).append((float(f['send_mbps']),int(f.get('tx_shm',0)),s,
            int(f.get('shm_full_dropped',0)),int(f.get('tx_dropped_oversize',0)),f.get('drained','?')))
    except (KeyError,ValueError): pass
print()
print("  슬롯   샘플/s    슬롯/샘플  링드롭  oversize  drained")
res={}
for k in sorted(rows):
    rs=rows[k]
    if len(rs)<2: print(f"  {k:<6} (유효 반복 부족)"); continue
    rate=st.median(r[0] for r in rs)*1e6/(2800*8); dps=st.median(r[1]/r[2] for r in rs if r[2])
    res[k]=(rate,dps,st.median(r[3] for r in rs),st.median(r[4] for r in rs),rs[0][5])
    print(f"  {k:<6} {rate:>8,.0f} {dps:>10.2f} {res[k][2]:>7,.0f} {res[k][3]:>9,.0f}  {res[k][4]}")
print()
for k,v in sorted(thin.items()):
    print(f"  주의: 조건 {k} 의 {len(v)}개 행이 sent<1000 으로 걸렸다 (sent={v}). 행이 없는 것이 아니라")
    print("    셀이 무너진 것이다 - 처리량이 아니라 그 사실이 이 조건의 결과다.")
if 1472 not in res or 4096 not in res: print("  VOID: 조건이 둘 다 필요하다."); raise SystemExit
if abs(res[1472][0]-121767)/121767 > 0.1:
    print(f"  VOID: 대조군이 오늘의 121,767 을 재현하지 못했다 ({res[1472][0]:,.0f}). 리그 상태가 다르다."); raise SystemExit
if res[4096][1] > 1.3:
    print(f"  VOID: 4096 에서 슬롯/샘플 {res[4096][1]:.2f} — 통짜 경로가 발동하지 않았다."); raise SystemExit
if res[4096][3] != 0:
    print(f"  VOID: tx_dropped_oversize={res[4096][3]:,.0f} — 재전송 stall 이 남아 있다."); raise SystemExit
if res[4096][2] != 0 or res[4096][4] != 'acked':
    print(f"  VOID: 링드롭 {res[4096][2]:,.0f}, drained={res[4096][4]} — 셀이 회복된 것이 아니다."); raise SystemExit
r=res[4096][0]
print(f"  한 슬롯: {r:,.0f} 샘플/s = {r*2800*8/1e6:,.0f} Mbps  (2 슬롯 대비 {r/res[1472][0]:.2f}배)")
if abs(r-244000)/244000 < 0.15:
    print("  대기는 DATAGRAM 당이다. 조각 3 은 2배짜리이고 런타임 천장을 올릴 측정 근거가 섰다.")
elif abs(r-137700)/137700 < 0.15:
    print("  대기는 SEQ_NO 당이다. 조각 3 은 12% 짜리다 - 천장은 그대로 두고 우선순위를 내린다.")
else:
    print("  두 예측 어느 쪽도 아니다. 세 번째 메커니즘이고, 숫자만 보고하고 아무것도 짓지 않는다.")
PYEOF

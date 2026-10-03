#!/usr/bin/env bash
# sent=128 은 링 슬롯 수인가, 신뢰성 히스토리 x span 인가 — 두 후보가 같은 숫자를 예측한다.
#
# one_slot_decides.sh (2026-10-03, sha=2d2130d7) 에서 4096 조건이 세 번 모두 똑같이 무너졌다:
# sent=128, write_fail=49, tx_shm=15411, retransmitted=0, drained=timeout. 세 번이 자리까지 같으므로
# 경쟁 상태나 부하가 아니라 한계값이다. 그런데 128 을 예측하는 것이 둘이고, 오늘 데이터로는 갈리지 않는다:
#
#   (A) 링의 슬롯 수.  tt_SEGMENT_BYTES=768KiB 고정 예산이므로 슬롯이 커지면 레코드 수가 줄어든다.
#       슬롯 4096 -> stride 4112 -> 191 -> 2의 거듭제곱으로 내려 128. 발행자가 슬롯 하나에 샘플 하나씩
#       넣고 멈춘 모양과 정확히 일치한다.
#   (B) tt_MAX_RELIABLE_HISTORY(64) x seq span(2) = 128. seq span 이 들어오면서 샘플당 seq 가 2개가
#       되었으므로, 샘플이 아니라 seq 로 세는 창이 있으면 64 샘플에서 멈춘다 — 역시 128 seq 다.
#
# 두 후보를 갈라놓는 조건: 슬롯 8192. stride 8208 -> raw 95 -> 슬롯 64. 2800바이트 레코드는 여전히
# 통째로 들어간다. (A) 면 수가 따라 내려가고, (B) 면 링과 무관하므로 그대로다.
#
# 실행 전 판정 (코드로 검사한다, 주석이 아니라):
#   8192 에서 sent 가 64 근처   -> (A) 링 슬롯 수다. 슬롯을 키우면 레코드 용량이 줄어드는 것이 원인이고,
#                                   고칠 자리는 세그먼트 예산이지 신뢰성 회계가 아니다.
#   8192 에서 sent 가 128 근처  -> (B) 링이 아니다. seq 로 세는 창을 찾을 차례이고 tt_MAX_RELIABLE_HISTORY
#                                   가 첫 후보다.
#   둘 다 아님                  -> 세 번째 것이다. 숫자만 보고하고 아무것도 짓지 않는다.
#
#   대조군: 같은 실행의 4096 조건이 sent=128 을 재현해야 한다. 재현 못 하면 리그가 다른 상태이고
#     8192 조건도 비교 대상이 없다 -> VOID.
#   VOID 조건: drained 가 timeout 이 아니면 셀이 무너지지 않은 것이므로 이 실험의 전제가 사라진다.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-$(cd "$REPO" && git rev-parse --short HEAD)}
OUT=${OUT:-$HOME/rig_results_safe/what_bounds_128.txt}
: >"$OUT"
echo "=== 128 을 묶는 것은 무엇인가 $(date -Is) sha=$SHA ===" | tee -a "$OUT"
for S in 4096 8192; do
    echo "--- tt_SEGMENT_SLOT_BYTES=$S ---" | tee -a "$OUT"
    FRAMEWORKS=tickle REPS=3 DUR=5 SCEN=reliable_throughput SIZE=p4 SHA="$SHA" \
        BUILD_FLAGS="-Dtt_SEGMENT_SLOT_BYTES=$S" OUT=/tmp/bounds128_$S.txt timeout 900 \
        "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
    grep -hE 'arm=ON .*framework=tickle' "/tmp/bounds128_$S.txt.tickle" 2>/dev/null | sed "s/^/S=$S /" >>"$OUT"
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
rows = {}
for line in open(sys.argv[1]):
    m = re.match(r'S=(\d+) ', line)
    if not m or 'framework=tickle' not in line:
        continue
    f = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    if f.get('role') != 'client':
        continue
    rows.setdefault(int(m.group(1)), []).append(f)

def col(rs, k):
    out = []
    for f in rs:
        try: out.append(float(f.get(k, 'nan')))
        except ValueError: pass
    return out

print()
print("  슬롯   예상 링슬롯   sent(중앙)   write_fail   drained          reps")
pred = {4096: 128, 8192: 64}
seen = {}
for S in sorted(rows):
    rs = rows[S]
    sent = col(rs, 'sent')
    if not sent:
        print(f"  {S:5}   {pred.get(S,'?'):>9}   (RESULT 행 없음)"); continue
    med = st.median(sent)
    seen[S] = med
    dr = ",".join(sorted({f.get('drained', '?') for f in rs}))
    wf = ",".join(sorted({f.get('write_fail', '?') for f in rs}))
    print(f"  {S:5}   {pred.get(S,'?'):>9}   {med:10.0f}   {wf:>10}   {dr:<14}  {len(rs)}  sent={[int(x) for x in sent]}")

print()
if 4096 not in seen or 8192 not in seen:
    print("VOID: 두 조건 모두 필요하다 - 한쪽에 사용 가능한 행이 없다.")
    sys.exit(0)
if not (100 <= seen[4096] <= 160):
    print(f"VOID: 대조군 4096 이 {seen[4096]:.0f} 으로 128 을 재현하지 못했다. 리그가 다른 상태이므로")
    print("  8192 조건도 비교 대상이 없다. 아무것도 결론짓지 않는다.")
    sys.exit(0)
print(f"대조군 4096 = {seen[4096]:.0f} (128 재현). 8192 = {seen[8192]:.0f}.")
if 48 <= seen[8192] <= 80:
    print("판정 (A): 링의 슬롯 수다. 슬롯을 키우면 768KiB 예산 안의 레코드 수가 줄고, 발행자는 슬롯")
    print("  하나에 샘플 하나를 넣고 멈춘다. 고칠 자리는 세그먼트 예산이지 신뢰성 회계가 아니다.")
elif 100 <= seen[8192] <= 160:
    print("판정 (B): 링이 아니다 - 슬롯 수를 반으로 줄였는데 한계가 따라오지 않았다. seq 로 세는 창을")
    print("  찾을 차례이고 tt_MAX_RELIABLE_HISTORY(64) x span(2) 가 첫 후보다.")
else:
    print(f"판정: 세 번째 것이다. 8192 에서 {seen[8192]:.0f} 은 두 예측(64, 128) 어느 쪽도 아니다.")
    print("  숫자를 보고하고 아무것도 짓지 않는다.")
PYEOF

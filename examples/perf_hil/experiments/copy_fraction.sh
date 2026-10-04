#!/usr/bin/env bash
# 세그먼트의 샘플당 비용 중 '바이트에 비례하는 부분'(복사)이 몇 %인가.
#
# 왜 이 질문인가 (COMPARISON 2026-10-04): 같은 호스트 p4 BEST_EFFORT 에서 각 프레임워크가 자기 커널
# 경로 대비 얻는 이득이 TickLE 6.79x, Fast DDS 11.6x 다. 우리 커널 경로가 더 좋은데(1.87x) 공유메모리는
# 6.6% 만 앞선다 - 즉 same-host 우위의 대부분을 세그먼트가 번 것이 아니라 네트워크 경로에서 물려받았다.
# Dev 의 후보: data-sharing 은 대여(loan)라 복사 0 회, 우리 링은 복사 2 회(쓰기 때 슬롯으로, 읽기 때
# rx_buffer 로). 그 둘이 샘플당 예산의 큰 몫이면 6e(b)(슬롯에 직접 인코딩)가 속도를 움직이고, 작은 몫이면
# 6e(b) 는 메모리만 아끼고 속도는 안 움직인다 - 어젯밤 0.99x 가 이미 그쪽을 가리켰다.
#
# 측정 방법: 샘플 크기만 바꾸고 코드 경로는 고정한다. p1(76) p2(1292) p3(1424) 는 **전부 데이터그램 1개**다
# (1424 + 24 프레이밍 = 1448 <= 1472). p4(2800) 는 2 개이므로 **제외한다** - 크기와 함께 데이터그램 수가
# 바뀌면 복사 비용과 seq_no 당 비용이 섞이고, 후자가 지배한다는 것은 이미 측정됐다(2026-10-04, 0.99x).
#
# 샘플당 시간 t(n) = a + b*n 으로 맞추고, b*n 이 t(n) 에서 차지하는 비율을 본다.
#
# 실행 전 판정 (코드로 검사한다):
#   ON arm 의 복사 비율(p3, 1424B 기준)
#     >= 25%  -> 바이트 비례 비용이 큰 항이다. 6e(b) 가 두 복사 중 하나를 없애므로 그 절반쯤을 회수할
#                것으로 기대할 수 있고, 메모리뿐 아니라 속도를 위해 할 가치가 있다.
#     <= 10%  -> 복사는 격차의 원인이 아니다. 6e(b) 는 메모리를 아끼고 속도는 안 움직인다(0.99x 와 일치).
#                고정 비용 항을 봐야 한다 - 원자 연산, rx_buffer 경유, 프레이밍 조립, 폴링.
#     사이    -> 숫자만 보고하고 아무것도 짓지 않는다.
#
#   대조군 (이것이 결과를 결정적으로 만든다): OFF arm(세그먼트 컴파일 제외)을 같은 방식으로 맞춘다.
#     b_OFF 는 커널 경로 자신의 바이트당 비용이다. b_ON ~= b_OFF 이면 세그먼트는 커널이 이미 치르던 것
#     위에 바이트당 비용을 **추가하지 않는 것**이고, 복사 2 회 가설은 비율의 절대값과 무관하게 반박된다.
#     세그먼트를 컴파일에서 빼는 것은 이 가설이 손댈 수 없는 arm 이므로, 같은 기울기가 나오면 그것이
#     가설에 대한 반증이지 잡음이 아니다.
#
#   VOID 조건:
#     - 어느 arm 이든 shm_full_dropped != 0  -> 버리면서 빨라지는 경로를 '빠르다'로 읽을 수 없다
#       (2026-10-03/04 에 drop 비율과 보고 속도의 상관이 +0.925, 세 번 재현)
#     - 세 크기의 datagrams/sample 이 같지 않으면 -> 코드 경로가 바뀐 것이고 이 실험의 전제가 사라진다
#     - 선형 적합의 R^2 < 0.9 -> 모델이 틀린 것이고 기울기는 아무 뜻이 없다
#     - 어느 크기든 drop-free 반복이 3 회 미만
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-$(cd "$REPO" && git rev-parse --short HEAD)}
REPS=${REPS:-5}
DUR=${DUR:-5}
OUT=${OUT:-$HOME/rig_results_safe/copy_fraction.txt}
: >"$OUT"
echo "=== 복사가 샘플당 예산의 몇 %인가 $(date -Is) sha=$SHA reps=$REPS ===" | tee -a "$OUT"
echo "    크기 p1/p2/p3 만: 전부 데이터그램 1개. p4 는 2개라 제외(복사와 seq_no 비용이 섞인다)." | tee -a "$OUT"
for SZ in p1 p2 p3; do
    echo "--- SIZE=$SZ ---" | tee -a "$OUT"
    FRAMEWORKS=tickle REPS="$REPS" DUR="$DUR" SCEN=best_effort_throughput SIZE="$SZ" SHA="$SHA" \
        OUT=/tmp/copyfrac_$SZ.txt timeout 1800 \
        "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
    grep -hE 'arm=(ON|OFF) RESULT.*framework=tickle.*role=client' "/tmp/copyfrac_$SZ.txt.tickle" 2>/dev/null \
        | sed "s/^/SZ=$SZ /" >>"$OUT"
done
echo "=== done $(date -Is) ===" | tee -a "$OUT"

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
rows = {}
drops = {}
dgram = {}
for line in open(sys.argv[1]):
    m = re.match(r'SZ=(p\d)\s+arm=(\w+) RESULT:', line)
    if not m:
        continue
    f = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    key = (m.group(1), m.group(2))
    try:
        sent = int(f['sent']); sb = int(f['sample_bytes']); el = float(f['elapsed_s'])
    except (KeyError, ValueError):
        continue
    d = int(f.get('shm_full_dropped', 0))
    if d:
        drops.setdefault(key, []).append(d)
        continue
    if sent < 1000:
        drops.setdefault(key, []).append(-sent)   # 빈약한 행도 말한다, 조용히 버리지 않는다
        continue
    rows.setdefault(key, []).append((sb, el / sent * 1e6))   # 샘플당 us
    # 코드 경로가 같은지: 데이터그램/샘플
    tx = int(f.get('tx_shm', 0)) + int(f.get('tx_udp', 0))
    dgram.setdefault(key, []).append(tx / sent)

def fit(pts):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    n = len(xs); mx = st.mean(xs); my = st.mean(ys)
    sxx = sum((x-mx)**2 for x in xs)
    if sxx == 0: return None
    b = sum((x-mx)*(y-my) for x, y in zip(xs, ys)) / sxx
    a = my - b*mx
    ss_res = sum((y-(a+b*x))**2 for x, y in zip(xs, ys))
    ss_tot = sum((y-my)**2 for y in ys)
    r2 = 1 - ss_res/ss_tot if ss_tot > 0 else 0.0
    return a, b, r2

print()
void = []
for arm in ("ON", "OFF"):
    pts = []
    print(f"=== arm {arm} ===")
    for sz in ("p1", "p2", "p3"):
        k = (sz, arm)
        if k in drops:
            print(f"  {sz}: 제외된 행 {drops[k]}  (양수=링 드롭, 음수=sent 가 그만큼뿐)")
        v = rows.get(k, [])
        if len(v) < 3:
            print(f"  {sz}: drop-free 반복 {len(v)} 회 - 3 회 미만")
            void.append(f"{arm}/{sz} 반복 부족")
            continue
        us = [p[1] for p in v]
        dg = st.mean(dgram[k])
        print(f"  {sz}: {v[0][0]:5} B  샘플당 {st.mean(us):7.3f} us  (n={len(us)}, {min(us):.3f}..{max(us):.3f})  데이터그램/샘플 {dg:.2f}")
        pts += v
    if len(pts) < 9:
        continue
    dgs = [st.mean(dgram[(sz, arm)]) for sz in ("p1","p2","p3") if (sz,arm) in dgram]
    if dgs and (max(dgs) - min(dgs)) > 0.1:
        print(f"  VOID {arm}: 데이터그램/샘플이 크기마다 다르다 {['%.2f'%d for d in dgs]} - 코드 경로가 같지 않다.")
        void.append(f"{arm} 경로 불일치"); continue
    r = fit(pts)
    if not r:
        void.append(f"{arm} 적합 불가"); continue
    a, b, r2 = r
    print(f"  적합 t(n) = {a:.3f} + {b*1000:.4f} ns/B * n     R^2={r2:.3f}")
    if r2 < 0.9:
        print(f"  VOID {arm}: R^2 {r2:.3f} < 0.9 - 선형이 아니므로 기울기에 뜻이 없다.")
        void.append(f"{arm} 비선형"); continue
    t3 = a + b*1424
    print(f"  p3(1424 B) 에서: 샘플당 {t3:.3f} us 중 바이트 비례분 {b*1424:.3f} us = {100*b*1424/t3:.1f}%")
    globals()[f"frac_{arm}"] = 100*b*1424/t3
    globals()[f"b_{arm}"] = b

print()
if void:
    print("VOID: " + "; ".join(void))
    print("  판정하지 않는다. 위 사유가 이 실행의 결과다.")
    sys.exit(0)
fon = globals().get("frac_ON"); bon = globals().get("b_ON"); boff = globals().get("b_OFF")
print(f"대조군: 바이트당 비용 ON {bon*1000:.4f} ns/B,  OFF {boff*1000:.4f} ns/B,  차이 {(bon-boff)*1000:+.4f}")
if boff and abs(bon-boff)/max(abs(boff),1e-12) < 0.25:
    print("판정: 세그먼트는 커널이 이미 치르던 것 위에 바이트당 비용을 더하지 않는다 (두 기울기가 25% 안).")
    print("  복사 2 회 가설은 반박된다. 6e(b) 는 메모리 논거로 남고 속도 논거는 서지 않는다.")
elif fon >= 25:
    print(f"판정: 바이트 비례 비용이 큰 항이다 ({fon:.1f}%). 6e(b) 가 두 복사 중 하나를 없애므로")
    print("  그 절반쯤 회수를 기대할 수 있고, 속도를 위해 할 가치가 있다.")
elif fon <= 10:
    print(f"판정: 복사는 격차의 원인이 아니다 ({fon:.1f}%). 6e(b) 는 메모리를 아끼고 속도는 안 움직인다.")
    print("  고정 비용 항으로 간다 - 원자 연산, rx_buffer 경유, 프레이밍 조립, 폴링.")
else:
    print(f"판정: {fon:.1f}% 는 두 구간 사이다. 숫자를 보고하고 아무것도 짓지 않는다.")
PYEOF

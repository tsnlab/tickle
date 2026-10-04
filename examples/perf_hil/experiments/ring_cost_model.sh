#!/usr/bin/env bash
# ring_cost_model.c 를 리그에서 네 arm x N 반복으로 돌리고, 실제 측정된 샘플당 예산에 대고 읽는다.
#
# 기준이 되는 실측값: 2026-10-04, p3(1424 B) BEST_EFFORT, 세그먼트 ON arm = **1.507 us/sample**
# (copy_fraction_20261004.txt, n=3, 1.504..1.511). 이 벤치는 링 부분만 모형화하므로 그보다 **작아야**
# 한다 - 크면 부분집합을 모형화한 것이 아니므로 아무것도 읽지 않는다.
#
# 실행 전 판정 (코드로 검사한다):
#   복사분  = (A1 - A3) / 1.507      캐시라인분 = (A1 - A2) / 1.507
#     복사분 >= 25%  -> 복사가 큰 항. 6e(b) 가 두 복사 중 하나를 없애므로 속도 논거가 선다.
#     복사분 <= 10%  -> 복사는 원인이 아니다. 6e(b) 는 메모리 논거로만 남는다.
#     캐시라인분 >= 10% -> 레이아웃을 고칠 가치가 있다 (읽기 전용 기하 정보를 뜨거운 쓰기 라인에서 분리).
#     캐시라인분 <= 3%  -> 레이아웃은 원인이 아니다. Dev 의 캐시라인 가설은 반박된다.
#     그 사이        -> 숫자만 보고하고 아무것도 짓지 않는다.
#
#   VOID 조건:
#     - A1 >= 1.507 us  -> 이 모형이 실제 경로의 부분집합이 아니다. 비교 대상이 될 수 없다.
#     - **판정이 쓰는 arm(A1, A2, A3)** 의 반복 간 폭이 그 arm 평균의 10% 를 넘으면 -> 분해할 만큼
#       안정적이지 않다. A4 는 상호작용 보고에만 쓰이므로 폭을 적되 판정을 막지 않는다.
#       (2026-10-04 첫 실행에서 A1/A2/A3 가 0.1~0.4% 로 단단한데 A4 가 18.1% 라 VOID 가 났다. 판정이
#        쓰지 않는 arm 이 판정을 막는 것은 기준의 범위 오류다. 다만 그 실행을 느슨한 기준으로 다시 읽지
#        않고 - 그것이 결과를 보고 기준을 바꾸는 것이므로 - 범위를 고친 뒤 새로 돌린다.)
#     - A1 이 A2/A3/A4 중 어느 것보다 작으면 -> 더 적은 일을 하는 arm 이 더 느리다는 뜻이므로 계측 오류.
#
#   상호작용도 함께 본다 (판정이 아니라 보고): (A1-A2)+(A1-A3) 와 (A1-A4) 가 다르면 두 효과가
#   독립이 아니다. 로컬 PC 에서 이미 그렇게 나왔으므로(초가산) 리그에서도 확인한다.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519
HOST=${HOST:-10.1.1.214}
REPS=${REPS:-5}
N=${N:-3000000}
BUDGET=${BUDGET:-1.507}
OUT=${OUT:-$HOME/rig_results_safe/ring_cost_model.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== 링 비용 분해 $(date -Is) host=$HOST reps=$REPS N=$N 기준예산=${BUDGET}us/sample ==="
sh_ "mkdir -p /tmp/rcm" </dev/null
sh_ "cat > /tmp/rcm/ring_cost_model.c" <"$REPO/examples/perf_hil/experiments/ring_cost_model.c" || { say "FATAL 복사 실패"; exit 1; }
for spec in "a1:" "a2:-DPAD_INDICES" "a3:-DNO_COPY" "a4:-DPAD_INDICES -DNO_COPY"; do
    n=${spec%%:*}; f=${spec#*:}
    sh_ "cd /tmp/rcm && gcc -O2 -pthread $f -o $n ring_cost_model.c 2>&1" </dev/null | head -3 | sed 's/^/  build /' | tee -a "$OUT"
done
say "--- 실행 ---"
for _rep in $(seq 1 "$REPS"); do
    for n in a1 a2 a3 a4; do
        sh_ "cd /tmp/rcm && taskset -c 0-3 ./$n $N" </dev/null | tee -a "$OUT"
    done
done
say "=== done $(date -Is) ==="

python3 - "$OUT" "$BUDGET" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
budget = float(sys.argv[2])
arms = {}
for line in open(sys.argv[1]):
    m = re.search(r'arm=(\w+).*us_per_record=([0-9.]+)', line)
    if m: arms.setdefault(m.group(1).split('_')[0], []).append(float(m.group(2)))
print()
void = []
mean = {}
for k in ("A1", "A2", "A3", "A4"):
    v = arms.get(k, [])
    if len(v) < 3:
        print(f"  {k}: 반복 {len(v)} 회 - 3 회 미만"); void.append(f"{k} 반복 부족"); continue
    m = st.mean(v); spread = (max(v) - min(v)) / m * 100
    mean[k] = m
    gates = k in ("A1", "A2", "A3")   # A4 는 상호작용 보고 전용 - 판정을 막지 않는다
    tag = "" if gates else "  (판정에 쓰지 않음)"
    print(f"  {k}: {m:.4f} us/record  (n={len(v)}, {min(v):.4f}..{max(v):.4f}, 폭 {spread:.1f}%){tag}")
    if spread > 10 and gates: void.append(f"{k} 폭 {spread:.1f}% > 10%")
print()
if len(mean) == 4:
    if mean["A1"] >= budget: void.append(f"A1 {mean['A1']:.4f} >= 기준예산 {budget} - 부분집합이 아니다")
    for k in ("A2", "A3", "A4"):
        if mean["A1"] < mean[k]: void.append(f"A1 < {k} - 일을 덜 하는 arm 이 더 느리다, 계측 오류")
if void:
    print("VOID: " + "; ".join(void)); print("  판정하지 않는다. 위 사유가 이 실행의 결과다."); sys.exit(0)
copy_abs = mean["A1"] - mean["A3"]; line_abs = mean["A1"] - mean["A2"]; both_abs = mean["A1"] - mean["A4"]
copy_f = 100 * copy_abs / budget; line_f = 100 * line_abs / budget
print(f"  복사분      A1-A3 = {copy_abs:.4f} us  -> 실측 예산 {budget} us 의 {copy_f:.1f}%")
print(f"  캐시라인분  A1-A2 = {line_abs:.4f} us  -> {line_f:.1f}%")
print(f"  둘 다       A1-A4 = {both_abs:.4f} us   (단순 합 {copy_abs+line_abs:.4f} - {'초가산' if both_abs > copy_abs+line_abs else '열가산' if both_abs < copy_abs+line_abs else '가산'})")
print()
if copy_f >= 25: print(f"복사: 큰 항이다 ({copy_f:.1f}%). 6e(b) 가 두 복사 중 하나를 없애므로 속도 논거가 선다.")
elif copy_f <= 10: print(f"복사: 원인이 아니다 ({copy_f:.1f}%). 6e(b) 는 메모리 논거로만 남는다.")
else: print(f"복사: {copy_f:.1f}% 는 두 구간 사이다. 보고만 하고 짓지 않는다.")
if line_f >= 10: print(f"캐시라인: 고칠 가치가 있다 ({line_f:.1f}%). 읽기 전용 기하 정보를 뜨거운 쓰기 라인에서 분리.")
elif line_f <= 3: print(f"캐시라인: 원인이 아니다 ({line_f:.1f}%). Dev 의 가설은 이 측정으로 반박된다.")
else: print(f"캐시라인: {line_f:.1f}% 는 두 구간 사이다. 보고만 하고 짓지 않는다.")
PYEOF

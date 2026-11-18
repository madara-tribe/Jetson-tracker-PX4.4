#!/usr/bin/env bash
# ============================================================
#  Step 2+3: GPU（CUDA / TensorRT FP32 / TensorRT FP16）で計測し、CPU と比べる
#  px-jetson:gpu イメージのコンテナ内 /ros2_ws/work で実行
#
#    TAG=gpu_day1 BASELINE=cpu_base ./bench/2_3_gpu_compare.sh
#
#  オフライン: cpu / cuda / trt_fp32 / trt_fp16 を同じ画像で計測
#  実機     : trt_fp16（LIVE_FP32=1 なら trt_fp32 も）
#  集計     : BASELINE（Step 1 の TAG）と比べた summary.md
#
#  ※ TensorRT は初回だけエンジン作成に数分かかる（trt_cache/ に保存され、2回目から速い）
# ============================================================
set -euo pipefail
source "$(dirname "$0")/_common.sh"
save_env gpu

# GPU が使えるか先に確認（使えないと黙って CPU に落ちるため）
CHECK="$("$BENCH" --model "$MODEL" --provider tensorrt --cache "$WORK/trt_cache/fp16" --iters 1 --warmup 0 --image "$IMAGE" 2>&1 || true)"
if ! grep -q "using provider: tensorrt" <<< "$CHECK"; then
  echo "ERROR: TensorRT が使えません。px-jetson:gpu イメージか、--runtime nvidia で起動したか確認"
  grep "\[yolo\]" <<< "$CHECK" || echo "$CHECK" | tail -20
  exit 1
fi

run_offline cpu      cpu      0
run_offline cuda     cuda     0
run_offline trt_fp32 tensorrt 0
run_offline trt_fp16 tensorrt 1

run_live trt_fp16 tensorrt true
if [[ "${LIVE_FP32:-0}" == "1" ]]; then run_live trt_fp32 tensorrt false; fi

ARGS=("$OUT")
if [[ -n "${BASELINE:-}" ]]; then ARGS+=(--baseline "$WORK/bench/results/$BASELINE"); fi
python3 "$WORK/bench/summarize.py" "${ARGS[@]}"
echo; echo "結果: $OUT/summary.md"

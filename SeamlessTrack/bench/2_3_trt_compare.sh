#!/usr/bin/env bash
# ============================================================
#  Step 2+3（TensorRT 直接版）: TensorRT FP32 / FP16 で計測し、CPU と比べる
#  CPU 版イメージ px-jetson:latest のままで動く（TensorRT は JetPack に同梱）
#  コンテナ内 /ros2_ws/work で実行:
#
#    TAG=trt_day1 BASELINE=cpu_base ./bench/2_3_trt_compare.sh
#
#  オフライン: cpu / trt_fp32 / trt_fp16 を同じ画像で計測
#  実機     : trt_fp16（LIVE_FP32=1 なら trt_fp32 も）
#  集計     : BASELINE（Step 1 の TAG）と比べた summary.md
#  ※ engines/yolo11_fp16.engine, yolo11_fp32.engine（make_engines.sh で作成）があれば自動で使う
#    無ければ px2 が作る（初回は数分。trt_cache/fp16, fp32 に保存）
# ============================================================
set -euo pipefail
source "$(dirname "$0")/_common.sh"
save_env trt

# GPU（TensorRT）が使えるか先に確認。使えないと黙って CPU に落ちるため
echo "TensorRT の確認（初回はエンジン作成で数分かかります）..."
CHECK="$("$BENCH" --model "$MODEL" --provider trt --fp16 1 --cache "$WORK/trt_cache/fp16" --engine "$TRT_ENGINE_FP16" \
          --iters 1 --warmup 0 --image "$IMAGE" 2>&1 || true)"
if ! grep -q "using provider: trt" <<< "$CHECK"; then
  echo "ERROR: TensorRT が使えません。以下を確認してください:"
  echo "  - コンテナを --runtime nvidia で起動したか（docker info | grep -i runtime に nvidia）"
  echo "  - ./build_px.sh のログに 'px2: TensorRT enabled' が出ているか"
  echo "$CHECK" | grep -E "\[trt\]|\[yolo\]" | tail -20
  exit 1
fi

run_offline cpu      cpu 0
run_offline trt_fp32 trt 0
run_offline trt_fp16 trt 1

run_live trt_fp16 trt true
if [[ "${LIVE_FP32:-0}" == "1" ]]; then run_live trt_fp32 trt false; fi

ARGS=("$OUT")
if [[ -n "${BASELINE:-}" ]]; then ARGS+=(--baseline "$WORK/bench/results/$BASELINE"); fi
python3 "$WORK/bench/summarize.py" "${ARGS[@]}"
echo; echo "結果: $OUT/summary.md"

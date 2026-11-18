# 共通設定（各スクリプトから source する）。コンテナ内 /ros2_ws/work で使う前提
WORK="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$WORK"
set +u; source /opt/ros/humble/setup.bash; source install/setup.bash; set -u

TAG="${TAG:?TAG を指定してください（例: TAG=day1 ./bench/1_baseline_cpu.sh）}"
OUT="$WORK/bench/results/$TAG"
ITERS="${ITERS:-300}"          # オフライン計測の回数
LIVE_SEC="${LIVE_SEC:-120}"    # 実機計測の秒数（0 で省略）
MODEL="${MODEL:-$WORK/src/px2/weights/yolo11.onnx}"
BENCH="$WORK/install/px2/lib/px2/bench_yolo"
# 入力画像: 指定がなければ保存済みの検出フレームを 1 枚使う（無ければ合成画像）
IMAGE="${IMAGE:-$(find "$WORK" -path "$WORK/bench" -prune -o -name 'detect_*.jpg' -print 2>/dev/null | head -1 || true)}"

# TensorRT エンジン: make_engines.sh で作ったもの（engines/）があれば優先して使う
#   px2 に作らせるとメモリ不足で遅いエンジンになることがあるため
[[ -z "${TRT_ENGINE_FP16:-}" && -s "$WORK/engines/yolo11_fp16.engine" ]] && TRT_ENGINE_FP16="$WORK/engines/yolo11_fp16.engine"
[[ -z "${TRT_ENGINE_FP32:-}" && -s "$WORK/engines/yolo11_fp32.engine" ]] && TRT_ENGINE_FP32="$WORK/engines/yolo11_fp32.engine"
TRT_ENGINE_FP16="${TRT_ENGINE_FP16:-}"; TRT_ENGINE_FP32="${TRT_ENGINE_FP32:-}"

mkdir -p "$OUT"
[[ -s "$MODEL" ]] || { echo "ERROR: model not found or empty: $MODEL"; exit 1; }
[[ -x "$BENCH" ]] || { echo "ERROR: $BENCH が無い。先に ./build_px.sh"; exit 1; }

save_env() {
  {
    echo "date: $(date -Iseconds)"
    echo "tag: $TAG"
    echo "model: $MODEL ($(stat -c %s "$MODEL") bytes, md5 $(md5sum "$MODEL" | cut -d' ' -f1))"
    echo "image: ${IMAGE:-<synthetic 1920x1080>}"
    echo "l4t: $(head -1 /etc/nv_tegra_release 2>/dev/null)"
    echo "ort: $(ls /usr/local/lib/libonnxruntime.so.* 2>/dev/null | xargs -r -n1 basename | tr '\n' ' ')"
    echo "nproc: $(nproc)"
    echo "trt_engine_fp16: ${TRT_ENGINE_FP16:-<auto>}"
    echo "trt_engine_fp32: ${TRT_ENGINE_FP32:-<auto>}"
  } > "$OUT/env_$1.txt"
}

run_offline() {  # $1=label $2=provider $3=fp16(0/1)
  local fp="fp32"; [[ "$3" == "1" ]] && fp="fp16"
  local eng=""; [[ "$2" == "trt" ]] && { [[ "$fp" == "fp16" ]] && eng="$TRT_ENGINE_FP16" || eng="$TRT_ENGINE_FP32"; }
  echo; echo "########## offline: $1 ${eng:+(engine: $eng)} ##########"
  "$BENCH" --model "$MODEL" --provider "$2" --fp16 "$3" --engine "$eng" \
    --cache "$WORK/trt_cache/$fp" --iters "$ITERS" --warmup 20 \
    --image "$IMAGE" --label "$1" --csv "$OUT/offline_$1.csv" 2>&1 | tee "$OUT/offline_$1.txt"
}

run_live() {  # $1=label $2=provider $3=fp16(true/false)
  [[ "$LIVE_SEC" -gt 0 ]] || return 0
  echo; echo "########## live: $1 (${LIVE_SEC}s) ##########"
  echo ">>> カメラの前で人が動いてください（PIR が反応すると DETECT に入り、計測が始まります）"
  local eng=""; [[ "$2" == "trt" ]] && { [[ "$3" == "true" ]] && eng="$TRT_ENGINE_FP16" || eng="$TRT_ENGINE_FP32"; }
  PROVIDER="$2" TRT_FP16="$3" TRT_ENGINE="$eng" SAVE_FRAMES=false TIMING_CSV="$OUT/live_$1.csv" \
    timeout --signal=INT --kill-after=10 "$LIVE_SEC" ./launch_px.sh > "$OUT/live_$1.log" 2>&1 || true
  echo "live rows: $(($(wc -l < "$OUT/live_$1.csv" 2>/dev/null || echo 1) - 1))  log: $OUT/live_$1.log"
}

#!/usr/bin/env bash
# ============================================================
#  make_engines.sh — ONNX → TensorRT エンジン（trtexec）を作る（コンテナ内で実行）
#
#  launch_px.sh（PROVIDER=trt）は engines/yolo11_fp16.engine / fp32 を自動で使う。
#  px2 に作らせるより速いエンジンができる（作成中のメモリ不足に強い）。
#  事前に Jetson 本体で ./scripts/perf_mode.sh --free-mem を実行しておくとよい。
#  作成に FP32 約 3 分、FP16 約 7 分。速度の測定結果も engines/summary.txt に残す。
#
#    ./make_engines.sh                    # fp32 と fp16
#    PRECISIONS="fp16" ./make_engines.sh
#
#  出力: engines/yolo11_<prec>.engine, engines/trtexec_<prec>.log, engines/summary.txt
#  ※ エンジンは TensorRT のバージョンと GPU に固有。Jetson 上・同じイメージで作ること
# ============================================================
set -euo pipefail
WORK="$(cd "$(dirname "$0")" && pwd)"
MODEL="${MODEL:-$WORK/src/px2/weights/yolo11.onnx}"
OUTDIR="${OUTDIR:-$WORK/engines}"
PRECISIONS="${PRECISIONS:-fp32 fp16}"
TRTEXEC="${TRTEXEC:-$(command -v trtexec || echo /usr/src/tensorrt/bin/trtexec)}"

[[ -x "$TRTEXEC" ]] || { echo "ERROR: trtexec が見つからない ($TRTEXEC)"; exit 1; }
[[ -s "$MODEL" ]]   || { echo "ERROR: model not found or empty: $MODEL"; exit 1; }
mkdir -p "$OUTDIR"
: > "$OUTDIR/summary.txt"

for p in $PRECISIONS; do
  flag=""; [[ "$p" == "fp16" ]] && flag="--fp16"
  echo "########## $p ##########"
  "$TRTEXEC" --onnx="$MODEL" --saveEngine="$OUTDIR/yolo11_$p.engine" $flag \
    --memPoolSize=workspace:1024 --warmUp=500 --duration=10 --useSpinWait \
    2>&1 | tee "$OUTDIR/trtexec_$p.log"
  {
    echo "== $p =="
    grep -E "Throughput:|Latency: min|GPU Compute Time: min|H2D Latency: min|D2H Latency: min" \
      "$OUTDIR/trtexec_$p.log" | sed 's/^\[[^]]*\] *\[I\] *//'
    echo
  } >> "$OUTDIR/summary.txt"
done
echo; cat "$OUTDIR/summary.txt"
ls -lh "$OUTDIR"/*.engine

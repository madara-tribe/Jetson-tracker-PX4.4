#!/usr/bin/env bash
# ============================================================
#  launch_px.sh — PX を起動する（コンテナ内 /ros2_ws/work で実行）
#
#    PROVIDER=cpu ./launch_px.sh                  # CPU（ONNX Runtime）
#    PROVIDER=trt ./launch_px.sh                  # TensorRT FP16（既定）
#    PROVIDER=trt TRT_FP16=false ./launch_px.sh   # TensorRT FP32
#  止めるときは Ctrl+C（Ctrl+Z は使わない）
#
#  WATCH        : PIR の検知（/px_wakeup）を待つ。カメラの映像は読み捨て
#  DETECT_TRACK : YOLO11 推論 → PD 制御 → サーボ指令。検出画像は
#                 install/px2/share/px2/data/ に保存（SAVE_FRAMES=true のとき）
# ============================================================
set -e

PROVIDER="${PROVIDER:-trt}"          # trt | cpu
TRT_FP16="${TRT_FP16:-true}"          # TensorRT の精度: true=FP16 / false=FP32
TRT_ENGINE="${TRT_ENGINE:-}"          # 使う .engine（空なら engines/ を探し、無ければ px2 が作る）
SAVE_FRAMES="${SAVE_FRAMES:-true}"    # 計測時は false（JPEG 保存が時間を食うため）
TIMING_CSV="${TIMING_CSV:-}"          # 計測 CSV の出力先（空なら書かない）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# make_engines.sh で作ったエンジンがあれば自動で使う
if [[ "${PROVIDER}" == "trt" && -z "${TRT_ENGINE}" ]]; then
  PREC=$([[ "${TRT_FP16}" == "true" ]] && echo fp16 || echo fp32)
  if [[ -s "${SCRIPT_DIR}/engines/yolo11_${PREC}.engine" ]]; then
    TRT_ENGINE="${SCRIPT_DIR}/engines/yolo11_${PREC}.engine"
  else
    echo "INFO: engines/yolo11_${PREC}.engine がありません。px2 がエンジンを作ります（数分。./make_engines.sh 推奨）"
  fi
fi
[[ -n "${TRT_ENGINE}" ]] && echo "INFO: TensorRT engine = ${TRT_ENGINE}"

# ros2 launch は空の値（name:=）を受け付けないので、値があるときだけ渡す
EXTRA_ARGS=()
[[ -n "${TIMING_CSV}" ]] && EXTRA_ARGS+=("timing_csv:=${TIMING_CSV}")
[[ -n "${TRT_ENGINE}" ]] && EXTRA_ARGS+=("trt_engine:=${TRT_ENGINE}")

ros2 launch px3_launch px3_system_launch.py \
  camera_path:=/dev/video/pir_camera \
  serial_port:=/dev/ttyACM0 \
  baudrate:=115200 \
  x_center_deg:=90.0 \
  y_center_deg:=90.0 \
  xmax:=150.0 xmin:=30.0 \
  ymax:=130.0 ymin:=50.0 \
  save_frames:=${SAVE_FRAMES} \
  watch_fps:=2.0 \
  active_width:=1920 \
  active_height:=1080 \
  active_fps:=30.0 \
  lost_max_frames:=30 \
  conf_threshold:=0.25 \
  max_step_deg:=3 \
  kp_yaw:=0.3 \
  kd_yaw:=0.2 \
  kp_pitch:=0.3 \
  kd_pitch:=0.2 \
  ort_provider:=${PROVIDER} \
  trt_fp16:=${TRT_FP16} \
  trt_cache_dir:=${SCRIPT_DIR}/trt_cache/$([[ "${TRT_FP16}" == "true" ]] && echo fp16 || echo fp32) \
  "${EXTRA_ARGS[@]}"


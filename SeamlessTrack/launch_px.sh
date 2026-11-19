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
#                 frames/ に保存（SAVE_FRAMES=true のとき。SAVE_DIR で変えられる）
#
#  調整用の変数（どれも省略できる。例: KD=0 DEAD_ZONE=1.5 ./launch_px.sh）
#    KP / KD        PD ゲイン（両軸共通）。この制御は「位置に足していく」形なので、
#                   KP は実質的に積分、KD は実質的に比例のゲインとして効く。
#                   KP は遅延が長いほど行き過ぎる。上げる前に遅延を測ること
#                   （目安: 遅延 100 ms 以下なら 0.4 まで。165 ms を超えるなら 0.2）
#                   KD は 1 フレームあたりの誤差の変化に掛ける。0〜0.6 では差が小さい
#    MAX_STEP       1 フレームで動かす最大量（度）
#    DEAD_ZONE      死帯（度、両軸共通）
#    LOST_TIMEOUT   見失ってから WATCH に戻るまでの時間（秒）
#    ACTIVE_W / ACTIVE_H / ACTIVE_FPS   カメラの取り込み（例: ACTIVE_W=1280 ACTIVE_H=720）
#    CAP_BUFFER     カメラ側に溜めるフレーム数（1 = 古いフレームを溜めない。0 = 指定しない）
#                   カメラによっては 1 で fps が半分になる（計測 CSV の loop_ms が約 66 ms）。
#                   そのときは 2 にする
#    LATEST_FRAME   true = 別スレッドでカメラを読み続け、最新の 1 枚だけを使う（既定）
#                   false = 以前と同じ読み方
#    SERVO_LAG_MS   サーボが指令に追いつくまでの遅れ（ms）。CSV の servo_at_capture_* に効く
#    REWAKE_DELAY_MS  WATCH に戻ってから、PIR が HIGH のままでも再起動しない時間（ms）
#    SAVE_DIR       検出画像の保存先（既定は このスクリプトの場所/frames）
#    MODEL_PATH / CALIB_PATH  モデルと校正ファイル（空なら install/px2/share/px2/weights/）
# ============================================================
set -e

PROVIDER="${PROVIDER:-trt}"          # trt | cpu
TRT_FP16="${TRT_FP16:-true}"          # TensorRT の精度: true=FP16 / false=FP32
TRT_ENGINE="${TRT_ENGINE:-}"          # 使う .engine（空なら engines/ を探し、無ければ px2 が作る）
SAVE_FRAMES="${SAVE_FRAMES:-false}"   # true にすると検出フレームごとに PNG を 1 枚保存（ループが遅くなる）
TIMING_CSV="${TIMING_CSV:-}"          # 計測 CSV の出力先（空なら書かない）

KP="${KP:-0.3}"
KD="${KD:-0.3}"
MAX_STEP="${MAX_STEP:-3.0}"
DEAD_ZONE="${DEAD_ZONE:-1.0}"
LOST_TIMEOUT="${LOST_TIMEOUT:-1.0}"
ACTIVE_W="${ACTIVE_W:-1920}"
ACTIVE_H="${ACTIVE_H:-1080}"
ACTIVE_FPS="${ACTIVE_FPS:-30.0}"
CAP_BUFFER="${CAP_BUFFER:-1}"
LATEST_FRAME="${LATEST_FRAME:-true}"
SERVO_LAG_MS="${SERVO_LAG_MS:-0.0}"
REWAKE_DELAY_MS="${REWAKE_DELAY_MS:-1000}"
MODEL_PATH="${MODEL_PATH:-}"
CALIB_PATH="${CALIB_PATH:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAVE_DIR="${SAVE_DIR:-${SCRIPT_DIR}/frames}"

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
echo "INFO: control KP=${KP} KD=${KD} MAX_STEP=${MAX_STEP} DEAD_ZONE=${DEAD_ZONE} LOST_TIMEOUT=${LOST_TIMEOUT}"
echo "INFO: camera ${ACTIVE_W}x${ACTIVE_H} @ ${ACTIVE_FPS} buffer=${CAP_BUFFER} latest_frame=${LATEST_FRAME} save_frames=${SAVE_FRAMES}"

# ros2 launch は空の値（name:=）を受け付けないので、値があるときだけ渡す
EXTRA_ARGS=()
[[ -n "${TIMING_CSV}" ]] && EXTRA_ARGS+=("timing_csv:=${TIMING_CSV}")
[[ -n "${TRT_ENGINE}" ]] && EXTRA_ARGS+=("trt_engine:=${TRT_ENGINE}")
[[ -n "${MODEL_PATH}" ]] && EXTRA_ARGS+=("model_path:=${MODEL_PATH}")
[[ -n "${CALIB_PATH}" ]] && EXTRA_ARGS+=("calib_path:=${CALIB_PATH}")

ros2 launch px3_launch px3_system_launch.py \
  camera_path:=/dev/video/pir_camera \
  serial_port:=/dev/ttyACM0 \
  baudrate:=115200 \
  pir_pin:=2 \
  x_center_deg:=90.0 \
  y_center_deg:=90.0 \
  xmax:=150.0 xmin:=30.0 \
  ymax:=130.0 ymin:=50.0 \
  save_frames:=${SAVE_FRAMES} \
  watch_fps:=2.0 \
  active_width:=${ACTIVE_W} \
  active_height:=${ACTIVE_H} \
  active_fps:=${ACTIVE_FPS} \
  capture_buffer_size:=${CAP_BUFFER} \
  capture_thread:=${LATEST_FRAME} \
  save_dir:=${SAVE_DIR} \
  servo_lag_ms:=${SERVO_LAG_MS} \
  rewake_delay_ms:=${REWAKE_DELAY_MS} \
  lost_timeout_s:=${LOST_TIMEOUT} \
  conf_threshold:=0.25 \
  max_step_deg:=${MAX_STEP} \
  kp_yaw:=${KP} \
  kd_yaw:=${KD} \
  kp_pitch:=${KP} \
  kd_pitch:=${KD} \
  dead_zone_yaw_deg:=${DEAD_ZONE} \
  dead_zone_pitch_deg:=${DEAD_ZONE} \
  ort_provider:=${PROVIDER} \
  trt_fp16:=${TRT_FP16} \
  trt_cache_dir:=${SCRIPT_DIR}/trt_cache/$([[ "${TRT_FP16}" == "true" ]] && echo fp16 || echo fp32) \
  "${EXTRA_ARGS[@]}"

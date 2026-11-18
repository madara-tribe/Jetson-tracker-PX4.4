#!/bin/bash
# ============================================================
#  Docker/docker_enter.sh — PX コンテナを起動する（Jetson 本体で実行）
#  SSH からでも使える（モニタ不要）。
#  事前: sudo ./Docker/pir_setup_udev/setup_udev.sh（1 回だけ）
#
#  変数: IMAGE（既定 px-jetson:latest）  WORK_DIR（既定 ../SeamlessTrack）
#        ARDUINO_DEV（既定 /dev/ttyACM0）
# ============================================================
set -euo pipefail

IMAGE="${IMAGE:-px-jetson:latest}"
echo "INFO: image = ${IMAGE}"
# Host folder that contains src/ and launch_px.sh (default: ../SeamlessTrack next to this script)
WORK_DIR="${WORK_DIR:-$(cd "$(dirname "$0")/../SeamlessTrack" && pwd)}"
ARDUINO_DEV="${ARDUINO_DEV:-/dev/ttyACM0}"
UDEV_SYMLINK="/dev/video/pir_camera"
CONTAINER_CAM="/dev/video/pir_camera"   # must match camera_path in launch_px.sh

# -------------------- checks --------------------
[[ -d "$WORK_DIR/src" ]] || { echo "ERROR: $WORK_DIR/src not found. Set WORK_DIR=..."; exit 1; }

if [[ -e "$UDEV_SYMLINK" ]]; then
  PIR_DEV=$(readlink -f "$UDEV_SYMLINK")
  echo "INFO: PIR camera  ${UDEV_SYMLINK} -> ${PIR_DEV}"
else
  echo "ERROR: ${UDEV_SYMLINK} not found. Run: sudo ./pir_setup_udev/setup_udev.sh"
  echo "       Video devices now:"; v4l2-ctl --list-devices 2>/dev/null || ls /dev/video*
  exit 1
fi
[[ -e "$ARDUINO_DEV" ]] || { echo "ERROR: ${ARDUINO_DEV} not found (Arduino unplugged?)"; ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null; exit 1; }

VIDEO_GID="$(stat -c '%g' "$PIR_DEV")"
TTY_GID="$(stat -c '%g' "$ARDUINO_DEV")"
echo "INFO: group-add GIDs video=${VIDEO_GID} tty=${TTY_GID}"

# -------------------- optional: GPU / X11 --------------------
GPU_ARGS=()
if docker info 2>/dev/null | grep -qi 'nvidia'; then
  GPU_ARGS=(--runtime nvidia)
else
  echo "WARN: nvidia runtime not found in docker -> TensorRT (PROVIDER=trt) will not work. Fix: sudo apt install nvidia-container-toolkit"
fi

X11_ARGS=()
if [[ -n "${DISPLAY:-}" ]]; then
  xhost +local:docker >/dev/null 2>&1 || true
  X11_ARGS=(--env "DISPLAY=${DISPLAY}" --volume /tmp/.X11-unix:/tmp/.X11-unix:rw)
fi

# -------------------- run --------------------
docker run -it --rm \
  --net=host --ipc=host \
  --privileged \
  "${GPU_ARGS[@]}" "${X11_ARGS[@]}" \
  --env "TZ=Asia/Tokyo" \
  --device "${PIR_DEV}:${CONTAINER_CAM}" \
  --device "${ARDUINO_DEV}:${ARDUINO_DEV}" \
  --group-add "${VIDEO_GID}" \
  --group-add "${TTY_GID}" \
  -v "${WORK_DIR}:/ros2_ws/work" \
  "${IMAGE}"

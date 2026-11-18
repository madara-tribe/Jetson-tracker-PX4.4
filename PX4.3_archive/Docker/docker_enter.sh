#!/bin/bash
# ============================================================
#  docker_enter.sh  (旧 run.sh / docker 起動用)
#  役割     : ホストの udev symlink (/dev/video/pir_camera) から実ノードを
#             解決し、コンテナ内へ launch が期待する名前と同名でマッピング
#             して test:latest コンテナを起動する。
#  実行場所 : ホスト
#  実行順序 : setup_udev.sh (+ 必要なら setup_serial.sh) の後。
#  前提     : /dev/video/pir_camera と /dev/ttyACM0 が存在すること。
#  起動後   : コンテナ内 /ros2_ws/work で launch_px.sh を実行する。
#
#  NOTE: --group-add は「グループ名」ではなくホスト上の実デバイスの
#        数値 GID を stat で取得して渡す。コンテナ内のグループ DB に
#        依存しないため、video/dialout の GID がホストと食い違う
#        イメージでも確実にデバイスへアクセスできる。
# ============================================================
set -euo pipefail

ARDUINO_DEV="/dev/ttyACM0"
UDEV_SYMLINK="/dev/video/pir_camera"
CONTAINER_CAM="/dev/video/pir_camera"   # コンテナ内デバイス名 = launch の camera_path と一致させる
HOST_USER="${SUDO_USER:-$USER}"

# -------------------- カメラ解決 --------------------
if [[ -L "$UDEV_SYMLINK" ]]; then
  PIR_DEV=$(readlink -f "$UDEV_SYMLINK")
  echo "INFO: Resolved PIR_DEV=${PIR_DEV} from symlink ${UDEV_SYMLINK}"
elif [[ -e "$UDEV_SYMLINK" ]]; then
  PIR_DEV="$UDEV_SYMLINK"
  echo "INFO: UDEV_SYMLINK is a real device node: ${PIR_DEV}"
else
  echo "ERROR: ${UDEV_SYMLINK} not found. Run udev setup first:"
  echo "  sudo ./ir_setup_udev/setup_udev.sh"
  exit 1
fi

if [[ ! -e "$PIR_DEV" ]]; then
  echo "ERROR: Resolved device '${PIR_DEV}' not found on host. Check camera connection."
  exit 1
fi

# -------------------- Arduino 確認 --------------------
if [[ ! -e "$ARDUINO_DEV" ]]; then
  echo "ERROR: ${ARDUINO_DEV} not found on host."
  exit 1
fi

# -------------------- video グループ所属チェック --------------------
if ! id -nG "$HOST_USER" | tr ' ' '\n' | grep -qx video; then
  echo "ERROR: user '$HOST_USER' is not in group 'video'"
  echo "Run sudo ./ir_setup_udev/setup_udev.sh and then log out/in once (or run: newgrp video)"
  exit 1
fi

# -------------------- 実デバイスの数値 GID を取得 --------------------
VIDEO_GID="$(stat -c '%g' "$PIR_DEV")"
TTY_GID="$(stat -c '%g' "$ARDUINO_DEV")"

echo "INFO: PIR device perms : $(stat -c 'mode=%a user=%U group=%G' "$PIR_DEV")"
echo "INFO: Arduino perms    : $(stat -c 'mode=%a user=%U group=%G' "$ARDUINO_DEV")"
echo "INFO: host user groups : $(id -nG "$HOST_USER")"
echo "INFO: using group-add GIDs: video=${VIDEO_GID}, tty=${TTY_GID}"

# -------------------- X11 許可 --------------------
xhost +local:docker

# -------------------- コンテナ起動 --------------------
docker run -it --rm \
  --net=host \
  --privileged \
  --env="DISPLAY=$DISPLAY" \
  --volume="/tmp/.X11-unix:/tmp/.X11-unix:rw" \
  --device="${PIR_DEV}:${CONTAINER_CAM}" \
  --device="${ARDUINO_DEV}:${ARDUINO_DEV}" \
  --group-add="${VIDEO_GID}" \
  --group-add="${TTY_GID}" \
  -v /home/hagi/Downloads/place/pC_place/:/ros2_ws \
  px-jetson:latest

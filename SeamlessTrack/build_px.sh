#!/usr/bin/env bash
# ============================================================
#  build_px.sh  — colcon build inside the container (/ros2_ws/work)
#  Removes build/ install/ log/ first if they were made on x86.
# ============================================================
set -e
cd /ros2_ws/work
if [[ -f install/px2/lib/px2/px2 ]] && ! file install/px2/lib/px2/px2 | grep -q aarch64; then
  echo "INFO: removing x86 build artifacts"; rm -rf build install log
fi
if [[ ! -s src/px2/weights/yolo11.onnx ]]; then
  echo "ERROR: src/px2/weights/yolo11.onnx is empty. Copy the real model first."; exit 1
fi
source /opt/ros/humble/setup.bash
# sequential + -j4 keeps memory use safe on Orin Nano 8GB
MAKEFLAGS="-j4" colcon build --symlink-install --executor sequential \
  --cmake-args -DCMAKE_BUILD_TYPE=Release
echo "Done. Next: source install/setup.bash && ./launch_px.sh"

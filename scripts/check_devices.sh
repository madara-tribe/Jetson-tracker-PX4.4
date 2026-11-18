#!/usr/bin/env bash
# ============================================================
#  scripts/check_devices.sh — カメラ・Arduino・権限・Docker の確認（ホスト）
# ============================================================
echo "== camera (32e4:9230)"; lsusb | grep 32e4 || echo "NG: カメラが見つかりません"
echo "== udev symlink"; ls -l /dev/video/pir_camera 2>/dev/null || echo "NG: sudo ./Docker/pir_setup_udev/setup_udev.sh"
echo "== Arduino"; ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "NG: Arduino が見つかりません"
echo "== groups"; id -nG | tr ' ' '\n' | grep -E "^(docker|dialout|video)$" | tr '\n' ' '; echo
echo "== docker runtime"; docker info 2>/dev/null | grep -i runtimes || echo "NG: docker"
echo "== images"; docker images | grep px-jetson || echo "NG: px-jetson イメージがありません（cd Docker && make）"
echo "== engines"; ls -lh "$(cd "$(dirname "$0")/.." && pwd)/SeamlessTrack/engines/"*.engine 2>/dev/null || echo "（engines なし: コンテナ内で ./make_engines.sh）"
echo "== model"; ls -lh "$(cd "$(dirname "$0")/.." && pwd)/SeamlessTrack/src/px2/weights/yolo11.onnx" 2>/dev/null || echo "NG: yolo11.onnx がありません"

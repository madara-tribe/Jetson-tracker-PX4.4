#!/usr/bin/env bash
# ============================================================
#  scripts/install_calib_deps.sh — キャリブレーション用ライブラリ（ホスト）
#    calibration/*.py と camera_calib_kit/*.py を Jetson 本体で動かすため
# ============================================================
set -euo pipefail
sudo apt-get update
sudo apt-get install -y python3-pip python3-tk python3-opencv python3-numpy
python3 -m pip install --user telemetrix pyserial
python3 -c "import cv2, numpy, serial, tkinter; from telemetrix import telemetrix; print('OK', cv2.__version__)"

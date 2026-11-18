#!/bin/bash
# ============================================================
#  setup_udev.sh
#  役割     : PIR カメラ (HD USB Camera 32e4:9230) 用 udev ルールを設置し、
#             /dev/video/pir_camera シンボリックリンクを作成する。
#  実行場所 : ホスト (sudo 必須)
#  前提     : 同じディレクトリに 99-pir_camera.rules があること。
#  実行順序 : 1番目。docker_enter.sh より前に一度だけ実行（再接続後も再実行可）。
# ============================================================
set -e

echo "=== PIR Camera (Night/Pattern-C) udev Setup Script ==="

# Must run as root
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run with sudo"
   exit 1
fi

USER_NAME=$(logname 2>/dev/null || echo "$SUDO_USER")
echo "Setting up for user: $USER_NAME"

# Add user to dialout (serial) and video (camera) groups
usermod -aG dialout "$USER_NAME"
usermod -aG video "$USER_NAME"
echo "Added $USER_NAME to dialout and video groups"

# Install udev rule (.rules はこのスクリプトと同じディレクトリに置く)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rm -f /etc/udev/rules.d/99-pir_camera.rules
cp "${SCRIPT_DIR}/99-pir_camera.rules" /etc/udev/rules.d/
echo "Installed 99-pir_camera.rules"

# Reload + trigger
udevadm control --reload-rules
udevadm trigger --subsystem-match=video4linux
echo "Reloaded and triggered udev rules"

sleep 2

# Verify
echo ""
echo "=== Verification ==="
if [[ -L /dev/video/pir_camera ]]; then
    RESOLVED=$(readlink -f /dev/video/pir_camera)
    echo "Symlink exists: /dev/video/pir_camera -> $RESOLVED"

    if v4l2-ctl -d "$RESOLVED" --all 2>&1 | grep -q "Video Capture"; then
        echo "$RESOLVED is a capture node"
    else
        echo "WARNING: $RESOLVED may not be a capture node!"
    fi

    echo ""
    echo "Current camera format:"
    v4l2-ctl -d "$RESOLVED" --get-fmt-video
else
    echo "Symlink /dev/video/pir_camera not found!"
    echo ""
    echo "Available video devices:"
    v4l2-ctl --list-devices
    exit 1
fi

echo ""
echo "=== Setup Complete ==="
echo "Log out / back in (or run: newgrp video) for group changes to take effect."

#!/usr/bin/env bash
# ============================================================
#  scripts/jetson_setup.sh — Jetson 本体の初期設定（1 回だけ）
#    時刻、Docker を sudo なしで使う設定、カメラ・シリアルの権限、
#    必要なツール、カメラの udev（/dev/video/pir_camera）
#  実行: ./scripts/jetson_setup.sh   （終わったら一度ログアウトして入り直す）
# ============================================================
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"

echo "== L4T: $(head -1 /etc/nv_tegra_release 2>/dev/null || echo unknown)"
grep -q "R36" /etc/nv_tegra_release 2>/dev/null || echo "WARN: R36（JetPack 6）ではありません"

sudo timedatectl set-timezone Asia/Tokyo
sudo apt-get update
sudo apt-get install -y v4l-utils psmisc
sudo usermod -aG docker,dialout,video "$USER"

docker info 2>/dev/null | grep -qi nvidia \
  && echo "OK: docker の nvidia ランタイムあり" \
  || echo "WARN: docker に nvidia ランタイムがありません（sudo apt install nvidia-container-toolkit）"

echo "== カメラ udev"
sudo "$REPO/Docker/pir_setup_udev/setup_udev.sh"

echo "完了。グループを反映するため、一度ログアウトして入り直してください。"

#!/bin/bash
# ============================================================
#  kill_camera_users.sh
#  役割     : /dev/video/pir_camera を掴んでいるプロセスを解放する。
#             「Device busy」でカメラが開けないときに使う。
#  実行場所 : ホスト（コンテナ外でデバイスを掴むプロセスを止める用途）
#  実行順序 : 必要時のみ。docker_enter.sh が camera open に失敗した場合など。
# ============================================================
set -e
VIDEO="/dev/video/pir_camera"

# fuser (psmisc) が無ければ導入
if ! command -v fuser &> /dev/null; then
  echo "[INFO] Installing psmisc (for fuser)..."
  sudo apt update
  sudo apt install -y psmisc
fi

# symlink を実ノードに解決する。
# fuser は symlink 自体ではなく解決先の実デバイスを見る必要があるため。
TARGET="$VIDEO"
if [[ -L "$VIDEO" ]]; then
  TARGET="$(readlink -f "$VIDEO")"
fi

if [[ ! -e "$TARGET" ]]; then
  echo "[INFO] $VIDEO not present (camera unplugged?). Nothing to do."
  exit 0
fi

# set -e 下でも、使用プロセスが無いとき fuser の非ゼロ終了で
# スクリプトごと落ちないよう || true を付ける。
PIDS=$(fuser "$TARGET" 2>/dev/null || true)

if [ -z "$PIDS" ]; then
  echo "[INFO] $TARGET is free."
else
  echo "[INFO] $TARGET is used by: $PIDS"
  for PID in $PIDS; do
    echo "[INFO] Killing PID $PID..."
    sudo kill -9 "$PID"
  done
  echo "[INFO] All processes using $TARGET have been terminated."
fi

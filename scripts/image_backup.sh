#!/usr/bin/env bash
# ============================================================
#  scripts/image_backup.sh — Docker イメージの保存・復元
#    ./scripts/image_backup.sh save     → ~/px-jetson_latest.tar.gz
#    ./scripts/image_backup.sh load     ← ~/px-jetson_latest.tar.gz
#  ONNX Runtime の配布元が消えているため、イメージは作り直せないことがある。
#  大事なイメージは保存しておく。
# ============================================================
set -euo pipefail
IMAGE="${IMAGE:-px-jetson:latest}"
FILE="${FILE:-$HOME/px-jetson_latest.tar.gz}"
case "${1:-}" in
  save) docker save "$IMAGE" | gzip > "$FILE"; ls -lh "$FILE" ;;
  load) gunzip -c "$FILE" | docker load ;;
  *) echo "usage: $0 save|load"; exit 2 ;;
esac

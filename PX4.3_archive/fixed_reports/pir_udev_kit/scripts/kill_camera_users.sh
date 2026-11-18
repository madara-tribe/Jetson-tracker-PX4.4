#!/bin/bash
set -e
VIDEO="/dev/video/pir_camera"

# Install psmisc (provides fuser) if missing
if ! command -v fuser &> /dev/null; then
  echo "[INFO] Installing psmisc (for fuser)..."
  sudo apt update
  sudo apt install -y psmisc
fi

# Resolve symlink to real node (fuser needs the actual device)
TARGET="$VIDEO"
if [[ -L "$VIDEO" ]]; then
  TARGET="$(readlink -f "$VIDEO")"
fi

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

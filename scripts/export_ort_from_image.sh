#!/usr/bin/env bash
# ============================================================
#  scripts/export_ort_from_image.sh
#  既存のイメージから ONNX Runtime 1.14.1（aarch64）を取り出し、
#  Docker/third_party/onnxruntime-1.14.1-aarch64.tgz に保存する。
#  Dockerfile はこのファイルがあればダウンロードせずに使う。
#  （microsoft/onnxruntime が GitHub から消えているため）
# ============================================================
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${IMAGE:-px-jetson:latest}"
OUT="$REPO/Docker/third_party/onnxruntime-1.14.1-aarch64.tgz"
mkdir -p "$(dirname "$OUT")"
docker run --rm --entrypoint sh "$IMAGE" -c \
  'cd /usr/local && tar czf - lib/libonnxruntime.so* include/onnxruntime' > "$OUT"
tar tzf "$OUT" | grep -E "libonnxruntime|onnxruntime_cxx_api.h"
ls -lh "$OUT"

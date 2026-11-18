#!/usr/bin/env bash
# ============================================================
#  scripts/perf_mode.sh — 性能を最大に固定する（再起動のたびに 1 回）
#    ./scripts/perf_mode.sh            電源モード MAXN + クロック固定
#    ./scripts/perf_mode.sh --free-mem 上に加えてキャッシュを空ける
#                                      （TensorRT エンジンを作る前に）
# ============================================================
set -euo pipefail
MODE="${MODE:-$(grep -E "POWER_MODEL ID" /etc/nvpmodel.conf | grep -i maxn | head -1 | sed -E 's/.*ID=([0-9]+).*/\1/')}"
sudo nvpmodel -m "${MODE:-0}"
sudo jetson_clocks
sudo nvpmodel -q
if [[ "${1:-}" == "--free-mem" ]]; then
  sudo sync && sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
fi
free -h

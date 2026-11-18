#!/usr/bin/env bash
# ============================================================
#  host_monitor.sh — Jetson 本体（コンテナの外）で実行する
#  電源モードを固定し、計測中の CPU / GPU 使用率・温度・電力を記録する
#
#    ./bench/host_monitor.sh cpu_base       # TAG はコンテナ側と同じにする
#    （計測が終わったら Ctrl+C）
#
#  出力: bench/results/<TAG>/tegrastats.log, power_mode.txt
# ============================================================
set -euo pipefail
TAG="${1:?usage: $0 <TAG>}"
OUT="$(cd "$(dirname "$0")" && pwd)/results/$TAG"
mkdir -p "$OUT"

# 毎回同じ条件で測るため、最大性能モード + クロック固定
if [[ "${SKIP_POWER:-0}" != "1" ]]; then
  echo "電源モード一覧:"; grep -E "POWER_MODEL ID" /etc/nvpmodel.conf || true
  MODE="${MODE:-$(grep -E "POWER_MODEL ID" /etc/nvpmodel.conf | grep -i maxn | head -1 | sed -E 's/.*ID=([0-9]+).*/\1/')}"
  MODE="${MODE:-0}"
  sudo nvpmodel -m "$MODE"
  sudo jetson_clocks
fi
{ sudo nvpmodel -q; sudo jetson_clocks --show; } > "$OUT/power_mode.txt" 2>&1 || true
echo "power mode -> $OUT/power_mode.txt"

echo "tegrastats を記録中: $OUT/tegrastats.log （Ctrl+C で終了）"
trap 'echo; echo stopped' INT
sudo tegrastats --interval 500 | tee "$OUT/tegrastats.log" > /dev/null || true

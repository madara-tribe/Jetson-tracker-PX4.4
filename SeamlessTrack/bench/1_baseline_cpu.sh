#!/usr/bin/env bash
# ============================================================
#  Step 1: CPU の基準値を取る（コンテナ内 /ros2_ws/work で実行）
#
#    TAG=cpu_base ./bench/1_baseline_cpu.sh
#
#  A) オフライン: 同じ画像で 300 回推論（bench_yolo）→ offline_cpu.csv
#  B) 実機     : launch_px.sh を CPU で LIVE_SEC 秒動かす → live_cpu.csv
#  C) 集計     : summary.md
#
#  変えられる値: ITERS=300  LIVE_SEC=120（0 で実機を省略）  IMAGE=path.jpg
#  ※ 同時にホスト側で ./bench/host_monitor.sh cpu_base を動かすと電力・温度も残る
# ============================================================
set -euo pipefail
source "$(dirname "$0")/_common.sh"
save_env cpu

run_offline cpu cpu 0
run_live    cpu cpu false

python3 "$WORK/bench/summarize.py" "$OUT"
echo; echo "結果: $OUT/summary.md"

# bench/ — 推論速度の計測

TAG は結果フォルダ名（`bench/results/<TAG>/`）。ホストとコンテナで同じ TAG を使う。

| スクリプト | 実行場所 | 内容 |
|---|---|---|
| `host_monitor.sh <TAG>` | Jetson 本体 | 電源モードを最大に固定し、tegrastats（CPU/GPU 使用率・温度・電力）を記録。Ctrl+C で終了 |
| `1_baseline_cpu.sh` | コンテナ | CPU の基準値（オフライン 300 回 + 実機 120 秒） |
| `2_3_trt_compare.sh` | コンテナ | CPU / TensorRT FP32 / FP16 を比較（`engines/` のエンジンを自動で使う） |
| `summarize.py` | どこでも | CSV を集計して `summary.md` を作る |

```bash
# 端末A（Jetson 本体）
./SeamlessTrack/bench/host_monitor.sh cpu_base
# 端末B（コンテナ内）
TAG=cpu_base ./bench/1_baseline_cpu.sh
TAG=trt_day1 BASELINE=cpu_base ./bench/2_3_trt_compare.sh
python3 bench/summarize.py bench/results/trt_day1 --baseline bench/results/cpu_base
```

変えられる値: `ITERS=300` `LIVE_SEC=120`（0 で実機を省略） `IMAGE=x.jpg` `LIVE_FP32=1` `TRT_ENGINE_FP16=...` `TRT_ENGINE_FP32=...`

## 出力（bench/results/<TAG>/）

| ファイル | 中身 |
|---|---|
| `offline_<label>.csv` | 同じ画像で N 回推論した 1 回ごとの pre / infer / post / total（trt は h2d / gpu / d2h も） |
| `live_<label>.csv` | 実機の 1 フレームごとの capture / pre / infer / post / control / loop、検出数 |
| `tegrastats.log`, `power_mode.txt` | CPU・GPU 使用率、温度、電力、電源モード |
| `summary.md` | 集計表（平均 / p50 / p95、FPS、CPU 比の速度倍率） |

## 公平に比べるための注意
- 全計測で電源モードを同じにする（`host_monitor.sh` か `scripts/perf_mode.sh`）
- 実機の値は人の動きで変わる。主に比べるのは **offline の infer**
- 計測中は VNC を閉じる。`SAVE_FRAMES` はスクリプトが自動で false にする

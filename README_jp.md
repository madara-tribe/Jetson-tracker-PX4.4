# SensorFireld-PX4.4

夜間用の人物追尾システム（PX）。Jetson Orin Nano 上で動かす。
PIR センサーが人を検知すると、IR カメラの映像から YOLO11 で人を見つけ、パン・チルトのサーボで追う。

- 推論は **CPU（ONNX Runtime）** と **GPU（TensorRT FP16 / FP32）** を切り替えられる
- ROS 2 Humble を Docker コンテナで動かす
- 配線は [docs/WIRING.md](docs/WIRING.md)

| 推論 | 1 フレームあたり（オフライン計測） |
|---|---|
| CPU（ONNX Runtime 1.14） | 約 88 ms（起動時の試し推論） |
| TensorRT FP32 | GPU 計算 8.6 ms |
| TensorRT FP16 | GPU 計算 5.4 ms、前処理込み合計 10.3 ms（約 97 FPS） |

詳しい数値は [docs/02_experiment_data.md](docs/02_experiment_data.md)。

---

## 構成

```
Docker/            Docker イメージ（Dockerfile, docker_enter.sh, Makefile, pir_setup_udev/）
SeamlessTrack/     ROS 2 ワークスペース（コンテナ内で /ros2_ws/work）
  src/             システム本体（px2: 推論・制御 / px3: サーボ・PIR / px3_launch / custom_msgs）
  build_px.sh      ビルド
  launch_px.sh     起動
  make_engines.sh  TensorRT エンジンを作る
  bench/           推論速度の計測
  engines/         TensorRT エンジン（git に入れない）
scripts/           Jetson 本体で使うスクリプト（初期設定、性能モード、イメージ保存 など）
calibration/       PIR・サーボ・カメラの確認とキャリブレーション（Jetson 本体で実行）
camera_calib_kit/  カメラの内部パラメータ（calib_result_ir.yaml）を作る道具
ci_checks/         CI のチェック（run_local.sh で手元でも実行できる）
docs/              配線、構築ログ、実験データ、コマンド集
PX4.3_archive/     PX4.3 から使わなくなったもの
```

---

## 動作環境

| 項目 | 値 |
|---|---|
| ボード | Jetson Orin Nano 開発キット（8GB） |
| JetPack | 6.2.x（L4T R36、Ubuntu 22.04） |
| Docker イメージ | `px-jetson:latest`（l4t-jetpack r36.4.0 + ROS 2 Humble + OpenCV 4.5.4 + ONNX Runtime 1.14.1 CPU + TensorRT 10.3） |
| カメラ | ELP IR USB カメラ（32e4:9230）、1920x1080 @ 30 fps |
| マイコン | Arduino（Telemetrix）、USB シリアル `/dev/ttyACM0` |
| センサー | PIR（SB612B）、サーボ 2 個（パン・チルト） |

---

## 構築（初回だけ）

### 1. Jetson 本体の準備
```bash
git clone <this repo> ~/SensorFireld-PX4.4 && cd ~/SensorFireld-PX4.4
./scripts/jetson_setup.sh        # 時刻、Docker・シリアル・カメラの権限、カメラの udev
# 一度ログアウトして入り直す
./scripts/check_devices.sh       # カメラ・Arduino・権限の確認
```

### 2. 重いファイルを置く
git には入れていないので、手元から cp する（下の「重いファイル」参照）。
最低限必要なのは **`SeamlessTrack/src/px2/weights/yolo11.onnx`**。

### 3. Docker イメージを作る
```bash
cd Docker && make                # px-jetson:latest
```
ONNX Runtime は GitHub から入手できなくなっているため、
`Docker/third_party/onnxruntime-1.14.1-aarch64.tgz` が必要。
既存のイメージがあるなら、そこから取り出せる: `./scripts/export_ort_from_image.sh`

### 4. ビルド（コンテナ内）
```bash
./Docker/docker_enter.sh         # コンテナに入る（/ros2_ws/work = SeamlessTrack/）
./build_px.sh
source install/setup.bash
```

### 5. TensorRT エンジンを作る（GPU を使う場合、1 回だけ）
```bash
# Jetson 本体（別の端末）: 性能を最大にしてメモリを空ける
./scripts/perf_mode.sh --free-mem
# コンテナ内: FP32 と FP16 のエンジンを作る（約 10 分）
./make_engines.sh                # → engines/yolo11_fp16.engine, yolo11_fp32.engine
```

---

## 動かす

```bash
# Jetson 本体（再起動のたびに 1 回）
./scripts/perf_mode.sh

# コンテナに入る
./Docker/docker_enter.sh

# コンテナ内
source install/setup.bash
PROVIDER=cpu ./launch_px.sh                   # CPU
PROVIDER=trt ./launch_px.sh                   # GPU: TensorRT FP16（既定）
PROVIDER=trt TRT_FP16=false ./launch_px.sh    # GPU: TensorRT FP32
```

- 止めるときは **Ctrl+C**（Ctrl+Z は使わない。プロセスが残ってカメラを掴んだままになる）
- PIR が人を検知するまでは待機（WATCH）。検知すると推論・追尾（DETECT）に入る
- 検出画像は `SAVE_FRAMES=true` のときだけ `SeamlessTrack/frames/` に保存される（`SAVE_DIR` で変えられる）
- ログに `px2: inference provider = trt` と、1 秒ごとの `px2[timing] ... infer=...ms` が出る

| 変数 | 意味 | 既定 |
|---|---|---|
| `PROVIDER` | `trt`（TensorRT）/ `cpu`（ONNX Runtime） | `trt` |
| `TRT_FP16` | `true`=FP16 / `false`=FP32 | `true` |
| `TRT_ENGINE` | 使うエンジン（空なら `engines/` を自動で使う） | 空 |
| `SAVE_FRAMES` | 検出したフレームを PNG で保存する（1 フレーム 1 枚。ループが遅くなる） | `false` |
| `TIMING_CSV` | 1 フレームごとの時間・誤差・指令角を CSV に書く | 空 |
| `KP` / `KD` | PD ゲイン（両軸共通）。`KP` は実質的に積分、`KD` は実質的に比例として効く。`KP` を上げる前に遅延を測ること | `0.3` / `0.3` |
| `MAX_STEP` | 1 フレームで動かす最大量（度） | `3.0` |
| `DEAD_ZONE` | 死帯（度）。指令が整数のうちは 1.0 未満にしない | `1.0` |
| `LOST_TIMEOUT` | 見失ってから WATCH に戻るまでの時間（秒） | `1.0` |
| `ACTIVE_W` / `ACTIVE_H` / `ACTIVE_FPS` | カメラの取り込み | `1920` / `1080` / `30.0` |
| `CAP_BUFFER` | カメラ側に溜めるフレーム数（0 = 指定しない）。fps が半分になるカメラでは 2 | `1` |
| `LATEST_FRAME` | `true` = 別スレッドでカメラを読み続け、最新の 1 枚だけを使う | `true` |
| `SAVE_DIR` | 検出画像の保存先 | `SeamlessTrack/frames` |
| `MODEL_PATH` / `CALIB_PATH` | モデルと校正ファイル（空なら `install/px2/share/px2/weights/`） | 空 |
| `SERVO_LAG_MS` | サーボが指令に追いつくまでの遅れ（ms）。CSV の `servo_at_capture_*` に使う | `0.0` |
| `REWAKE_DELAY_MS` | WATCH に戻ってから、PIR が HIGH のままでも再起動しない時間（ms） | `1000` |

PIR を使わずにカメラ・推論・サーボだけ確かめるときは、[docs/WIRING.md](docs/WIRING.md) の「配線の確認手順」を参照。

---

## 計測

```bash
./SeamlessTrack/bench/host_monitor.sh cpu_base                 # Jetson 本体
TAG=cpu_base ./bench/1_baseline_cpu.sh                          # コンテナ内: CPU の基準値
TAG=trt_day1 BASELINE=cpu_base ./bench/2_3_trt_compare.sh       # コンテナ内: TensorRT と比較
```
詳しくは [SeamlessTrack/bench/README.md](SeamlessTrack/bench/README.md)。

---

## 重いファイル（git に入れない。手元から cp する）

| ファイル | 置き場所 | 大きさ | 作り方 / 入手先 |
|---|---|---|---|
| `yolo11.onnx` | `SeamlessTrack/src/px2/weights/` | 約 10 MB | **必須**。YOLO11n（出力 `[1,84,8400]`、COCO 80 クラス） |
| `yolo11_fp16.engine` | `SeamlessTrack/engines/` | 約 8 MB | `./make_engines.sh`（Jetson ごと・TensorRT の版ごとに作り直し） |
| `yolo11_fp32.engine` | `SeamlessTrack/engines/` | 約 12 MB | 同上 |
| `onnxruntime-1.14.1-aarch64.tgz` | `Docker/third_party/` | 数 MB | `./scripts/export_ort_from_image.sh`（イメージを作り直すときだけ必要） |
| `px-jetson_latest.tar.gz` | `~/`（リポジトリの外） | 数 GB | `./scripts/image_backup.sh save`（イメージのバックアップ） |

---

## リモート操作

Tailscale と SSH で、どのネットワークからでも入れる。VNC は SSH トンネル経由。
手順は [docs/03_commands.md](docs/03_commands.md) の「SSH・Tailscale・VNC」。

---

## CI

| ワークフロー | 内容 |
|---|---|
| `cpp-ci` | clang-format（14）のチェック |
| `python-lint` | ruff の整形・lint と unit test |
| `px-parameter-contract` | launch と C++ のパラメータが一致しているか |
| `colcon-build` | ROS 2 のビルド。**ONNX Runtime が GitHub から消えているため、手動実行だけ** |

手元で同じチェック: `./ci_checks/run_local.sh`（`--fix` で整形を自動修正）。uv と clang-format 14 が必要（Mac での実行を想定）。

---

## ドキュメント

| ファイル | 内容 |
|---|---|
| [docs/WIRING.md](docs/WIRING.md) | 配線図と接続表 |
| [docs/01_conversation_log.md](docs/01_conversation_log.md) | 構築ログ（何をして、何が起きて、どう直したか） |
| [docs/02_experiment_data.md](docs/02_experiment_data.md) | 環境・計測結果・エラー一覧 |
| [docs/03_commands.md](docs/03_commands.md) | 用途別のコマンド集 |
| [docs/04_fix_log_2026-11-19.md](docs/04_fix_log_2026-11-19.md) | 修正 F01〜F27 の記録（動きが変わった点、修正箇所、確認項目） |
| [docs/04_fix_patch_2026-11-19.patch](docs/04_fix_patch_2026-11-19.patch) | 2026-11-18 のコードに対する差分（修正 F01〜F27） |
| [docs/05_roadmap_2026-11-19.md](docs/05_roadmap_2026-11-19.md) | 改良ロードマップ（課題、対応済み・未対応の修正、今後の段階） |
| [docs/06_review_of_fixes_F01-F17.md](docs/06_review_of_fixes_F01-F17.md) | 修正 F01〜F17 のレビュー結果と、その対応 |
| [PX4.3_archive/README.md](PX4.3_archive/README.md) | 使わなくなったものと戻し方 |

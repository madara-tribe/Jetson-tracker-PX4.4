# 02. 実験データ・ログまとめ — PX on Jetson Orin Nano

> **注**: この記録は PX4.3 のフォルダ構成（`Docker/docker_enter_jetson.sh`、`Dockerfile.jetson`、`bench/convert_trt.sh`、`setup_serial.sh` など）の時点で書いたもの。
> PX4.4 での改名・移動は [PX4.3_archive/README.md](../PX4.3_archive/README.md)、今のコマンドは [03_commands.md](03_commands.md) を参照。

new-px-rep の構築・計測で得た値の記録（2026-11-17〜18）。
計測条件が分かるよう、どのツール・どの状態で取ったかも書いている。

---

## 1. ハードウェア・ソフトウェア環境

| 項目 | 値 | 出どころ |
|---|---|---|
| ボード | Jetson Orin Nano 開発キット（8GB） | — |
| GPU | Orin、Compute Capability 8.7、8 SM | trtexec |
| GPU メモリ（CPU と共有） | 7619 MiB | trtexec |
| GPU の最大クロック | 1.02 GHz（MAXN_SUPER 相当） | trtexec |
| ファームウェア | Jetson System firmware 36.4.0-gcid-37537400（2024-09-13） | 起動画面 |
| JetPack | 6.2.1（SD カードイメージ） | — |
| OS | Ubuntu 22.04.5 LTS（jammy） | — |
| TensorRT | 10.3.0 | trtexec |
| メモリ | 合計 7.4Gi、スワップ 11Gi | `free -h` |
| ホスト名 / ユーザー | hagi-desktop / hagi（UID 1000） | — |

### Docker イメージ
| イメージ | 中身 | 状態 |
|---|---|---|
| `px-jetson:latest` | l4t-jetpack r36.4.0 + ROS 2 Humble + OpenCV 4.5.4（Ubuntu）+ ONNX Runtime 1.14.1 aarch64 CPU + TensorRT 10.3（ベースに同梱） | ビルド済み。**ORT の配布元が消えたので作り直せない → `docker save` で保存推奨** |
| `px-jetson:gpu` | 上 + ONNX Runtime 1.20.1 GPU | **作成不可**（microsoft/onnxruntime が GitHub から消失） |

CPU 版イメージのビルド時間（各ステップ）: apt 145 s、OpenCV 97 s、ROS 2 158 s、ORT 2.5 s、pip 7.4 s。

### デバイス
| デバイス | 値 |
|---|---|
| カメラ | ELP IR、USB ID 32e4:9230、`/dev/video/pir_camera` → `/dev/video0`（udev） |
| カメラ設定 | 1920x1080 @ 30 fps（MJPG） |
| キャリブレーション | calib 1280x720 → active 1920x1080 に換算して読み込み |
| Arduino | `/dev/ttyACM0`、115200 baud、Telemetrix |
| グループ GID | video=44、tty=20 |

---

## 2. ネットワーク

| 接続 | Jetson の IP | 備考 |
|---|---|---|
| 会社 Wi-Fi（11/17 初回） | 192.168.68.65/22 | DHCP |
| スマホのテザリング | 172.20.10.2/28 | Mac から ping 不可（端末間通信が通らない） |
| 会社 Wi-Fi（11/17 夕〜11/18） | 192.168.50.84/24 | DHCP |
| **Tailscale（固定）** | **100.103.158.11（hagi-desktop）** | どのネットワークでも同じ |
| USB-C 直結 | 192.168.55.1 | ネットワーク不要 |

Tailscale 上の他の機器: Mac 100.121.197.61、DGX `spark-2f6d` 100.75.134.61（11/17 時点で offline）。

SSH: `ssh.service` active、port 22、自動起動。
x11vnc: systemd サービス、active、`127.0.0.1:5900`（localhost のみ）、表示 `:0`、Mac 側は SSH トンネルで 15901。

---

## 3. 推論の計測結果

### 3-1. trtexec（TensorRT 単体、`bench/convert_trt.sh`、11/18 09:2x）
モデル: yolo11.onnx（入力 `images[1,3,640,640]`、出力 `output0[1,84,8400]`）。`--warmUp=500 --duration=10 --useSpinWait`。

| 精度 | 1秒あたり推論回数 | Latency 平均（コピー込み） | GPU 計算 平均 | GPU 計算 p99 | H2D | D2H | エンジン作成 | サイズ |
|---|---|---|---|---|---|---|---|---|
| FP32 | 116.6 | 9.12 ms | 8.56 ms | 9.44 ms | 0.38 ms | 0.17 ms | 172 s | 12.4 MB |
| **FP16** | **194.7** | **5.64 ms** | **5.13 ms** | **6.23 ms** | 0.29 ms | 0.22 ms | 398 s | 8.2 MB |

- FP16 は FP32 の約 1.7 倍速い。
- FP16 の作成中、メモリ不足で飛ばした tactic は 342 件（このときの空きは 71MB 程度）。FP32 は 0 件。
- Enqueue（CPU 側の投入）平均 1.86 ms。

### 3-2. px2 の推論（`bench_yolo`、同じ画像で繰り返し）

**(a) px2 が自分で作った FP16 エンジン（11/18 09:2x、jetson_clocks 前、50 回）**
| 段階 | 平均 | p50 | p95 | 最大 |
|---|---|---|---|---|
| pre（前処理、CPU） | 7.57 | 7.50 | 8.77 | 9.41 |
| infer | 19.97 | 19.78 | 23.06 | 23.36 |
| 　h2d | 1.10 | 1.02 | 1.58 | 1.72 |
| 　gpu | 17.78 | 17.79 | 20.06 | 20.36 |
| 　d2h | 0.96 | 0.87 | 1.28 | 1.63 |
| post | 0.05 | 0.04 | 0.07 | 0.10 |
| **total** | **27.59** | 27.07 | 31.49 | 32.82 |
→ 36.2 FPS。エンジン作成 589.6 s（空きメモリ 12MB → 0MB まで減り、大量の tactic を飛ばした）。

**(b) trtexec の FP16 エンジンを `--engine` で読み込み（11/18 09:4x、jetson_clocks 後、200 回）**
| 段階 | 平均 | p50 | p95 | 最大 |
|---|---|---|---|---|
| pre（前処理、CPU） | 3.67 | 3.63 | 3.88 | 6.08 |
| infer | 6.57 | 6.55 | 7.18 | 7.27 |
| 　h2d | 0.65 | 0.64 | 0.71 | 1.22 |
| 　gpu | 5.37 | 5.37 | 5.98 | 6.05 |
| 　d2h | 0.48 | 0.47 | 0.51 | 0.60 |
| post | 0.02 | 0.02 | 0.03 | 0.03 |
| **total** | **10.26** | 10.23 | 10.81 | 12.20 |
→ **97.4 FPS**。エンジン読み込み 0.3 s。

**(a) → (b) の比較**
| | (a) | (b) | 倍率 |
|---|---|---|---|
| GPU 計算 | 17.8 ms | 5.4 ms | 3.3 倍 |
| 前処理 | 7.6 ms | 3.7 ms | 2.1 倍（jetson_clocks の効果と推定） |
| 合計 | 27.6 ms | 10.3 ms | 2.7 倍 |

**今の内訳（b）**: GPU 計算 52%、前処理 36%、コピー 11%、後処理 0%。
→ Step 4 で効くのは前処理の GPU 化（3.7 ms）とコピー削減（1.1 ms）。

### 3-3. まだ取っていないデータ
- CPU（ONNX Runtime 1.14）の基準値：`bench/1_baseline_cpu.sh`
- 実機（カメラあり）の 1 フレームごとの時間：`TIMING_CSV` / `live_*.csv`
- 電力・温度：`bench/host_monitor.sh`（tegrastats）

---

## 4. メモリ

`free -h`（11/18 09:17、停止中のプロセスを消した後）
```
               total        used        free      shared  buff/cache   available
Mem:           7.4Gi       2.2Gi       1.3Gi        41Mi       3.9Gi       5.0Gi
Swap:           11Gi       1.0Mi        11Gi
```
- TensorRT が見る「使えるメモリ」は `free` に近い値。キャッシュ（buff/cache）は `drop_caches` で空けられる。
- エンジン作成時のメモリ不足はエンジンの速さに直結する（3-2 の (a)）。

---

## 5. ビルド結果

| 対象 | 結果 |
|---|---|
| colcon build（custom_msgs / px3_launch / px3 / px2） | 11 s / 3 s / 50 s / 53 s |
| CMake の TensorRT 検出 | `px2: TensorRT enabled (/usr/lib/aarch64-linux-gnu/libnvinfer.so)` |
| 起動ログ（px3） | `x_center_deg=90 / y_center_deg=90 was sent`、`PIR D2 configured`、`serial OK (servo + PIR)` |

---

## 6. エラーと原因・対処の一覧

| # | 症状 | 原因 | 対処 |
|---|---|---|---|
| 1 | 5.1.3 の SD で起動後に画面が暗くなる | ファームウェアがすでに 36.4.0 | 6.2.1 の SD を直接使う |
| 2 | Mac から 172.20.10.2 に ping が通らない | テザリングで端末間通信が通らない | Tailscale、または USB-C 直結 |
| 3 | `nc -vz ori 22` が名前解決エラー | Tailscale の名前が hagi-desktop | IP 100.103.158.11 を使う / `tailscale set --hostname=ori` |
| 4 | heredoc で `>` が出て止まる | 終わりの `EOF` が無い | 続きを貼り、行頭に `EOF` |
| 5 | `UID 0 is not unique`（docker build） | `sudo make jetson` | sudo なし / Makefile を修正 |
| 6 | `/dev/video/pir_camera not found` | udev 未設定、`setup_udev.sh` の `rm` | `rm -f` に修正して実行 |
| 7 | `/root/.../SeamlessTrack/src not found` | sudo で `$HOME=/root` | sudo なし、WORK_DIR 自動判定 |
| 8 | `failed to create symbolic link ... Is a directory` | symlink 有無のビルドを混在 | `rm -rf build install log` |
| 9 | `could not read Username` / 404（ORT） | microsoft/onnxruntime が GitHub から消失 | TensorRT 直接版に方針変更 |
| 10 | `malformed launch argument 'timing_csv:='` | ros2 launch は空の値を受け付けない | 値があるときだけ渡す |
| 11 | カメラが動かない（起動直後） | エンジン作成中 + Ctrl+Z で前のプロセスが残存 | Ctrl+C、`kill -9 %1 %2`、エンジンを先に作る |
| 12 | `Skipping tactic ... insufficient memory` 大量 | エンジン作成時の空きメモリ不足 | drop_caches、trtexec のエンジンを使う |
| 13 | `No module named 'telemetrix'` | ホストにライブラリ未導入 | `pip install --user telemetrix pyserial` ほか |

---

## 7. 成果物（パッチ）の対応

| ファイル | 内容 | 状態 |
|---|---|---|
| px_jetson.zip | Jetson 用 Dockerfile・起動スクリプト（最初の版） | 以下に統合 |
| px_gpu_patch.zip | ONNX Runtime GPU 対応（10 ファイル） | 以下に統合 |
| px_bench_patch.zip | 上 + 計測の仕組み（21 ファイル） | 以下に統合 |
| **px_trt_patch.zip** | **上 + TensorRT 直接版（24 ファイル）** | **最新。これだけ使えばよい** |
| login_ori.txt | Jetson への接続手順（Tailscale / VNC / ssh config） | 現役 |

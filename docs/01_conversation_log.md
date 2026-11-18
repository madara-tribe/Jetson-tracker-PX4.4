# 01. 構築ログ（prompt と回答の記録）— PX on Jetson Orin Nano

> **注**: この記録は PX4.3 のフォルダ構成（`Docker/docker_enter_jetson.sh`、`Dockerfile.jetson`、`bench/convert_trt.sh`、`setup_serial.sh` など）の時点で書いたもの。
> PX4.4 での改名・移動は [PX4.3_archive/README.md](../PX4.3_archive/README.md)、今のコマンドは [03_commands.md](03_commands.md) を参照。

new-px-rep（Jetson GPU 版 PX）の構築記録。日付は JST。
各項目は「自分の prompt → 回答の要点 → 重要なデータ / コマンド」の順。
コマンドの最新・完全版は `03_commands.md`、計測値は `02_experiment_data.md` を参照。

---

## A. ハードウェア準備・JetPack（11/08〜11/17）

### A-1. セットアップ用パーツの確認（11/08）
- **prompt**: Jetson Orin のセットアップに、写真のパーツ（電源コード 2 種、DP→HDMI 変換、ケース）で足りるか。
- **回答**:
  - 電源は日本プラグ付き AC アダプタ（DC バレルジャック、付属品は 19V）を使う。米国 3 ピンコードは不要。
  - Orin Nano の映像出力は DisplayPort のみ。DP→HDMI 変換（サンワ AD-DPHD04、パッシブ）で会社の HDMI モニタに出せる。映らなければアクティブ変換。
  - キーボード・マウスは USB-A 有線なら会社のもので可。microSD は 64GB 以上（UHS-1）。Wi-Fi は内蔵。
- **データ**: 開発キットの USB-A は 4 ポート。初期設定後は SSH で操作できるのでモニタは最初だけ。

### A-2. JetPack の版とダウンロード先（11/09）
- **prompt**: Jetson Orin 用の JetPack の版とダウンロードサイトを調べて。
- **回答**: 最新は 7.2.1（Ubuntu 24.04、SD イメージ廃止）だが、PX には **JetPack 6.2.x（Ubuntu 22.04、SD イメージ）** を推奨（ROS 2 Humble と合う）。
- **データ / URL**:
  - 6.2.1 SD イメージ: https://developer.nvidia.com/embedded/jetpack-sdk-621
    （直リンク `jp62-r1-orin-nano-sd-card-image.zip`）
  - 5.1.3（ファームウェア更新用）: https://developer.nvidia.com/embedded/jetpack-sdk-513
    （「JETSON ORIN NANO DEVELOPER KIT」ボタン → `JP513-orin-nano-sd-card-image_b29.zip`）
  - 手順: https://docs.nvidia.com/jetson/orin-nano-devkit/user-guide/latest/update_firmware.html
  - 起動時に Esc で UEFI。ファームウェアが 36.0 より新しければ 6.x を直接起動できる。

### A-3. 5.1.3 で起動したら画面が暗くなった（11/17）
- **prompt**: 5.1.3 の SD で起動したら NVIDIA ロゴの後すぐ暗くなった。原因は？
- **回答**: 画面のファームウェアが **36.4.0（2024-09-13）** ＝すでに JetPack 6 世代。5.1.3 とは世代が合わず起動しない。**5.1.3 の手順は不要。6.2.1 の SD を直接使う。**

### A-4. Ubuntu Pro は必要か（11/17）
- **回答**: 不要。Skip for now でよい（後から `sudo pro attach` で有効化可）。

### A-5. 起動後にやること（11/17）
- **prompt**: Jetson が起動した。次は PX の Dockerfile 起動と、リモート PC から SSH ログインを考えている。
- **回答**: 順番は **SSH → 基本設定（apt upgrade、電源モード、jtop、Docker の nvidia ランタイム確認）→ PX の Docker**。x86 用 Dockerfile（Ubuntu 20.04 / Foxy）はそのまま動かないので作り直しが必要。

---

## B. SSH・ネットワーク・リモート操作

### B-1. `ip a` / ssh の状態確認と Mac からの接続（11/17）
- **回答**: SSH は active（port 22、自動起動）。Wi-Fi `wlP1p1s0` の IP は 192.168.68.65（DHCP なので変わる）。タイムゾーンが EST になっている。
- **コマンド**: `ssh hagi@192.168.68.65`、`ssh-copy-id`、`~/.ssh/config` に `Host ori`。

### B-2. テザリング中の SSH（11/17）
- **データ**: テザリング時の IP は 172.20.10.2。Mac からの ping が全て timeout。
- **回答**: Mac が同じテザリングにいるか確認。端末間通信が止められている場合は **Tailscale** か **USB-C 直結（`ssh hagi@192.168.55.1`）**。

### B-3. Tailscale・VNC の常時接続設定（11/17）
- **prompt**: 起動と同時に VNC、IP を常に同じに、Mac の ssh/config を設定（DGX 用 login.txt を参考に）。
- **回答**: `login_ori.txt` を作成。
  - IP 固定は Tailscale の IP / 名前で代替（Wi-Fi の IP はこちらで固定できない）。管理画面で Disable key expiry。
  - x11vnc を systemd サービス化（自動ログイン必須、`-localhost`）。
  - Mac 側は DGX（15900）と被らないよう `LocalForward 15901`。
  - モニタを外す場合は DP ダミープラグ推奨。
- **途中のつまずき**: heredoc 貼り付けで `>` が出て止まった → 続きを貼り、行頭に `EOF` だけを書く。
- **結果**: x11vnc は active（127.0.0.1:5900）。Tailscale 上の名前は `ori` ではなく **hagi-desktop（100.103.158.11）**。

### B-4. 192.168.68.65 で SSH できない（11/18）
- **回答**: Wi-Fi の IP はネットワークごとに変わる（192.168.68.65 → 172.20.10.2 → 192.168.50.84）。**`ssh hagi@100.103.158.11`（Tailscale）を使う。**

---

## C. Docker（Jetson 用 PX 環境）

### C-1. Dockerfile と起動コマンドの書き換え（11/17、px_jetson.zip）
- **回答**:
  | 項目 | x86 PC | Jetson |
  |---|---|---|
  | ベース | ubuntu:20.04 | nvcr.io/nvidia/l4t-jetpack:r36.4.0 |
  | ROS 2 | Foxy | Humble |
  | ONNX Runtime | 1.14.1 ソースビルド | 1.14.1 aarch64 の配布版（CPU） |
  | OpenCV | 4.7 ビルド + apt 4.5.4 | Ubuntu の 4.5.4 のみ（cv_bridge と同じ） |
  - `docker_enter_jetson.sh`: SSH 経由（DISPLAY なし）で止まらないよう X11 を任意に、`--runtime nvidia` を自動付与。
- **確認**: Ubuntu 22.04.5 jammy で問題なし。`cat /etc/nv_tegra_release` が R36 であること。

### C-2. ビルド失敗: `UID 0 is not unique`（11/17）
- **原因**: `sudo make jetson` で `$(id -u)` が 0 になった。
- **対処**: sudo なしで実行（docker グループ）、または `--build-arg USER_UID=1000`。Makefile と Dockerfile を sudo でも動くよう修正。

### C-3. コンテナに入れない（11/17）
- **原因 1**: x86 用の `docker_enter.sh` を実行していた → `docker_enter_jetson.sh` を使う。
- **原因 2**: `/dev/video/pir_camera` が無い。`setup_udev.sh` の `rm` がファイル未作成でエラー → `rm -f` に修正して実行。
- **原因 3**: `sudo bash docker_enter_jetson.sh` で `$HOME=/root` → sudo なしで実行、`WORK_DIR` を自動判定に修正。

### C-4. colcon build で `failed to create symbolic link`（11/18）
- **原因**: `--symlink-install` 有り（build_px.sh）と無し（colcon build）を同じ build/ で混ぜた。
- **対処**: `rm -rf build install log && ./build_px.sh`。以後は build_px.sh だけ使う。

### C-5. GPU 版 ONNX Runtime（make jetson-gpu）が作れない（11/18）
- `git clone` で `could not read Username` → tar.gz ダウンロードに変更 → `wget` が 404。
- 確認すると **microsoft/onnxruntime のリポジトリ自体が GitHub から消えていた**（API も Not Found。会社 Wi-Fi / テザリング / Mac すべて同じ。Hacker News でも報告あり）。
- **結論**: GPU 版 ORT はビルドしない。**TensorRT を直接使う方針に変更。** CPU 版イメージは中に ORT 1.14 があるので使える（作り直すと失敗するので `docker save` で保存）。

---

## D. ハードウェア（配線・モデル・キャリブレーション）

### D-1. 配線図（11/17）
- **回答**: PC を Jetson に置き換えるだけ。Arduino は USB で Jetson に接続し、サーボ・PIR の配線は今のまま（5V/GND → ブレッドボード、PIR→D2、サーボ1→D9、サーボ2→D10）。カメラは Jetson の USB-A。
- 理由: Jetson の GPIO は 3.3V、サーボのパルス生成は Arduino が得意、コード変更不要。

### D-2. 配線の強化（11/17）
- ネジ端子シールド、レバー式コネクタ（WAGO 221）/ ユニバーサル基板、JST-XH ロックコネクタ、USB のケーブルクランプ、可動部の余裕、サーボ用の別 5V 電源（2A 以上、GND 共通）。応急処置はホットボンド。

### D-3. yolo11.onnx にあたるファイル（11/17）
- 候補 1: `acceralation/YOLO11/training/yolo11_output/llvip/yolo11n_llvip_ir/weights/best.onnx`（LLVIP 赤外線、1 クラス）
- 候補 2: `acceralation/YOLO11/training/yolo11n.pt`（COCO 80 クラス、要 ONNX 書き出し）
- `px/.../yolov7Tiny_640_640.onnx` は出力形式が違うので不可。ORT 1.14 は opset 18 まで。
- **実際に Jetson に置いたモデル**: 出力 `[1,84,8400]` ＝ COCO 80 クラス版（10,741,320 bytes）。

### D-4. キャリブレーション用ライブラリ（11/18）
- スクリプト: `calibration/calibrate_pir_camera.py`（OpenCV 画面）、`dual_calibrate.py`（tkinter）、`pir_telemetrix_validate.py`（シリアル）。
- apt: `python3-opencv python3-tk python3-numpy`、pip: `telemetrix pyserial`。コンテナの PX を止めてから、画面のある環境（モニタ / VNC）で実行。

---

## E. GPU 化（計測の仕組み・TensorRT）

### E-1. ONNX Runtime の GPU 対応コード（11/17、px_gpu_patch.zip）
- `ort_provider`（tensorrt / cuda / cpu、自動フォールバック）、TensorRT エンジンのキャッシュ、起動時ウォームアップ、1 秒ごとの timing ログ。`Dockerfile.jetson.gpu`（ORT 1.20.1 をソースビルド）。
- → C-5 のとおり ORT が入手できず、GPU 版 ORT は未使用。

### E-2. 計測の仕組み（11/17、px_bench_patch.zip）
- **prompt**: 1（CPU 基準値）用、2・3（GPU / FP16 比較）用のスクリプトを作って。2・3 と 4 は同時がいいか？
- **回答**: **2・3 と 4 は別々に**（比較の基準が要る、不具合の切り分け、4 の方針は 3 の結果で決まる）。
  - px2 に `timing_csv`（1 フレーム 1 行）。
  - `bench_yolo`（ROS 不要、同じ画像で N 回推論）。
  - `bench/1_baseline_cpu.sh`、`2_3_gpu_compare.sh`、`convert_trt.sh`（trtexec、独立）、`host_monitor.sh`（電源モード固定 + tegrastats）、`summarize.py`。

### E-3. TensorRT を直接使う実装（11/18、px_trt_patch.zip）
- `trt_engine.cpp`（TensorRT 10 API。.engine を読み込み、無ければ ONNX から作って保存）。
- `PROVIDER=trt`（CPU 版イメージのまま動く）、`TRT_FP16`、`TRT_ENGINE`。
- `bench_yolo --provider trt --engine ...`、内訳 h2d / gpu / d2h を記録。
- `bench/2_3_trt_compare.sh`。CMake は TensorRT が見つかったときだけ有効（`px2: TensorRT enabled` を確認）。
- **px_trt_patch.zip が最新で、px_gpu_patch / px_bench_patch の内容をすべて含む。**

### E-4. `malformed launch argument 'timing_csv:='`（11/18）
- **原因**: ros2 launch は空の値を受け付けない。→ launch_px.sh を値があるときだけ渡すよう修正。

### E-5. カメラが動かない・大量の `Skipping tactic ... insufficient memory`（11/18）
- **原因**: px2 は最初に TensorRT エンジンを作る（数分）→ その後にカメラが動く。さらに **Ctrl+Z でプロセスが一時停止のまま残り**、カメラ・シリアル・メモリを使い続けていた。
- **対処**: Ctrl+C で止める、`kill -9 %1 %2`。エンジンは `bench_yolo` で先に作る。メモリは `drop_caches` で空ける。

### E-6. 計測結果の分析（11/18）
- px2 が作ったエンジン: GPU 17.8 ms（遅い）。trtexec: FP16 GPU 5.1 ms、FP32 8.6 ms。
- **trtexec のエンジンを px2 で読むと GPU 5.4 ms、合計 10.3 ms（97 FPS）** → 遅さの原因は px2 がメモリ不足の中で作ったエンジン。
- 実機は `TRT_ENGINE=/ros2_ws/work/engines/yolo11_fp16.engine PROVIDER=trt ./launch_px.sh`。

---

## F. 現在の状態と次の作業（11/18 10:00 時点）

- **確認済み**: Jetson の環境、リモート操作、パッチ前の CPU 版 PX の実機動作、オフラインの推論計測（CPU / TensorRT）。
- **未確認**: **パッチ後の PX の実機動作（cpu / trt とも）**、CPU 基準値（Step 1）。
- **次の順番**:
  1. `PROVIDER=cpu ./launch_px.sh` で実機の動作確認
  2. `TRT_ENGINE=... PROVIDER=trt ./launch_px.sh` で実機の動作確認
  3. `TAG=cpu_base ./bench/1_baseline_cpu.sh`
  4. `TAG=trt_day1 BASELINE=cpu_base ./bench/2_3_trt_compare.sh`（engines/ のエンジンを自動で使うよう修正済み。px_trt_patch.zip を再展開）
  5. Step 4: 前処理の GPU 化、CPU⇔GPU コピーの削減、INT8

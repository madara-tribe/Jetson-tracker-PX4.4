# 03. コマンド集 — PX4（Jetson Orin Nano）

用途別にまとめたコマンド（2026-11-18、PX4.4 の構成に合わせて更新）。
**どこで実行するか**を各見出しに書いている：

- 🖥 **Mac**：手元の Mac
- 🟩 **Jetson**：Jetson 本体（コンテナの外）
- 🐳 **コンテナ**：`./Docker/docker_enter.sh` で入ったコンテナ内（`/ros2_ws/work` = `SeamlessTrack/`）

```bash
# Jetson 上のリポジトリの場所（以降 $REPO）
REPO=~/PX4
```

---

## 0. よく使う流れ

```bash
# 🖥 Mac → Jetson にログイン（どのネットワークでも可）
ssh hagi@100.103.158.11

# 🟩 Jetson：性能を最大に固定（再起動のたびに 1 回）
cd $REPO && ./scripts/perf_mode.sh

# 🟩 Jetson：コンテナに入る
./Docker/docker_enter.sh

# 🐳 コンテナ：ビルド → 起動
./build_px.sh && source install/setup.bash
./kill_camera_users.sh
PROVIDER=trt ./launch_px.sh        # TensorRT FP16（engines/ のエンジンを自動で使う）
# 止めるのは Ctrl+C（Ctrl+Z は使わない）
```

---

## 1. スクリプトの一覧（用途別）

| 用途 | スクリプト | 実行場所 |
|---|---|---|
| Jetson の初期設定（時刻・権限・udev） | `scripts/jetson_setup.sh` | 🟩 |
| デバイス・イメージ・モデルの確認 | `scripts/check_devices.sh` | 🟩 |
| 性能を最大に固定（+ メモリを空ける） | `scripts/perf_mode.sh [--free-mem]` | 🟩 |
| Docker イメージの保存・復元 | `scripts/image_backup.sh save\|load` | 🟩 |
| ONNX Runtime をイメージから取り出す | `scripts/export_ort_from_image.sh` | 🟩 |
| キャリブレーション用ライブラリ | `scripts/install_calib_deps.sh` | 🟩 |
| カメラの udev | `Docker/pir_setup_udev/setup_udev.sh` | 🟩（sudo） |
| Docker イメージを作る | `cd Docker && make` | 🟩 |
| コンテナに入る | `Docker/docker_enter.sh` | 🟩 |
| ビルド | `SeamlessTrack/build_px.sh` | 🐳 |
| 起動 | `SeamlessTrack/launch_px.sh` | 🐳 |
| カメラを掴んでいるプロセスを止める | `SeamlessTrack/kill_camera_users.sh` | 🐳 / 🟩 |
| TensorRT エンジンを作る | `SeamlessTrack/make_engines.sh` | 🐳 |
| 計測 | `SeamlessTrack/bench/*.sh` | 🐳（`host_monitor.sh` は 🟩） |
| CI と同じチェック | `ci_checks/run_local.sh [--fix]` | 🖥 |

---

## 2. Jetson 本体の初期設定（1 回だけ）🟩

```bash
cat /etc/nv_tegra_release        # R36 = JetPack 6 系
cd $REPO
./scripts/jetson_setup.sh        # 終わったら一度ログアウトして入り直す
./scripts/check_devices.sh
sudo pip3 install jetson-stats   # 任意: jtop で GPU・温度を見る
```

ファームウェアの確認（SD カード作成前）: 電源を入れて NVIDIA ロゴで Esc 連打 → UEFI の版が 36.0 より新しければ JetPack 6.2.1 の SD をそのまま使う。

---

## 3. SSH・Tailscale・VNC

### 3-1. SSH 🟩 / 🖥
```bash
# 🟩
sudo systemctl status ssh
ip -4 a show wlP1p1s0 | grep inet
# 🖥 鍵でログイン
ls ~/.ssh/id_ed25519.pub || ssh-keygen -t ed25519
ssh-copy-id hagi@100.103.158.11
```

### 3-2. Tailscale（IP を固定する代わり）🟩
```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up                 # DGX・Mac と同じアカウントでログイン
sudo tailscale set --hostname=ori # 任意
tailscale ip -4                   # 100.103.158.11
```
管理画面 https://login.tailscale.com/admin/machines で **Disable key expiry**。

### 3-3. VNC（起動と同時に x11vnc）🟩
```bash
sudo apt install -y x11vnc && x11vnc -storepasswd
# 自動ログイン: /etc/gdm3/custom.conf の [daemon] に
#   AutomaticLoginEnable=true
#   AutomaticLogin=hagi
sudo tee /etc/systemd/system/x11vnc.service >/dev/null <<'EOF'
[Unit]
Description=x11vnc for desktop :0 (localhost only, use SSH tunnel)
After=display-manager.service network-online.target
Wants=network-online.target

[Service]
User=hagi
ExecStartPre=/bin/sh -c 'for i in $(seq 1 60); do [ -e /run/user/1000/gdm/Xauthority ] && exit 0; sleep 2; done; exit 1'
ExecStart=/usr/bin/x11vnc -display :0 -auth /run/user/1000/gdm/Xauthority -rfbauth /home/hagi/.vnc/passwd -localhost -forever -shared -noxdamage -o /home/hagi/x11vnc.log
Restart=always
RestartSec=5

[Install]
WantedBy=graphical.target
EOF
sudo systemctl daemon-reload && sudo systemctl enable --now x11vnc
ss -ltnp | grep 5900              # 127.0.0.1:5900
export DISPLAY=:0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
gsettings set org.gnome.desktop.screensaver lock-enabled false
gsettings set org.gnome.desktop.session idle-delay 0
```

### 3-4. Mac 側 🖥
```bash
cat >> ~/.ssh/config <<'EOF'

Host ori
    HostName 100.103.158.11
    User hagi
    LocalForward 15901 127.0.0.1:5900
    ServerAliveInterval 30
EOF
ssh ori                           # ログイン
ssh -N ori                        # 端末A: VNC 用トンネル
open vnc://127.0.0.1:15901        # 端末B: 画面
pkill -f "ssh -N ori"             # 終了
```

### 3-5. つながらないとき 🖥
```bash
tailscale status | grep -E "hagi-desktop|ori"   # offline なら Jetson の電源・ネットを確認
ssh hagi@192.168.55.1                           # USB-C 直結（ネットワーク不要）
# ssh が固まったら: Enter → ~ → .
```

---

## 4. 重いファイルの転送 🖥

git に入れていないファイルは、手元から cp / scp する。

| ファイル | 置き場所（リポジトリからの相対パス） |
|---|---|
| `yolo11.onnx`（必須、約 10 MB） | `SeamlessTrack/src/px2/weights/yolo11.onnx` |
| `yolo11_fp16.engine`（約 8 MB） | `SeamlessTrack/engines/yolo11_fp16.engine` |
| `yolo11_fp32.engine`（約 12 MB） | `SeamlessTrack/engines/yolo11_fp32.engine` |
| `onnxruntime-1.14.1-aarch64.tgz` | `Docker/third_party/onnxruntime-1.14.1-aarch64.tgz` |

```bash
scp yolo11.onnx ori:~/PX4/SeamlessTrack/src/px2/weights/yolo11.onnx
scp yolo11_fp16.engine yolo11_fp32.engine ori:~/PX4/SeamlessTrack/engines/

# リポジトリを同期（生成物と重いファイルは送らない）
rsync -av --exclude build --exclude install --exclude log --exclude trt_cache \
  --exclude '*.engine' --exclude '*.onnx' --exclude results \
  PX4/ ori:~/PX4/

# 結果を持ってくる
scp -r ori:~/PX4/SeamlessTrack/bench/results ./px_results
```

🟩 同じ Jetson の中で古いフォルダからコピーする場合:
```bash
OLD=~/Downloads/place/SensorFireld-PX4.3-night-pattern-C/SeamlessTrack
cp $OLD/src/px2/weights/yolo11.onnx $REPO/SeamlessTrack/src/px2/weights/
cp $OLD/engines/yolo11_fp16.engine $OLD/engines/yolo11_fp32.engine $REPO/SeamlessTrack/engines/
```

---

## 5. Docker 🟩

```bash
cd $REPO
./scripts/image_backup.sh save            # まずイメージを保存（作り直せないことがあるため）
./scripts/export_ort_from_image.sh        # ONNX Runtime を Docker/third_party/ に取り出す
cd Docker && make                         # px-jetson:latest（sudo なし）
cd $REPO && ./Docker/docker_enter.sh      # コンテナに入る
```
- 既存の `px-jetson:latest` はそのまま使える（作り直しは Dockerfile を変えたときだけ）。
- コードの場所は自動で判定（`Docker/` の隣の `SeamlessTrack/`）。
- 復元: `./scripts/image_backup.sh load`

---

## 6. カメラ・Arduino 🟩

```bash
./scripts/check_devices.sh
sudo ./Docker/pir_setup_udev/setup_udev.sh   # /dev/video/pir_camera が無いとき
v4l2-ctl --list-devices
```

---

## 7. ビルド 🐳

```bash
./build_px.sh                          # 常にこれを使う（--symlink-install 付き）
source install/setup.bash
grep -h "px2: TensorRT" log/latest_build/px2/stdout*.log   # TensorRT enabled
ls install/px2/lib/px2/                # px2, bench_yolo
rm -rf build install log && ./build_px.sh   # symlink エラーなどでおかしくなったら
```

---

## 8. PX を動かす 🐳

```bash
./kill_camera_users.sh
PROVIDER=cpu ./launch_px.sh                    # CPU
PROVIDER=trt ./launch_px.sh                    # TensorRT FP16
PROVIDER=trt TRT_FP16=false ./launch_px.sh     # TensorRT FP32
```

| 変数 | 意味 | 既定 |
|---|---|---|
| `PROVIDER` | `trt` / `cpu` | `trt` |
| `TRT_FP16` | `true`=FP16 / `false`=FP32 | `true` |
| `TRT_ENGINE` | 使う .engine（空なら `engines/yolo11_fp16.engine` か `fp32` を自動で使う） | 空 |
| `SAVE_FRAMES` | 検出したフレームを `frames/`（`SAVE_DIR`）に PNG で保存する（1 フレーム 1 枚。ループが遅くなる） | `false` |
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

### PIR を使わずに確認（別の端末から）
```bash
# 🟩
docker exec -it $(docker ps -q --filter ancestor=px-jetson:latest | head -1) bash
# 🐳（入った先）
source /opt/ros/humble/setup.bash && source /ros2_ws/work/install/setup.bash
ros2 topic echo /pir/state                         # 200 ms ごとに今の値が出る。手を振って true に変われば PIR は正常
ros2 topic pub --once --qos-durability transient_local --qos-reliability reliable \
  /px_wakeup std_msgs/msg/Bool "{data: true}"      # DETECT に強制的に入れる
```

### 止める・後片付け
```bash
# Ctrl+Z で止めてしまったら
jobs -l
kill -9 %1 %2 2>/dev/null; pkill -9 -f "px2|watch_node|ros2 launch"
```

---

## 9. TensorRT エンジンを作る

```bash
# 🟩 先に性能最大 + メモリを空ける（VNC・ブラウザも閉じる）
./scripts/perf_mode.sh --free-mem
# 🐳 FP32 と FP16 を作り、速度も測る（約 10 分）
./make_engines.sh
cat engines/summary.txt
```
エンジンは Jetson・TensorRT の版ごとに固有。JetPack を更新したら作り直す。

---

## 10. 計測

### 10-1. オフライン（カメラなし）🐳
```bash
B=install/px2/lib/px2/bench_yolo; M=src/px2/weights/yolo11.onnx
$B --model $M --provider cpu --iters 200
$B --model $M --provider trt --fp16 1 --engine /ros2_ws/work/engines/yolo11_fp16.engine --iters 200
$B --model $M --provider trt --fp16 0 --engine /ros2_ws/work/engines/yolo11_fp32.engine --iters 200
```

### 10-2. まとめて計測・比較
```bash
./SeamlessTrack/bench/host_monitor.sh cpu_base                 # 🟩 端末A（Ctrl+C で終了）
TAG=cpu_base ./bench/1_baseline_cpu.sh                          # 🐳 CPU の基準値
TAG=trt_day1 BASELINE=cpu_base ./bench/2_3_trt_compare.sh       # 🐳 TensorRT と比較
python3 bench/summarize.py bench/results/trt_day1 --baseline bench/results/cpu_base
```

### 10-3. 状態の監視 🟩
```bash
free -h; jtop; sudo tegrastats --interval 500; sudo nvpmodel -q
```

---

## 11. キャリブレーション（コンテナの外）🟩

```bash
./scripts/install_calib_deps.sh
# PX を止めてから、モニタか VNC の画面で
python3 calibration/pir_telemetrix_validate.py     # PIR（SSH でも可）
python3 calibration/dual_calibrate.py              # サーボ（画面が必要）
python3 calibration/calibrate_pir_camera.py        # カメラ（画面が必要）
# カメラの内部パラメータを作り直すとき: camera_calib_kit/
```

---

## 12. CI 🖥

```bash
pip install clang-format==14.0.6     # CI（ubuntu-22.04）と同じ版
./ci_checks/run_local.sh             # チェックだけ
./ci_checks/run_local.sh --fix       # 整形を自動で直してからチェック
```

---

## 13. トラブル対応の早見表

| 症状 | 対処 |
|---|---|
| IP が分からない / 変わった | 🖥 `ssh hagi@100.103.158.11` |
| `permission denied ... docker.sock` | 🟩 `./scripts/jetson_setup.sh` → ログインし直す |
| `/dev/video/pir_camera not found` | 🟩 `sudo ./Docker/pir_setup_udev/setup_udev.sh` |
| colcon の symlink エラー | 🐳 `rm -rf build install log && ./build_px.sh` |
| カメラが使用中 | 🐳 `./kill_camera_users.sh` |
| 起動しても何も起きない | PIR が反応していない → 8 章の「PIR を使わずに確認」、配線は docs/WIRING.md |
| 起動してもサーボが中央に動かない | px2 の初期化（カメラ、モデル、エンジン）で失敗すると px3 が作られない → px2 の起動ログで `Failed to open camera`、`model not found`、`[trt]` のエラーを探す。px3 の起動（4 秒待ち）はエンジンを読み終わったあとに始まる |
| ループが約 15 Hz しか出ない（`loop_ms` が約 66 ms） | カメラがバッファ 1 枚で 1 フレームおきになっている → `CAP_BUFFER=2 ./launch_px.sh` と比べる |
| `model not found` / `Calibration not loaded` | `./build_px.sh` をやり直す（`install/px2/share/px2/weights/` に置かれる）。または `MODEL_PATH=...` `CALIB_PATH=...` で場所を指定 |
| `Skipping tactic ... insufficient memory` | 🟩 `./scripts/perf_mode.sh --free-mem` → 🐳 `./make_engines.sh` |
| TensorRT が使えず CPU に落ちる | 🟩 `docker info \| grep -i runtime`（nvidia）、🐳 ビルドログの `px2: TensorRT enabled` |
| `make` で ONNX Runtime のダウンロードが 404 | 🟩 `./scripts/export_ort_from_image.sh` を先に実行 |

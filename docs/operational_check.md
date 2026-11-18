# PX4.4 移行後の動作確認手順（2026-11-18 実施・確認済み）

PX4.3 のフォルダ（`~/Downloads/place/SensorFireld-PX4.3-night-pattern-C`）から PX4 リポジトリに移したときの確認手順。
CPU / TensorRT FP16 / FP32 の 3 モードで動作を確認した。

```bash
# 🟩 Jetson：重いファイルを旧フォルダからコピー
cd ~/PX4
OLD=~/Downloads/place/SensorFireld-PX4.3-night-pattern-C/SeamlessTrack
cp $OLD/src/px2/weights/yolo11.onnx SeamlessTrack/src/px2/weights/
cp $OLD/engines/yolo11_fp16.engine $OLD/engines/yolo11_fp32.engine SeamlessTrack/engines/

# 🟩 イメージの保存と ONNX Runtime の取り出し（1 回）
./scripts/image_backup.sh save
./scripts/export_ort_from_image.sh

# 🟩 確認と性能モード
./scripts/check_devices.sh
./scripts/perf_mode.sh

# 🟩 コンテナに入る
./Docker/docker_enter.sh

# 🐳 コンテナ内：ビルドして 3 モードで起動（それぞれ Ctrl+C で止める）
rm -rf build install log && ./build_px.sh && source install/setup.bash
./kill_camera_users.sh
PROVIDER=cpu ./launch_px.sh
PROVIDER=trt ./launch_px.sh                   # 「INFO: TensorRT engine = .../engines/yolo11_fp16.engine」
PROVIDER=trt TRT_FP16=false ./launch_px.sh    # fp32 のエンジンが選ばれる
```

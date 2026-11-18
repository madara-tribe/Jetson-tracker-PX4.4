# 🖥 Mac → Jetson
scp SensorFireld-PX4.4.zip ori:~/
ssh ori

# 🟩 Jetson：展開して、重いファイルをコピー
cd ~ && unzip -o SensorFireld-PX4.4.zip && cd ~/SensorFireld-PX4.4
OLD=~/Downloads/place/SensorFireld-PX4.3-night-pattern-C/SeamlessTrack
cp $OLD/src/px2/weights/yolo11.onnx SeamlessTrack/src/px2/weights/
cp $OLD/engines/yolo11_fp16.engine $OLD/engines/yolo11_fp32.engine SeamlessTrack/engines/
chmod +x scripts/*.sh Docker/*.sh SeamlessTrack/*.sh SeamlessTrack/bench/*.sh ci_checks/*.sh

# 🟩 イメージの保存と ONNX Runtime の取り出し（今のうちに1回）
./scripts/image_backup.sh save
./scripts/export_ort_from_image.sh

# 🟩 確認と性能モード
./scripts/check_devices.sh
./scripts/perf_mode.sh

# 🟩 コンテナに入る（今の px-jetson:latest をそのまま使う。作り直しは不要）
./Docker/docker_enter.sh

# 🐳 コンテナ内：ビルドして、3つのモードで起動を確認（それぞれ Ctrl+C で止める）
rm -rf build install log && ./build_px.sh && source install/setup.bash
./kill_camera_users.sh
PROVIDER=cpu ./launch_px.sh
PROVIDER=trt ./launch_px.sh                   # 「INFO: TensorRT engine = .../engines/yolo11_fp16.engine」が出る
PROVIDER=trt TRT_FP16=false ./launch_px.sh    # fp32 のエンジンが選ばれる

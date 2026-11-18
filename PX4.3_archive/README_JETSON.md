# PX on Jetson Orin Nano (JetPack 6.2.x)

Host: JetPack 6.2.x, L4T R36 (`cat /etc/nv_tegra_release`), Ubuntu 22.04 jammy.

## Added files
- `Docker/Dockerfile.jetson` — l4t-jetpack r36.4.0 + ROS 2 Humble + ONNX Runtime 1.14.1 (aarch64, CPU)
- `Docker/docker_enter_jetson.sh` — starts the container (works over SSH)
- `SeamlessTrack/build_px.sh` — colcon build inside the container
- `Docker/Makefile` — new target `make jetson` (old `make run` kept for x86)

## Steps (on the Jetson, over SSH)
```bash
sudo usermod -aG docker $USER          # then log out / in once
cd ~/SensorFireld-PX4.3-night-pattern-C/Docker
sudo ./pir_setup_udev/setup_udev.sh    # once; creates /dev/video/pir_camera
make jetson                            # build image (~20-40 min, ~10 GB)

# copy the real model (the repo file is 0 bytes):
#   SeamlessTrack/src/px2/weights/yolo11.onnx

./docker_enter_jetson.sh               # WORK_DIR defaults to ~/SensorFireld-PX4.3-night-pattern-C/SeamlessTrack
# inside the container (/ros2_ws/work):
./build_px.sh
source install/setup.bash
./launch_px.sh                         # run inside tmux
```

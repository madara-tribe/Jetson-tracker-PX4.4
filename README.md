# PX4 — Night-time Human Tracking on Jetson Orin Nano

A night-time human tracking turret running on a Jetson Orin Nano.
When the PIR sensor detects motion, YOLO11 finds the person in the IR camera image
and two servos (pan / tilt) driven by an Arduino keep the camera pointed at them.

- Inference runs on **CPU (ONNX Runtime)** or **GPU (TensorRT FP16 / FP32)**, selectable at launch
- ROS 2 Humble runs inside a Docker container
- Wiring: [docs/WIRING.md](docs/WIRING.md)

| Backend | Time per frame (offline benchmark) |
|---|---|
| CPU (ONNX Runtime 1.14) | ~88 ms (warm-up inference) |
| TensorRT FP32 | 8.6 ms GPU compute |
| TensorRT FP16 | 5.4 ms GPU compute, 10.3 ms end-to-end incl. pre/post-processing (~97 FPS) |

Details: [docs/02_experiment_data.md](docs/02_experiment_data.md)

---

## Repository layout

```
Docker/            Docker image (Dockerfile, docker_enter.sh, Makefile, pir_setup_udev/)
SeamlessTrack/     ROS 2 workspace (mounted at /ros2_ws/work in the container)
  src/             Core packages: px2 (inference + control), px3 (servo + PIR), px3_launch, custom_msgs
  build_px.sh      Build
  launch_px.sh     Launch
  make_engines.sh  Build TensorRT engines
  bench/           Inference benchmarks
  engines/         TensorRT engines (not tracked by git)
scripts/           Host-side helpers for the Jetson (setup, performance mode, image backup, ...)
calibration/       PIR / servo / camera checks and calibration (run on the Jetson host)
camera_calib_kit/  Tools to generate the camera intrinsics (calib_result_ir.yaml)
ci_checks/         CI checks (run_local.sh runs them locally)
docs/              Wiring, build log, experiment data, command reference
PX4.3_archive/     Files no longer used since PX4.3
```

---

## Requirements

| Item | Value |
|---|---|
| Board | Jetson Orin Nano Developer Kit (8 GB) |
| JetPack | 6.2.x (L4T R36, Ubuntu 22.04) |
| Docker image | `px-jetson:latest` — l4t-jetpack r36.4.0 + ROS 2 Humble + OpenCV 4.5.4 + ONNX Runtime 1.14.1 (CPU) + TensorRT 10.3 |
| Camera | ELP IR USB camera (32e4:9230), 1920x1080 @ 30 fps |
| Microcontroller | Arduino running Telemetrix, USB serial `/dev/ttyACM0` |
| Sensors / actuators | PIR sensor (SB612B), 2 servos (pan / tilt) |

---

## Setup (first time only)

### 1. Prepare the Jetson
```bash
git clone <this repo> ~/PX4 && cd ~/PX4
./scripts/jetson_setup.sh        # timezone, Docker / serial / camera permissions, camera udev rule
# log out and log back in
./scripts/check_devices.sh       # check camera, Arduino and permissions
```

### 2. Copy the large files
They are not in git. Copy them from your local machine (see [Large files](#large-files)).
At minimum you need **`SeamlessTrack/src/px2/weights/yolo11.onnx`**.

### 3. Build the Docker image
```bash
cd Docker && make                # builds px-jetson:latest
```
ONNX Runtime 1.14.1 can no longer be downloaded from GitHub, so the build needs
`Docker/third_party/onnxruntime-1.14.1-aarch64.tgz`.
If you already have the image, extract it with `./scripts/export_ort_from_image.sh`.

### 4. Build the workspace (inside the container)
```bash
./Docker/docker_enter.sh         # enter the container (/ros2_ws/work = SeamlessTrack/)
./build_px.sh
source install/setup.bash
```

### 5. Build TensorRT engines (GPU only, once)
```bash
# On the Jetson host (another terminal): max performance + free memory
./scripts/perf_mode.sh --free-mem
# In the container: build FP32 and FP16 engines (~10 min)
./make_engines.sh                # -> engines/yolo11_fp16.engine, yolo11_fp32.engine
```

---

## Usage

```bash
# On the Jetson host (once after each reboot)
./scripts/perf_mode.sh

# Enter the container
./Docker/docker_enter.sh

# In the container
source install/setup.bash
PROVIDER=cpu ./launch_px.sh                   # CPU
PROVIDER=trt ./launch_px.sh                   # GPU: TensorRT FP16 (default)
PROVIDER=trt TRT_FP16=false ./launch_px.sh    # GPU: TensorRT FP32
```

- Stop with **Ctrl+C**. Do not use Ctrl+Z: the suspended process keeps the camera and serial port open.
- The system waits (WATCH) until the PIR sensor fires, then switches to detection and tracking (DETECT).
- Detection images are saved to `SeamlessTrack/install/px2/share/px2/data/`.
- The log shows `px2: inference provider = trt` and a per-second `px2[timing] ... infer=...ms` line.

| Variable | Meaning | Default |
|---|---|---|
| `PROVIDER` | `trt` (TensorRT) / `cpu` (ONNX Runtime) | `trt` |
| `TRT_FP16` | `true` = FP16 / `false` = FP32 | `true` |
| `TRT_ENGINE` | Engine file to use (empty = pick from `engines/` automatically) | empty |
| `SAVE_FRAMES` | Save detection images | `true` |
| `TIMING_CSV` | Write per-frame timings to a CSV file | empty |

To test the camera, inference and servos without the PIR sensor, see "Checking the wiring" in [docs/WIRING.md](docs/WIRING.md).

---

## Benchmarks

```bash
./SeamlessTrack/bench/host_monitor.sh cpu_base                 # Jetson host
TAG=cpu_base ./bench/1_baseline_cpu.sh                          # container: CPU baseline
TAG=trt_day1 BASELINE=cpu_base ./bench/2_3_trt_compare.sh       # container: compare with TensorRT
```
See [SeamlessTrack/bench/README.md](SeamlessTrack/bench/README.md).

---

## Large files

Not tracked by git. Copy them from your local machine.

| File | Location | Size | How to get it |
|---|---|---|---|
| `yolo11.onnx` | `SeamlessTrack/src/px2/weights/` | ~10 MB | **Required.** YOLO11n (output `[1,84,8400]`, 80 COCO classes) |
| `yolo11_fp16.engine` | `SeamlessTrack/engines/` | ~8 MB | `./make_engines.sh` (rebuild per device and TensorRT version) |
| `yolo11_fp32.engine` | `SeamlessTrack/engines/` | ~12 MB | Same as above |
| `onnxruntime-1.14.1-aarch64.tgz` | `Docker/third_party/` | a few MB | `./scripts/export_ort_from_image.sh` (only needed to rebuild the image) |
| `px-jetson_latest.tar.gz` | `~/` (outside the repo) | a few GB | `./scripts/image_backup.sh save` (image backup) |

---

## Remote access

Use Tailscale + SSH to reach the Jetson from any network; VNC goes through an SSH tunnel.
See the "SSH / Tailscale / VNC" section of [docs/03_commands.md](docs/03_commands.md).

---

## CI

| Workflow | What it checks |
|---|---|
| `cpp-ci` | clang-format (v14) on `SeamlessTrack/src` |
| `python-lint` | ruff format / lint and unit tests |
| `px-parameter-contract` | Launch-file and C++ parameters stay in sync (`max_step_deg` is excluded because it is tuned often) |
| `colcon-build` | ROS 2 build. **Manual trigger only**, because ONNX Runtime can no longer be downloaded from GitHub |

Run the same checks locally: `./ci_checks/run_local.sh` (`--fix` applies formatting). Requires uv and clang-format 14 (`pip install clang-format==14.0.6`).

---

## Documentation

| File | Contents |
|---|---|
| [docs/WIRING.md](docs/WIRING.md) | Wiring diagram and pin tables |
| [docs/01_conversation_log.md](docs/01_conversation_log.md) | Build log: what was done, what went wrong, how it was fixed |
| [docs/02_experiment_data.md](docs/02_experiment_data.md) | Environment, benchmark results, error list |
| [docs/03_commands.md](docs/03_commands.md) | Command reference by task |
| [PX4.3_archive/README.md](PX4.3_archive/README.md) | Files moved out in PX4.4 and how to restore them |

The documents in `docs/` are written in Japanese.

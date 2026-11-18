# PX4.3_archive

PX4.4（Jetson Orin Nano 版）で使わなくなったものを、捨てずにここへまとめたもの（2026-11-18）。
ビルドや CI の対象外（ruff は `pyproject.toml` で除外）。

| もの | 元の場所 | 移した理由 | 戻すとき |
|---|---|---|---|
| `simulation/` | `simulation/` | 今は使わない（MuJoCo のシミュレーション） | 強化学習（sim2real）を始めるとき。`.github/workflows/sim-hil.yaml` と `ci_checks/tests/test_sim_hil_param_contract.py`、`pyproject.toml` の `simulation` グループ（mujoco, numpy, matplotlib, opencv-python-headless）と一緒に戻す |
| `.github/workflows/sim-hil.yaml` | `.github/workflows/` | simulation 用の CI | 同上 |
| `ci_checks/tests/test_sim_hil_param_contract.py` | `ci_checks/tests/` | simulation のパラメータとの突き合わせ | 同上（pyyaml も依存に戻す） |
| `fixed_reports/` | `fixed_reports/` | 過去の調査・修正レポート | 参照用 |
| `Docker/Dockerfile` | `Docker/Dockerfile` | x86 PC（Ubuntu 20.04 / ROS 2 Foxy）用 | PC で動かすとき |
| `Docker/docker_enter.sh` | `Docker/docker_enter.sh` | x86 PC 用の起動スクリプト | 同上 |
| `Docker/install_*.bash`、`ros2_setup_scripts_ubuntu.sh` | `Docker/` | x86 の Dockerfile からだけ使われていた | 同上 |
| `Docker/brio_setup_udev/` | `Docker/` | 昼間版（BRIO / RealSense）のカメラ用 | 昼間版を動かすとき |
| `Docker/Dockerfile.jetson.gpu` | `Docker/` | GPU 版 ONNX Runtime 1.20.1 をビルドする。ソース（microsoft/onnxruntime）が GitHub から消えて作れない | ONNX Runtime が再び入手できたら |
| `SeamlessTrack/bench/2_3_gpu_compare.sh` | `SeamlessTrack/bench/` | 上の GPU 版 ONNX Runtime 用の計測 | 同上 |
| `SeamlessTrack/setup_serial.sh` | `SeamlessTrack/` | `sudo chmod 777` の一時しのぎ。コンテナは `--privileged` と `--group-add` で起動するので不要 | シリアルやカメラの権限エラーが出たとき |
| `README_JETSON.md` | トップ | `README.md` に統合 | — |
| `check_run.sh` | トップ | `ci_checks/run_local.sh` に置き換え | — |

PX4.4 での名前の変更（archive ではなく改名）:
- `Docker/Dockerfile.jetson` → `Docker/Dockerfile`
- `Docker/docker_enter_jetson.sh` → `Docker/docker_enter.sh`
- `SeamlessTrack/bench/convert_trt.sh` → `SeamlessTrack/make_engines.sh`
- Makefile のターゲット `make jetson` → `make`（`make build`）

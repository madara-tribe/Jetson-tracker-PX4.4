# simulation/ — Pattern-C MuJoCo Simulation (CI integration)

Pattern-B用に構築した `px-mujoco-main` の資産をPattern-C（Night, PIR + IR camera
+ YOLO11）用に移植したものです。`sim-hil.yaml` CIワークフローから呼び出します。

## Pattern-Bからの主な変更点

| 項目 | Pattern-B | Pattern-C |
|------|-----------|-----------|
| launch起動スクリプト | `run.sh` | `launch_px.sh` |
| kp_yaw/kd_yaw/kp_pitch/kd_pitch/max_step_deg/conf_threshold | launchデフォルトのみ | `launch_px.sh`で明示的にオーバーライド |
| モデル(ONNX) | yolov7Tiny_640_640.onnx | yolo11.onnx |
| モデルファイル名 | `models/pattern_b_integrated.xml` | `models/pattern_c_integrated.xml` |

`axis_pd_controller.hpp` / `px3_servo_node.h` はPattern-Bと完全に同一のため、
制御ロジック自体のテスト（Level3/Level4）はコード変更なしで流用しています。

## ⚠️ 既知の未解決事項（要フォローアップ）

**`camera_calib.yaml` はIRカメラで再キャリブレーションされていません。**

`SeamlessTrack/src/px2/weights/calib_result.yaml` の中身を確認したところ、
Pattern-B（BRIO/RGB）のキャリブレーション結果と完全に一致していました
（同じ `calibration_date`, 同じ `captures/img_*.png` 一覧）。ELPのIRカメラで
撮り直したキャリブレーションではなく、BRIOの値が暫定的に転用されている
可能性が高いです。

加えて、`launch_px.sh` は `active_width:=1920 / active_height:=1080` で
カメラを起動していますが、`calib_result.yaml` の `image_width/height` は
`1280x720` であり、**解像度が一致していません**（この不一致自体は
Pattern-Bのrun.shにも同様に存在するため、Pattern-C固有の新しい問題では
なく、両ブランチに共通する既存の課題です）。

現状のCIはこの解像度不一致を検知対象にしていません
（`camera_calib.yaml`は`test_sim_hil_param_contract.py`の契約チェック対象外）。
実際のIRカメラでの再キャリブレーションが完了次第、`camera_calib.yaml`を
差し替えることを推奨します。

**`models/parts/camera.xml` の質量・寸法もBRIO実測値のままです。**

ELP-USBFHD05MT-KL36IR（ボードタイプ）の実測値が未取得のため、
BRIOカメラの実測値（70x30x30mm, 50g）を暫定的に流用しています。
PD制御・joint limitの検証自体への影響は小さいと考えていますが、
正確な値で検証したい場合は実測して差し替えてください。

## ローカルでの実行方法

```bash
cd simulation
pip install -r requirements.txt

# CIのゲート対象（CIと同じ判定基準）
python scripts/v3/run_level3_tracking_tests.py
python scripts/v4/run_level4_fsm_tests.py
```

Level1/2（`run_level1_static.py`, `run_level2_sweep.py`）はCI対象外です。
test1〜5（Level3/4）の前提となるactuator/jointの物理的成立を確認するための
スクリプトであり、必要に応じて手動実行してください。

## 検証区分・既知の制約について

`reports/01_behavior_source_reference.md` と `reports/04_future_problem_prediction.md`
はPattern-B向けに書かれた一次資料ですが、制御ロジック自体はPattern-Cと共通の
ため、そのまま参照可能です。

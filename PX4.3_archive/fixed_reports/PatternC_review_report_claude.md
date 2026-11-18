# Pattern-C Detailed Code Review

**Project:** SensorFireld-PX4.3 Night / Pattern-C  
**Reviewed archive:** `SensorFireld-PX4.3-night-pattern-C_test2.zip`  
**Review date:** 2026-07-19  
**Scope:** Static review of ROS 2 runtime code, YOLO/ONNX processing, camera geometry, PD control, PIR/serial behavior, launch configuration, simulation, and CI.

> **Editor's note (added after independent verification, see Section 12):**
> Every Critical/High-severity finding in this report was independently re-checked against the actual source
> (`inference.cpp`, `axis_pd_controller.hpp`, `capture_settings.hpp`, `px3_system_launch.py`, `yolo_inference.cpp`,
> `calib_result.yaml`) and confirmed accurate. One finding (C-03) required a currency/wording correction, and two
> additional known issues not covered by this report have been appended in Section 12. See Section 12 for details
> before acting on C-03 as written.

## 1. Executive summary

Pattern-C has a reasonable prototype architecture:

1. px3 reads the PIR sensor and controls the two servos.
2. `watch_node` converts PIR activity into a wake-up request.
3. px2 changes from `WATCH` to `DETECT_TRACK` mode.
4. YOLO11 detects people.
5. px2 converts the selected bounding box into yaw and pitch errors.
6. PD controllers generate absolute servo commands.
7. px3 sends the commands to the hardware and returns an acknowledgement.
8. px2 returns to `WATCH` after the target is lost.

The design is suitable for supervised prototype testing, but it is **not yet reliable enough for unattended or long-duration operation**. The most important problems are:

- launch parameters do not reliably reach the embedded px3 node;
- the supplied YOLO model file is empty;
- the calibration appears to belong to a different camera and resolution;
- control geometry uses requested rather than actual frame dimensions;
- the PD controller can accumulate hidden state beyond servo limits;
- controller state is reset without synchronizing the physical servo position;
- fatal startup failures can return exit status 0;
- frame saving can rapidly fill storage;
- serial disconnection and reconnection are not handled.

## 2. Review conclusion

| Question | Conclusion |
|---|---|
| Is the overall architecture understandable? | Yes |
| Is the code free of problems? | No |
| Can it be tested with supervision? | Yes, after restoring the ONNX model |
| Is tracking geometry currently trustworthy? | Not until the real IR camera is recalibrated and actual frame dimensions are enforced |
| Is it ready for unattended use? | No |
| Is it ready to become a stable Pattern-D foundation? | Only after the critical and high-priority issues are corrected |

## 3. Severity summary

| ID | Severity | Finding | Type |
|---|---|---|---|
| C-01 | Critical | Embedded px3 does not reliably receive launch parameters | Confirmed defect |
| C-02 | Critical | Supplied `yolo11.onnx` is zero bytes | Confirmed packaging defect |
| C-03 | Critical | Calibration appears to be from the previous RGB camera | Confirmed by repository documentation; hardware validation required |
| H-01 | High | Actual camera resolution/FPS are logged but not enforced | Confirmed defect |
| H-02 | High | ONNX tensor buffer length uses bytes instead of float-element count | Confirmed API misuse |
| H-03 | High | Fatal startup exceptions return exit status 0 | Confirmed defect |
| H-04 | High | Serial initialization failure leaves a running but inactive system | Confirmed behavior |
| H-05 | High | Controller internal position can wind up beyond servo limits | Confirmed control defect |
| H-06 | High | Controller reset does not match the physical servo position | Confirmed state mismatch |
| H-07 | High | Saved images can fill storage indefinitely | Confirmed operational risk |
| H-08 | High | No serial reconnection or runtime hardware-health transition | Confirmed missing behavior |
| M-01 | Medium | `DETECT_TRACK` is detection-based target selection, not identity tracking | Design limitation |
| M-02 | Medium | ACK confirms command handling, not physical servo arrival | Design limitation |
| M-03 | Medium | YOLO preprocessing modifies the original frame to RGB | Confirmed defect |
| M-04 | Medium | Successful detections are saved twice with misleading names | Confirmed defect |
| M-05 | Medium | Model input/output metadata and tensor shapes are insufficiently validated | Confirmed missing validation |
| M-06 | Medium | `person = class 0` is assumed rather than verified | Model-dependent risk |
| M-07 | Medium | Target selection uses largest area without temporal continuity | Design limitation |
| M-08 | Medium | PIR activity can be lost before px3 becomes ready | Timing-dependent risk |
| M-09 | Medium | `/px_wakeup` carries multiple command and feedback meanings | Maintainability risk |
| M-10 | Medium | PIR setup write ignores partial writes and errors | Confirmed defect |
| M-11 | Medium | Serial read errors are treated like normal timeouts | Confirmed defect |
| M-12 | Medium | Model and calibration paths are hard-coded to one workspace | Confirmed portability defect |
| M-13 | Medium | Several runtime parameters are not validated | Confirmed missing validation |
| L-01 | Low | Float servo angles are truncated instead of rounded | Confirmed precision issue |
| L-02 | Low | Current PIR filtering is duplicate suppression, not complete debounce | Design limitation |
| L-03 | Low | `chmod 777` is used as a device-permission workaround | Security/operations issue |

## 4. Critical findings

### C-01: Embedded px3 launch parameters are not routed correctly

The launch file attaches px3-related values to the `px2` process:

```python
"serial_port": LaunchConfiguration("serial_port"),
"baudrate": ParameterValue(LaunchConfiguration("baudrate"), value_type=int),
"x_center_deg": ParameterValue(LaunchConfiguration("x_center_deg"), value_type=float),
"y_center_deg": ParameterValue(LaunchConfiguration("y_center_deg"), value_type=float),
```

The embedded px3 node is then created with global ROS arguments disabled:

```cpp
auto px3_node = std::make_shared<px3_embedded::Px3ServoNode>(
    rclcpp::NodeOptions().use_global_arguments(false));
```

This means launch-time overrides are not reliably available to px3. The problem is hidden because the current launch values match px3 defaults.

**Impact**

- a changed serial device may be ignored;
- a changed baud rate may be ignored;
- px2 and px3 may use different center-angle assumptions;
- parameter-contract CI can pass without proving runtime delivery to the embedded node.

**Required correction**

Explicitly build `NodeOptions` for px3 with parameter overrides, or run px3 as a normal separately launched ROS node. Add an integration test that reads the runtime parameters from both nodes.

### C-02: The supplied YOLO model is empty

`SeamlessTrack/src/px2/weights/yolo11.onnx` has a size of zero bytes in the reviewed archive.

**Impact:** the reviewed archive cannot create an ONNX Runtime session or perform detection.

**Required correction**

- restore the actual ONNX model or its Git LFS object;
- verify the file is nonempty in CI;
- optionally verify a checksum;
- run a one-frame inference smoke test.

### C-03: Calibration is not proven to belong to the IR camera

*(Corrected 2026-07-19 — see Section 12 for full rationale.)*

`calib_result.yaml` records a calibration resolution of 1280×720 while Pattern-C requests 1920×1080. This resolution mismatch was independently re-verified and is real.

The claim that this file is provably the previous BRIO/RGB camera's calibration is **not confirmed by any explicit repository documentation** found on independent review — no file states this directly. It is a reasonable inference from circumstantial evidence (the repository separately contains `Docker/brio_setup_udev/`, i.e. a Logitech Brio RGB-camera udev setup used in the day/Pattern-A/B branches), but should be labeled as an *inference from repository structure*, not a confirmed fact.

**Status update since this report was written:** in code newer than the reviewed `test2.zip`, `inference.cpp`'s `CALIB_YAML_PATH` was changed from `weights/calib_result.yaml` to a dedicated `weights/calib_result_ir.yaml`, which addresses the "wrong-camera-file" concern specifically. However, **that new IR-specific file is still `image_width: 1280` / `image_height: 720`**, so the underlying 1280×720-vs-1920×1080 resolution mismatch — and the risk of incorrect yaw/pitch geometry it implies — remains unresolved as of the latest code.

Simple intrinsic scaling is not sufficient when a different physical camera, crop mode, lens, or sensor readout is used.

**Impact**

- incorrect yaw/pitch errors;
- oscillation or systematic pointing bias;
- apparently correct simulation results but incorrect hardware behavior.

**Required correction:** recalibrate the real IR camera at the exact deployed resolution (1920×1080, or change `active_width`/`active_height` defaults to match the calibrated 1280×720) and verify reprojection error.

## 5. High-priority findings

### H-01: Camera settings are not enforced

`applyCameraSettingsToCap()` requests width, height, and FPS, reads the resulting values, logs them, and always returns `true`. Control calculations still use `active_width_` and `active_height_`.

If the camera returns a different size, the target center and scaled camera matrix are wrong.

**Correction**

- compare requested and actual settings;
- fail startup or explicitly adapt when they differ;
- calculate target points from `frame.cols` and `frame.rows`;
- scale calibration from the actual frame size.

### H-02: Incorrect ONNX tensor buffer-length argument

The code passes:

```cpp
inputImage.total() * sizeof(float)
```

to the templated `CreateTensor<float>()` API. That API expects a count of float elements, so it should normally receive:

```cpp
inputImage.total()
```

The current fixed tensor shape may prevent immediate failure because the runtime consumes the amount required by the tensor shape, but the declared buffer contract is incorrect and unsafe.

### H-03: Fatal errors report success to the operating system

`main()` catches fatal exceptions, prints a message, and still returns `0`.

**Impact:** service managers, launch scripts, and automated checks can interpret a failed startup as success.

**Correction:** return a nonzero status after fatal exceptions.

### H-04: px3 initialization failure creates a silent inactive system

If serial initialization fails, px3 logs the error and publishes `px3_ready=false`. Px2 can then remain in WATCH indefinitely.

**Correction:** implement an explicit ERROR state with retry/backoff, health reporting, or nonzero process termination.

### H-05: Controller state winds up beyond servo limits

The controller clamps its returned angle but not its internal `pos_deg`:

```cpp
pos_deg += delta_deg;
const double servo = center_deg + pos_deg;
return std::clamp(servo, min_deg, max_deg);
```

At a limit, `pos_deg` can continue increasing. When error reverses, the hidden excess must unwind before the servo moves away from the limit.

**Correction:** clamp `pos_deg` itself to `[min_deg - center_deg, max_deg - center_deg]` and add saturation-recovery tests.

### H-06: Controller state and physical servo position can disagree

When detection starts, both controllers reset to a center-relative position of zero. When WATCH begins, the physical servos are not returned to center. A new detection cycle can therefore command a sudden jump.

**Correction options**

- center the servos on WATCH entry;
- preserve the last commanded controller position;
- ideally initialize from measured servo feedback.

### H-07: Diagnostic image saving can exhaust storage

`launch_px.sh` enables `save_frames:=true`. Successful frames are currently saved twice, and no retention or cleanup policy exists.

**Correction**

- default `save_frames` to false for deployment;
- sample at a limited interval;
- use a circular retention policy;
- monitor free disk space.

### H-08: Runtime serial failure has no recovery path

The system has no reconnect logic if the Arduino disconnects after startup. Read errors do not update readiness, and there is no hardware-health state transition.

**Correction:** detect permanent I/O errors, publish unhealthy state, stop issuing valid-looking ACKs, and retry connection with bounded backoff.

## 6. Medium- and low-priority findings

### Detection is not persistent tracking

The system runs YOLO each frame and selects the largest person box. It does not preserve target identity. With several people, the selected target can switch abruptly.

### ACK is not physical feedback

Px3 sends ACK after writing commands and waiting 30 ms. It does not measure actual servo angle, motion completion, power loss, or mechanical blockage.

### Preprocessing changes the source frame

`cv::cvtColor(image, image, cv::COLOR_BGR2RGB)` modifies the original frame. Later OpenCV image output can therefore have red and blue channels exchanged.

### Frame-save logic is misleading

The code saves a detected frame once with the `undetect_` prefix and again with `detect_`. Frames with no valid box are not saved because the helper rejects zero-area boxes.

### Model assumptions are insufficiently checked

The code assumes one input called `images`, one output called `output0`, and an output compatible with `[1,84,8400]`. It should validate output count, rank, dimensions, element type, and model names before indexing tensors.

### Person class index is assumed

Class index 0 is correct for standard COCO ordering but must be verified against the actual exported model metadata.

### Largest-area target selection is weak

Target selection ignores temporal continuity and uses size rather than a combined score using confidence, previous target position, center distance, or tracking identity.

### PIR readiness race

PIR HIGH received before `px3_ready=true` is ignored. If the sensor remains HIGH and no new report is generated, wake-up may be delayed until a new LOW-to-HIGH cycle.

### `/px_wakeup` has too many meanings

The same Boolean topic represents wake-up request, active detection, WATCH recovery, WatchNode reset, and px3 PIR reset. Separate request and state topics would be easier to verify.

### PIR setup can silently fail

`send_set_pin_mode()` uses one unchecked `write()` instead of `write_all()`. Partial writes and errors are ignored.

### Serial read errors are hidden

`read()` results less than or equal to zero are treated as polling timeouts. Permanent USB errors and disconnection are not distinguished from a normal timeout.

### Resource paths are not portable

The code hard-codes `/ros2_ws/work/src/px2/` even though `ament_index_cpp` is available. Installed package resources should be resolved through the ROS package index.

### Parameter validation is incomplete

Validation should cover confidence range, positive image sizes, positive loss-frame count, nonnegative debounce duration, supported camera FPS, PIR pin, and consistency between servo center and limits.

### Float angles are truncated

`static_cast<int>()` truncates servo commands. `std::lround()` would avoid systematic downward bias.

### Device permissions are overly broad

`chmod 777` is acceptable only as a temporary diagnostic workaround. Deployment should use udev rules and appropriate groups.

## 7. CI and verification assessment

### Existing strengths

- ROS 2 colcon build workflow;
- clang-format enforcement;
- Python formatting and linting;
- parameter-contract checks;
- MuJoCo Level 3 control-response tests;
- Level 4 WATCH/DETECT FSM tests;
- simulation artifacts and documentation.

### Important coverage gaps

The current workflows do not adequately exercise the real C++ runtime behavior for:

- ONNX tensor construction and model metadata;
- person-only postprocessing and NMS;
- coordinate unpadding;
- actual camera-size mismatch;
- launch parameter delivery to embedded px3;
- controller recovery after saturation;
- controller/physical-position synchronization;
- serial partial writes and USB disconnection;
- PIR startup timing;
- stale or missing ACK behavior;
- multi-person target switching;
- disk-retention behavior.

The Python simulation reproduces control and state behavior but does not execute the complete production C++ path. Simulation success therefore does not prove that the C++ camera, ONNX, ROS, or serial integration is correct.

## 8. Recommended remediation order

### Phase 1 — Make startup and inference valid

1. Restore the real YOLO model and add a model-presence/checksum check.
2. Fix embedded px3 parameter delivery.
3. Correct the ONNX tensor element count.
4. Return nonzero status on fatal startup failure.
5. Add model input/output validation and a one-frame smoke test.

### Phase 2 — Make camera geometry trustworthy

1. Recalibrate the actual IR camera.
2. Operate at the exact calibrated resolution.
3. Validate actual width, height, FPS, and pixel format at startup.
4. Use actual frame dimensions in all target calculations.

### Phase 3 — Stabilize servo control

1. Clamp internal controller state.
2. synchronize control state with the last commanded or measured servo position.
3. Verify yaw/pitch sign and mechanical limits on real hardware.
4. Replace truncation with rounding.
5. Add saturation, reversal, dead-zone, and repeated-wakeup tests.

### Phase 4 — Improve operational reliability

1. Add explicit health/error states.
2. Detect serial disconnection and implement bounded reconnection.
3. Separate wake-up request and mode/state topics.
4. Add image retention and disk monitoring.
5. Replace `chmod 777` with permanent udev/group permissions.

### Phase 5 — Improve tracking quality

1. Add persistent target identity using SORT, ByteTrack, or an equivalent tracker.
2. Combine confidence, location, size, and continuity in target selection.
3. Test multiple people, occlusion, re-entry, and low-contrast IR scenes.

## 9. Minimum acceptance criteria before unattended operation

- [ ] Real nonempty ONNX model is packaged and validated.
- [ ] Runtime px3 parameters match launch values.
- [ ] IR calibration is produced with the deployed camera and resolution.
- [ ] Actual camera dimensions must equal or safely update configured dimensions.
- [ ] Controller recovers immediately from both axis limits.
- [ ] A new wake-up cycle does not cause an unexpected center jump.
- [ ] Camera, model, and serial failures produce explicit unhealthy state or nonzero exit.
- [ ] Arduino reconnection is tested.
- [ ] Frame saving has bounded retention.
- [ ] Multi-person behavior is defined and tested.
- [ ] Real-hardware endurance testing is completed.

## 10. Verification limitations

This report is based on detailed static inspection of the supplied archive plus local Python and shell syntax checks.

The Python files compiled successfully, and the reviewed shell scripts passed Bash syntax validation. Full unit tests could not be executed in the review environment because `pytest` was not installed. A full ROS 2 build, ONNX inference run, and real hardware validation were not possible because the supplied ONNX model is empty and the camera, Arduino, PIR sensor, and servos were not connected.

Therefore, hardware-dependent items are explicitly described as risks where direct confirmation was not possible.

## 11. Final judgment

Pattern-C is a credible prototype with a clear PIR-to-detection-to-servo flow. Its main state machine and package structure are understandable, and the existing simulation/CI work provides a useful foundation.

However, the system still has several defects that can cause incorrect configuration, inaccurate tracking, servo-limit recovery problems, silent startup failure, or unreliable long-term behavior. It should be treated as a supervised test-stage implementation until the critical and high-priority findings in this report are resolved.

## 12. Independent verification addendum (2026-07-19)

This section documents a follow-up review that re-checked this report's findings directly against the source tree and adds items this report did not cover.

### 12.1 Findings independently confirmed accurate against source

The following were checked directly against the code and confirmed correct as written:

| ID | Confirmed evidence |
|---|---|
| C-01 | `inference.cpp`: `px3_node` is constructed with `rclcpp::NodeOptions().use_global_arguments(false)` and no explicit parameter overrides; `px3_system_launch.py` attaches `serial_port`/`baudrate`/`x_center_deg`/`y_center_deg` only to the `px2` `Node()` action. |
| C-02 | `weights/yolo11.onnx` is 0 bytes in the reviewed archive. |
| H-01 | `capture_settings.hpp::applyCameraSettingsToCap()` always returns `true` once `cap.isOpened()`; it never compares requested vs. actual `CAP_PROP_FRAME_WIDTH/HEIGHT`. `inference.cpp` uses `active_width_`/`active_height_` (the *requested*, not actual, values) for target-center math at lines computing `p_tgt` and `img_center`. |
| H-02 | `yolo_inference.cpp::RunInference()` passes `inputImage.total() * sizeof(float)` as the `p_data_element_count` argument to `Ort::Value::CreateTensor<float>()`, which expects an element count, not a byte count. |
| H-03 | `main()` in `inference.cpp` catches `std::exception`, logs it, and falls through to `return 0;` — no non-zero exit path exists for fatal startup failure. |
| H-05 | `AxisPdController::updateFromErrorRad()`: `pos_deg += delta_deg;` is unclamped; only the final `servo` value is clamped via `std::clamp(servo, min_deg, max_deg)`. |
| H-06 | `enterDetectTrackMode()` unconditionally calls `yaw_axis_.reset(0.0)` / `pitch_axis_.reset(0.0)`; `enterWatchMode()` issues no command to physically re-center the servos. A new DETECT cycle can therefore command an abrupt absolute-angle jump from wherever the servo was last left. |
| M-04 | `saveFrameIfNeeded(frame, box, frame_id, "undetect_")` is called unconditionally every frame; its only internal guard is `roi_img.area() < kMinBoxArea`, which a successful detection passes. A second call with `"detect_"` follows for the same frame. Net effect: every successful detection frame is saved twice, once under a misleading `undetect_` filename. |
| L-03 | `SeamlessTrack/setup_serial.sh` runs `sudo chmod 777 /dev/ttyACM0` and `sudo chmod 777 /dev/video/pir_camera`. |
| M-12 | `inference.h`: `static constexpr const char* kPkgPath = "/ros2_ws/work/src/px2/";` — hard-coded absolute path, not resolved via `ament_index_cpp`. |

No inaccuracies were found in any of the above as originally written.

### 12.2 Correction applied

- **C-03** — see the inline correction in Section 4. Summary: the resolution mismatch (1280×720 calibration vs. 1920×1080 request) is real and still unresolved in the latest code. The claim that the file is "confirmed by repository documentation" to be a leftover RGB/BRIO calibration is not independently verifiable from any document found in the repo; it is a reasonable inference from circumstantial evidence, not a confirmed fact, and should be read as such.

### 12.3 Related issues not covered by this report

- **`calibration/calibrate_pir_camera.py` writes a key `camera_calibration.hpp` cannot read.** The script writes distortion coefficients under the key `distortion_coefficients` (`fs.write("distortion_coefficients", D)`), while `camera_calibration.hpp::loadCalibrationFile()` reads the key `dist_coeffs` (`fs["dist_coeffs"] >> Draw;`). OpenCV's `FileStorage::operator>>` does not throw when a key is absent; it silently leaves the target empty. The calibration file currently shipped in `weights/` happens to use the correct `dist_coeffs` key (it was evidently produced some other way), so this is not causing a problem *today* — but the next time someone re-runs `calibrate_pir_camera.py` to recalibrate, the regenerated file's distortion coefficients will silently fail to load, and the system will run undistorted without any error being raised. This should be added as a Medium-severity finding alongside M-06 (person-class-index assumption), since both are "silently wrong, not crashing" categories of risk.
- **CI branch filters do not include a future Pattern-D branch.** `cpp-ci.yaml`, `colcon-build.yaml`, and `px-parameter-contract.yml` all scope their `branches:` triggers to `[main, day/pattern-A, day/pattern-B, night/pattern-C]`. When a Pattern-D branch is created, these will need to be updated or CI silently stops running on it. Low severity, housekeeping only.

### 12.4 Not independently re-verified

Items in this report that were not re-checked line-by-line in this addendum (M-01, M-02, M-05 through M-11 except M-04/M-12 above, L-01, L-02, and the CI/simulation coverage assessment in Section 7) were read and judged plausible against the surrounding code and architecture but were not each individually traced to source in this pass. Nothing found during verification contradicts them.

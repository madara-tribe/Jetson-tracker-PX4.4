// ============================================================
//  inference.cpp  — OnnxInferenceNode (px2) 実装
//
//  WATCH モード : PIR wakeup を待機。フレームは読み捨て。
//  DETECT_TRACK : YOLO 推論 → PD 制御 → /inference publish
// ============================================================
#include "inference.h"
#include "px3/px3_servo_node.h"

#include <ament_index_cpp/get_package_share_directory.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

#define YOLO_INPUT_H 640
#define YOLO_INPUT_W 640
// model_path / calib_path が空のときに使う、share/px2/ からの相対パス
#define ONNX_YOLO_PATH "weights/yolo11.onnx"
#define CALIB_YAML_PATH "weights/calib_result_ir.yaml"
#define DEFAULT_SAVE_DIR "/tmp/px2_frames"

using namespace std::chrono_literals;

namespace onnx_inference {

OnnxInferenceNode::OnnxInferenceNode() : Node("px2") {
  // -------------------- parameters --------------------
  device_path = declare_parameter<std::string>("device_path", "/dev/video0");

  watch_fps_ = declare_parameter<double>("watch_fps", 10.0);

  active_width_ = declare_parameter<int>("active_width", 1920);
  active_height_ = declare_parameter<int>("active_height", 1080);
  active_fps_ =
      declare_parameter<double>("active_fps", 30.0); // メンバへ代入（DETECT時も同値を使用）

  capture_buffer_size_ = declare_parameter<int>("capture_buffer_size", 1);
  // true: 別スレッドで読み続け、最新の 1 枚だけを使う。false: 以前と同じ（処理スレッドで読む）
  use_capture_thread_ = declare_parameter<bool>("capture_thread", true);

  // ファイルの場所。空なら share/px2/（colcon のインストール先）から探す
  model_path_ = declare_parameter<std::string>("model_path", "");
  calib_path_ = declare_parameter<std::string>("calib_path", "");
  save_dir_ = declare_parameter<std::string>("save_dir", "");
  if (model_path_.empty() || calib_path_.empty()) {
    const std::filesystem::path share = ament_index_cpp::get_package_share_directory("px2");
    if (model_path_.empty())
      model_path_ = (share / ONNX_YOLO_PATH).string();
    if (calib_path_.empty())
      calib_path_ = (share / CALIB_YAML_PATH).string();
  }
  if (save_dir_.empty())
    save_dir_ = DEFAULT_SAVE_DIR;
  RCLCPP_INFO(get_logger(), "px2: model=%s", model_path_.c_str());
  RCLCPP_INFO(get_logger(), "px2: calib=%s", calib_path_.c_str());

  lost_timeout_s_ = declare_parameter<double>("lost_timeout_s", 1.0);
  if (lost_timeout_s_ <= 0.0)
    throw std::invalid_argument("lost_timeout_s must be > 0");
  save_frames_ = declare_parameter<bool>("save_frames", false);
  if (save_frames_) {
    std::filesystem::create_directories(save_dir_);
    RCLCPP_INFO(get_logger(), "px2: detected frames are saved to %s", save_dir_.c_str());
  }
  const double conf_threshold = declare_parameter<double>("conf_threshold", 0.25);

  // 推論デバイス: "trt"（TensorRT 直接）| "tensorrt" | "cuda" | "cpu"
  //   使えなければ自動で CPU などに切り替える
  OrtBackendOptions backend;
  backend.provider = declare_parameter<std::string>("ort_provider", "tensorrt");
  backend.trt_fp16 = declare_parameter<bool>("trt_fp16", true);
  backend.trt_cache_dir = declare_parameter<std::string>("trt_cache_dir", "/tmp/px_trt_cache");
  backend.trt_engine = declare_parameter<std::string>("trt_engine", "");

  // 計測 CSV（空なら書かない）。例: /ros2_ws/work/bench/results/xxx/live_cpu.csv
  const std::string timing_csv_path = declare_parameter<std::string>("timing_csv", "");
  timing_fp16_ = backend.trt_fp16;
  if (!timing_csv_path.empty()) {
    const auto parent = std::filesystem::path(timing_csv_path).parent_path();
    if (!parent.empty())
      std::filesystem::create_directories(parent);
    timing_csv_.open(timing_csv_path, std::ios::out | std::ios::trunc);
    if (!timing_csv_) {
      throw std::runtime_error("cannot open timing_csv: " + timing_csv_path);
    }
    // detected 以降の列は 2026-11-19 に追加。前の列の並びは変えていない。
    //   detected       : このフレームで人を検出したか (1/0)
    //   yaw_err_deg    : 画像中心からのずれ（度）。検出なしのときは 0
    //   pitch_err_deg  : 同上（縦）
    //   cmd_x_deg/cmd_y_deg : px2 が持っている指令角（px3 が四捨五入して整数でサーボに送る）
    //   published      : このフレームで /inference を送ったか (1/0)
    //   session_id     : DETECT に入るたびに 1 増える番号
    //   to_watch       : このフレームで見失いと判定し、WATCH に戻ったか (1/0)
    //   frame_age_ms   : フレームを撮ってから処理を始めるまでの時間（capture_thread=true のとき）
    //   servo_at_capture_x_deg/y : フレームを撮った時刻の指令角（整数。px3 が送った値）
    //   target_abs_x_deg/y       : 人の絶対角 = 撮影時の指令角 + 誤差。検出なしのときは空
    // first_frame は「DETECT に入った最初の 1 フレーム」だけ 1（以前は最初に検出するまで 1）。
    // capture_ms は「前のフレームの処理が終わってから、次のフレームを受け取るまで」の時間。
    timing_csv_ << "time_s,frame_id,provider,fp16,first_frame,capture_ms,pre_ms,infer_ms,post_ms,"
                   "control_ms,loop_ms,n_det,best_conf,box_area,"
                   "detected,yaw_err_deg,pitch_err_deg,cmd_x_deg,cmd_y_deg,published,"
                   "session_id,to_watch,frame_age_ms,"
                   "servo_at_capture_x_deg,servo_at_capture_y_deg,"
                   "target_abs_x_deg,target_abs_y_deg\n";
    timing_t0_ = std::chrono::steady_clock::now();
    RCLCPP_INFO(get_logger(), "px2: timing CSV -> %s", timing_csv_path.c_str());
  }

  x_max_ = declare_parameter<double>("xmax", 180.0);
  x_min_ = declare_parameter<double>("xmin", 0.0);
  y_max_ = declare_parameter<double>("ymax", 90.0);
  y_min_ = declare_parameter<double>("ymin", 20.0);
  x_center_deg_ = declare_parameter<double>("x_center_deg", 90.0);
  y_center_deg_ = declare_parameter<double>("y_center_deg", 90.0);
  // px3 は起動時にサーボを中心へ動かす。px2 が持つ「今の角度」も同じ値から始める
  servo_x_deg_ = x_center_deg_;
  servo_y_deg_ = y_center_deg_;
  // 履歴の最初の 1 件（px3 が起動時に送る中心角。px3 と同じく四捨五入した値）
  cmd_history_.push(std::chrono::steady_clock::now(), std::round(x_center_deg_),
                    std::round(y_center_deg_));
  servo_lag_ms_ = declare_parameter<double>("servo_lag_ms", 0.0);
  if (servo_lag_ms_ < 0.0)
    throw std::invalid_argument("servo_lag_ms must be >= 0");

  // px3 に渡すだけの値（px3ParameterOverrides() を参照）
  px3_serial_port_ = declare_parameter<std::string>("serial_port", "/dev/ttyACM0");
  px3_baudrate_ = declare_parameter<int>("baudrate", 115200);
  px3_pir_pin_ = declare_parameter<int>("pir_pin", 2);

  // PD controller gains and step limit — tunable via launch arguments
  //   max_step_deg : 1 フレームで動かす最大量（度）。launch の既定値と同じ 3.0
  //   kd_*         : 「1 フレームあたりの誤差の変化」に掛ける（dt では割らない）。
  //                  以前の Kd=0.2（dt で割る）は 30 Hz で 6.0 相当だった
  const double max_step_deg = declare_parameter<double>("max_step_deg", 3.0);
  const double kp_yaw = declare_parameter<double>("kp_yaw", 0.3);
  const double kd_yaw = declare_parameter<double>("kd_yaw", 0.3);
  const double kp_pitch = declare_parameter<double>("kp_pitch", 0.3);
  const double kd_pitch = declare_parameter<double>("kd_pitch", 0.3);

  yaw_dead_zone_deg_ = declare_parameter<double>("dead_zone_yaw_deg", 1.0);
  pitch_dead_zone_deg_ = declare_parameter<double>("dead_zone_pitch_deg", 1.0);
  if (yaw_dead_zone_deg_ < 0.0 || pitch_dead_zone_deg_ < 0.0)
    throw std::invalid_argument("dead_zone_*_deg must be >= 0");

  // -------------------- axis controller --------------------
  yaw_axis_.Kp = kp_yaw;
  yaw_axis_.Kd = kd_yaw;
  yaw_axis_.max_step_deg = max_step_deg;
  yaw_axis_.center_deg = x_center_deg_;
  yaw_axis_.min_deg = x_min_;
  yaw_axis_.max_deg = x_max_;
  yaw_axis_.validate("yaw_axis");
  yaw_axis_.reset(0.0);

  pitch_axis_.Kp = kp_pitch;
  pitch_axis_.Kd = kd_pitch;
  pitch_axis_.max_step_deg = max_step_deg;
  pitch_axis_.center_deg = y_center_deg_;
  pitch_axis_.min_deg = y_min_;
  pitch_axis_.max_deg = y_max_;
  pitch_axis_.validate("pitch_axis");
  pitch_axis_.reset(0.0);

  // -------------------- publishers/subscribers --------------------
  {
    rclcpp::QoS qos(rclcpp::KeepLast(1));
    qos.reliable().durability_volatile();
    pub_abs_ = create_publisher<custom_msgs::msg::AbsResult>("inference", qos);
  }

  {
    // /px_wakeup: transient_local — watch_node が先に起動済みでも受信できる
    auto qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
    wakeup_pub_ = create_publisher<std_msgs::msg::Bool>("px_wakeup", qos);
    wakeup_sub_ = create_subscription<std_msgs::msg::Bool>(
        "px_wakeup", qos, [this](const std_msgs::msg::Bool::SharedPtr msg) {
          if (msg->data)
            wakeup_requested_.store(true);
        });
    publishWakeup(false);
  }

  {
    auto ready_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
    px3_ready_sub_ = create_subscription<std_msgs::msg::Bool>(
        "px3_ready", ready_qos, [this](const std_msgs::msg::Bool::SharedPtr msg) {
          if (msg->data) {
            px3_ready_ = true;
            RCLCPP_INFO(get_logger(), "px2: received px3_ready=True");
          }
        });
  }

  // -------------------- camera --------------------
  RCLCPP_INFO(get_logger(), "Opening camera at: %s", device_path.c_str());
  cap_.open(device_path, cv::CAP_V4L2);
  if (!cap_.isOpened()) {
    RCLCPP_ERROR(get_logger(), "Failed to open camera: %s", device_path.c_str());
    throw std::runtime_error("camera open failed");
  }

  vision::capture::applyCameraSettingsToCap(cap_, get_logger(), active_width_, active_height_,
                                            active_fps_, capture_buffer_size_);
  mode_ = Mode::WATCH;

  // -------------------- calibration + model --------------------
  if (vision::loadCalibrationFile(calib_path_, active_width_, active_height_, calib_)) {
    RCLCPP_INFO(get_logger(), "Loaded calibration: (calib %dx%d -> active %dx%d)", calib_.calib_w,
                calib_.calib_h, active_width_, active_height_);
    if (calib_.aspect_mismatch) {
      RCLCPP_WARN(get_logger(),
                  "Calibration aspect ratio (%dx%d) differs from capture (%dx%d): "
                  "pixel-to-angle conversion will be off. Recalibrate at the capture resolution.",
                  calib_.calib_w, calib_.calib_h, active_width_, active_height_);
    }
  } else {
    RCLCPP_ERROR(get_logger(), "Calibration not loaded: %s", calib_path_.c_str());
    throw std::runtime_error("Calibration not loaded");
  }

  if (!std::filesystem::exists(model_path_))
    throw std::runtime_error("model not found: " + model_path_);
  yolo_ = std::make_unique<YoloDetect>(model_path_, static_cast<float>(conf_threshold), backend);
  RCLCPP_INFO(get_logger(), "px2: inference provider = %s (requested %s)",
              yolo_->activeProvider().c_str(), backend.provider.c_str());
  // GPU を要求したのに CPU で動いている場合は、はっきり知らせる（CPU は 1 フレーム約 88 ms）
  requested_provider_ = backend.provider;
  provider_fell_back_ = (requested_provider_ != "cpu" && yolo_->activeProvider() == "cpu");
  if (provider_fell_back_) {
    RCLCPP_ERROR(get_logger(),
                 "px2: requested provider '%s' is NOT in use. Running on CPU (much slower). "
                 "Check the [yolo]/[trt] messages above (engine file, nvidia runtime, build).",
                 requested_provider_.c_str());
  }
  RCLCPP_INFO(get_logger(),
              "px2: control kp=(%.2f, %.2f) kd=(%.2f, %.2f) per-frame, max_step=%.1f deg, "
              "dead_zone=(%.1f, %.1f) deg, lost_timeout=%.2f s",
              yaw_axis_.Kp, pitch_axis_.Kp, yaw_axis_.Kd, pitch_axis_.Kd, max_step_deg,
              yaw_dead_zone_deg_, pitch_dead_zone_deg_, lost_timeout_s_);

  timer_ = create_wall_timer(50ms, std::bind(&OnnxInferenceNode::callbackInference, this));
}

std::vector<rclcpp::Parameter> OnnxInferenceNode::px3ParameterOverrides() const {
  return {
      rclcpp::Parameter("serial_port", px3_serial_port_),
      rclcpp::Parameter("baudrate", px3_baudrate_),
      rclcpp::Parameter("x_center_deg", x_center_deg_),
      rclcpp::Parameter("y_center_deg", y_center_deg_),
      rclcpp::Parameter("pir_pin", px3_pir_pin_),
  };
}

void OnnxInferenceNode::publishWakeup(bool on) {
  std_msgs::msg::Bool m;
  m.data = on;
  wakeup_pub_->publish(m);
}

// WATCHモードに遷移。wakeup_requested_ をクリアしてPIR待機状態に戻る。
void OnnxInferenceNode::enterWatchMode() {
  mode_ = Mode::WATCH;
  if (use_capture_thread_)
    grabber_.setDecode(false);    // WATCH 中は取り出すだけ（展開しない）
  wakeup_requested_.store(false); // 次のPIR検知まで誤トリガーを防ぐ
  publishWakeup(false);
  RCLCPP_INFO(get_logger(), "px2: enter WATCH mode");
}

// DETECT_TRACKモードに遷移。PDコントローラーを「今のサーボ角」から再開しwakeup状態をpublish。
void OnnxInferenceNode::enterDetectTrackMode() {
  mode_ = Mode::DETECT_TRACK;
  ++session_id_;
  first_detect_frame_ = true;
  prev_frame_detected_ = false;
  last_det_t_ = std::chrono::steady_clock::now(); // 見失い判定はここから数える
  // WATCH に戻ってもサーボは最後の向きのまま止まっている。
  // 位置を 0（中央）に戻すと、最初の指令で中央へ飛ぶので、今の角度から再開する。
  yaw_axis_.resetAtServoDeg(servo_x_deg_);
  pitch_axis_.resetAtServoDeg(servo_y_deg_);
  if (use_capture_thread_) {
    // 読み取りスレッドが cap_ を使っているので、ここでカメラの設定をし直さない。
    // WATCH 中も取り出し続けているので、古いフレームは溜まっていない。展開を始めるだけでよい。
    grabber_.setDecode(true);
  } else {
    vision::capture::applyCameraSettingsToCap(cap_, get_logger(), active_width_, active_height_,
                                              active_fps_, capture_buffer_size_); // ★復活
  }

  publishWakeup(true); // watch_node へ検知中を通知（wakeup_sent_ リセット防止）
  RCLCPP_INFO(get_logger(), "px2: enter DETECT+TRACK mode");
}

// 軸指定でPD制御を計算する。YAW/PITCHで対象軸・誤差関数・サーボ変数を切り替える。
double OnnxInferenceNode::computeAxisErrorDeg(Axis axis, const cv::Rect2d& box) {
  const cv::Point2f p_meas = vision::bbox::bboxCenter(box);

  if (axis == Axis::YAW) {
    const cv::Point2f p_tgt(static_cast<float>(active_width_ / 2.0), p_meas.y);
    const double yaw_rad = control::yawErrFromPixels(p_meas, p_tgt, calib_.K, calib_.D);
    const double err_deg = yaw_axis_.errorDeg(yaw_rad);
    RCLCPP_DEBUG(get_logger(), "yaw_rad=%.3f | err_deg=%.2f", yaw_rad, err_deg);
    return err_deg;
  } else {
    const cv::Point2f p_tgt(p_meas.x, static_cast<float>(active_height_ * kTargetYRatio));
    const double pitch_rad = control::pitchErrFromPixels(p_meas, p_tgt, calib_.K, calib_.D);
    const double err_deg = pitch_axis_.errorDeg(pitch_rad);
    RCLCPP_DEBUG(get_logger(), "pitch_rad=%.3f | err_deg=%.2f", pitch_rad, err_deg);
    return err_deg;
  }
}

void OnnxInferenceNode::publishState(float yaw_deg, float pitch_deg) {
  custom_msgs::msg::AbsResult m;
  m.x_angle = yaw_deg;
  m.y_angle = pitch_deg;
  RCLCPP_DEBUG(get_logger(), "px2: publishState X=%.2f Y=%.2f", m.x_angle, m.y_angle);
  pub_abs_->publish(m);
  // 送った指令を履歴に残す。px3 は四捨五入した整数をサーボに送るので、同じ値を入れる
  cmd_history_.push(std::chrono::steady_clock::now(), std::round(static_cast<double>(yaw_deg)),
                    std::round(static_cast<double>(pitch_deg)));
}

void OnnxInferenceNode::saveFrameIfNeeded(const cv::Mat& frame, const cv::Rect2d& roi_img,
                                          int frame_id, const std::string& name_prefix) {
  if (!save_frames_)
    return;
  if (roi_img.area() < kMinBoxArea)
    return;

  const int center_x = static_cast<int>(roi_img.x + roi_img.width / 2);
  const int center_y = static_cast<int>(roi_img.y + roi_img.height / 2);

  cv::Mat vis = frame.clone();
  cv::drawMarker(vis, cv::Point(center_x, center_y), cv::Scalar(0, 0, 255), cv::MARKER_CROSS, 30, 2,
                 cv::LINE_AA);
  cv::line(vis, cv::Point(center_x, 0), cv::Point(center_x, frame.rows), cv::Scalar(0, 255, 0), 1,
           cv::LINE_AA);
  cv::line(vis, cv::Point(0, center_y), cv::Point(frame.cols, center_y), cv::Scalar(0, 255, 0), 1,
           cv::LINE_AA);
  cv::rectangle(vis, roi_img, cv::Scalar(255, 0, 0), 2, cv::LINE_AA);

  const std::filesystem::path out =
      std::filesystem::path(save_dir_) / (name_prefix + std::to_string(frame_id) + ".png");
  cv::imwrite(out.string(), vis);
  RCLCPP_DEBUG(get_logger(), "px2: saved image -> %s", out.string().c_str());
}

void OnnxInferenceNode::callbackInference() {
  if (started_)
    return;
  started_ = true;
  timer_->cancel(); // 1 回起動すれば十分。以後 50 ms ごとに空振りさせない
  pipeline_thread_ = std::thread(&OnnxInferenceNode::runPipelineLoop, this);
}

// 次のフレームを受け取る。
//   capture_thread=true : 読み取りスレッドの最新の 1 枚を待つ（WATCH 中は何も来ない）
//   capture_thread=false: このスレッドで cap_.read() する（以前と同じ）
bool OnnxInferenceNode::nextFrame(cv::Mat& frame, std::chrono::steady_clock::time_point& t_capture,
                                  bool& got) {
  got = false;
  if (use_capture_thread_) {
    // WATCH 中は 50 ms ごとに戻って、起動要求を確かめる
    const auto timeout = (mode_ == Mode::WATCH) ? 50ms : 200ms;
    const auto r = grabber_.waitNext(frame, t_capture, grab_seq_, timeout);
    if (r == vision::LatestFrameGrabber::Result::kEnded)
      return false;
    got = (r == vision::LatestFrameGrabber::Result::kFrame);
    return true;
  }
  if (!cap_.read(frame))
    return false;
  t_capture = std::chrono::steady_clock::now();
  got = !frame.empty();
  return true;
}

// WATCH / DETECT_TRACK のステートマシン本体。
void OnnxInferenceNode::runPipelineLoop() {
  RCLCPP_INFO(get_logger(), "[px2] pipeline start. capture_thread=%s",
              use_capture_thread_ ? "true" : "false");

  int frame_id = 0;
  cv::Mat frame;
  auto t_prev = std::chrono::steady_clock::now();

  if (use_capture_thread_) {
    grabber_.setDecode(false); // WATCH から始まる
    grabber_.start(&cap_);
  }

  auto t_iter_end = std::chrono::steady_clock::now(); // 直前の DETECT 処理が終わった時刻
  while (rclcpp::ok()) {
    bool got = false;
    std::chrono::steady_clock::time_point t_capture; // フレームを撮った時刻
    if (!nextFrame(frame, t_capture, got))
      break;
    const auto t_got = std::chrono::steady_clock::now(); // フレームを受け取った時刻

    // ==================================================
    //  WATCH mode
    // ==================================================
    if (mode_ == Mode::WATCH) {
      const auto t0 = std::chrono::steady_clock::now();

      // PIR 経由の /px_wakeup=true により wakeup_requested_ がセットされる。
      // px3 の準備ができるまでは、起動要求を消さずに残しておく。
      if (px3_ready_.load() && wakeup_requested_.exchange(false)) {
        enterDetectTrackMode();
        continue;
      }

      if (!use_capture_thread_) {
        // 低 fps を維持してフレームを読み捨て（カメラバッファを空にするため必要）
        const double fps = (watch_fps_ > 0.0 ? watch_fps_ : 2.0);
        const auto period = std::chrono::duration<double>(1.0 / fps);
        const auto dt = std::chrono::steady_clock::now() - t0;
        if (dt < period)
          std::this_thread::sleep_for(period - dt);
      }

      if (got)
        ++frame_id;
      continue;
    }

    if (!got)
      continue; // まだ新しいフレームが無い

    // ===== DETECT+TRACK =====
    const auto t_now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(t_now - t_prev).count();
    t_prev = t_now;

    const bool is_first_frame = first_detect_frame_;
    first_detect_frame_ = false; // DETECT に入った最初の 1 フレームだけ 1
    const cv::Size imsz = frame.size();
    const auto tp0 = std::chrono::steady_clock::now();
    // preprocess は frame を書き換えない（保存する画像の色が入れ替わらない）
    cv::Mat inputImage = yolo_->preprocess(frame, YOLO_INPUT_W, YOLO_INPUT_H);
    const auto tp1 = std::chrono::steady_clock::now();
    std::vector<Ort::Value> outputTensors = yolo_->RunInference(inputImage);
    const auto tp2 = std::chrono::steady_clock::now();
    std::vector<Result> results = yolo_->postprocess(imsz, outputTensors);
    const auto tp3 = std::chrono::steady_clock::now();
    using ms = std::chrono::duration<double, std::milli>;

    cv::Rect2d box = vision::bbox::findBiggestBBox(results);
    const bool detected = box.area() >= kMinBoxArea;
    RCLCPP_DEBUG(get_logger(),
                 "px2[debug]: results.size()=%zu box=(x=%.1f,y=%.1f,w=%.1f,h=%.1f) -> %s",
                 results.size(), box.x, box.y, box.width, box.height,
                 detected ? "detected" : "no detection");

    double yaw_err_deg = 0.0;
    double pitch_err_deg = 0.0;
    bool published = false;
    bool to_watch = false;

    // フレームを撮った時刻のサーボ角（指令角の履歴から引く）。この値は記録するだけで、
    // 制御にはまだ使っていない（段階 11 で使う）。
    double servo_cap_x_deg = servo_x_deg_;
    double servo_cap_y_deg = servo_y_deg_;
    cmd_history_.at(t_capture - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double, std::milli>(servo_lag_ms_)),
                    servo_cap_x_deg, servo_cap_y_deg);

    if (detected) {
      last_det_t_ = tp3;
      // 直前のフレームで検出できていないときは、前フレームとの差が使えないので D 項を使わない
      const bool use_d = prev_frame_detected_;

      yaw_err_deg = computeAxisErrorDeg(Axis::YAW, box);
      pitch_err_deg = computeAxisErrorDeg(Axis::PITCH, box);
      const bool yaw_in_dead = std::abs(yaw_err_deg) < yaw_dead_zone_deg_;
      const bool pitch_in_dead = std::abs(pitch_err_deg) < pitch_dead_zone_deg_;

      // 死帯の中: 動かさないが、誤差は覚えておく（死帯を出た瞬間に D 項が跳ねないように）
      if (yaw_in_dead) {
        yaw_axis_.observe(yaw_err_deg);
      } else {
        servo_x_deg_ = yaw_axis_.commit(yaw_err_deg, use_d);
      }
      if (pitch_in_dead) {
        pitch_axis_.observe(pitch_err_deg);
      } else {
        servo_y_deg_ = pitch_axis_.commit(pitch_err_deg, use_d);
      }
      if (!yaw_in_dead || !pitch_in_dead) {
        publishState(static_cast<float>(servo_x_deg_), static_cast<float>(servo_y_deg_));
        published = true;
      }

      // 検出したフレームだけ保存する（1 フレーム 1 枚）。
      // 以前は "undetect_" と "detect_" の 2 枚を、どちらも検出時に保存していた。
      saveFrameIfNeeded(frame, box, frame_id, "detect_");

    } else {
      // 最後に検出してから lost_timeout_s 秒たったら WATCH に戻る
      const double lost_s = std::chrono::duration<double>(tp3 - last_det_t_).count();
      if (lost_s >= lost_timeout_s_) {
        enterWatchMode();
        to_watch = true;
      }
    }
    prev_frame_detected_ = detected;

    // 1 秒に 1 回だけ出す（毎フレームのログは DEBUG に下げた）
    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 1000,
                         "px2[timing] provider=%s pre=%.1fms infer=%.1fms post=%.1fms loop=%.1fms"
                         " age=%.1fms | det=%d err=(%.2f, %.2f)deg cmd=(%.1f, %.1f)deg",
                         yolo_->activeProvider().c_str(), ms(tp1 - tp0).count(),
                         ms(tp2 - tp1).count(), ms(tp3 - tp2).count(), dt * 1000.0,
                         ms(tp0 - t_capture).count(), detected ? 1 : 0, yaw_err_deg, pitch_err_deg,
                         servo_x_deg_, servo_y_deg_);
    if (provider_fell_back_) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000,
                           "px2: running on CPU although '%s' was requested (much slower)",
                           requested_provider_.c_str());
    }

    // ---------- 計測 CSV ----------
    const auto tp4 = std::chrono::steady_clock::now();
    if (timing_csv_.is_open()) {
      float best_conf = 0.0f;
      for (const auto& r : results)
        best_conf = std::max(best_conf, r.accuracy);
      timing_csv_ << std::chrono::duration<double>(tp4 - timing_t0_).count() << ',' << frame_id
                  << ',' << yolo_->activeProvider() << ',' << (timing_fp16_ ? 1 : 0) << ','
                  << (is_first_frame ? 1 : 0) << ',' << ms(t_got - t_iter_end).count() << ','
                  << ms(tp1 - tp0).count() << ',' << ms(tp2 - tp1).count() << ','
                  << ms(tp3 - tp2).count() << ',' << ms(tp4 - tp3).count() << ',' << dt * 1000.0
                  << ',' << results.size() << ',' << best_conf << ',' << box.area() << ','
                  << (detected ? 1 : 0) << ',' << yaw_err_deg << ',' << pitch_err_deg << ','
                  << servo_x_deg_ << ',' << servo_y_deg_ << ',' << (published ? 1 : 0) << ','
                  << session_id_ << ',' << (to_watch ? 1 : 0) << ',' << ms(tp0 - t_capture).count()
                  << ',' << servo_cap_x_deg << ',' << servo_cap_y_deg << ',';
      if (detected) {
        timing_csv_ << (servo_cap_x_deg + yaw_err_deg) << ',' << (servo_cap_y_deg + pitch_err_deg);
      } else {
        timing_csv_ << ','; // 検出なし: 絶対角は空
      }
      timing_csv_ << '\n';
      if (++timing_rows_ % 30 == 0)
        timing_csv_.flush();
    }
    t_iter_end = std::chrono::steady_clock::now();

    ++frame_id;
  }
  if (timing_csv_.is_open())
    timing_csv_.flush();
}

} // namespace onnx_inference

// ============================================================
//  main — 3ノード構成
//    px2_node (YOLO 推論 + DETECT+TRACK)
//    px3_node (servo + PIR 管理。px2 と同じプロセス。パラメータは px2 経由で受け取る)
//    watch_node は別プロセス (px3 パッケージ)
// ============================================================
int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  try {
    // px2 を先に作る。launch のパラメータは px2 にだけ届くので、
    // px3 用の値（serial_port / baudrate / x_center_deg / y_center_deg / pir_pin）は
    // px2 から受け取って px3 に渡す。
    // px3 は use_global_arguments(false) のまま（launch の __node:=px2 で名前が変わるのを防ぐ）。
    auto px2_node = std::make_shared<onnx_inference::OnnxInferenceNode>();
    auto px3_node = std::make_shared<px3_embedded::Px3ServoNode>(
        rclcpp::NodeOptions().use_global_arguments(false).parameter_overrides(
            px2_node->px3ParameterOverrides()));

    rclcpp::executors::MultiThreadedExecutor exec(rclcpp::ExecutorOptions(), 4);
    exec.add_node(px3_node);
    exec.add_node(px2_node);
    exec.spin();

  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << std::endl;
  }
  rclcpp::shutdown();
  return 0;
}

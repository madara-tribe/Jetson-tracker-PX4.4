// ============================================================
//  inference.cpp  — OnnxInferenceNode (px2) 実装
//
//  WATCH モード : PIR wakeup を待機。フレームは読み捨て。
//  DETECT_TRACK : YOLO 推論 → PD 制御 → /inference publish
// ============================================================
#include "inference.h"
#include "px3/px3_servo_node.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

#define YOLO_INPUT_H 640
#define YOLO_INPUT_W 640
#define ONNX_YOLO_PATH "weights/yolo11.onnx"
#define CALIB_YAML_PATH "weights/calib_result_ir.yaml"

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

  lost_max_frames_ = declare_parameter<int>("lost_max_frames", 15);
  save_frames_ = declare_parameter<bool>("save_frames", false);
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
    timing_csv_ << "time_s,frame_id,provider,fp16,first_frame,capture_ms,pre_ms,infer_ms,post_ms,"
                   "control_ms,loop_ms,n_det,best_conf,box_area\n";
    timing_t0_ = std::chrono::steady_clock::now();
    RCLCPP_INFO(get_logger(), "px2: timing CSV -> %s", timing_csv_path.c_str());
  }

  x_max_ = declare_parameter<double>("xmax", 180.0);
  x_min_ = declare_parameter<double>("xmin", 0.0);
  y_max_ = declare_parameter<double>("ymax", 90.0);
  y_min_ = declare_parameter<double>("ymin", 20.0);
  x_center_deg_ = declare_parameter<double>("x_center_deg", 90.0);
  y_center_deg_ = declare_parameter<double>("y_center_deg", 90.0);

  // PD controller gains and step limit — tunable via launch arguments
  const double max_step_deg =
      declare_parameter<double>("max_step_deg", 1.2); // 3.0 → 1.2（dead zone未満に抑制）
  const double kp_yaw = declare_parameter<double>("kp_yaw", 0.3);
  const double kd_yaw = declare_parameter<double>("kd_yaw", 0.2);
  const double kp_pitch = declare_parameter<double>("kp_pitch", 0.3);
  const double kd_pitch = declare_parameter<double>("kd_pitch", 0.2);

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
                                            active_fps_);
  mode_ = Mode::WATCH;

  // -------------------- calibration + model --------------------
  if (vision::loadCalibrationFile(pkg_path + CALIB_YAML_PATH, active_width_, active_height_,
                                  calib_)) {
    RCLCPP_INFO(get_logger(), "Loaded calibration: (calib %dx%d -> active %dx%d)", calib_.calib_w,
                calib_.calib_h, active_width_, active_height_);
  } else {
    RCLCPP_WARN(get_logger(), "Calibration not loaded.");
    throw std::runtime_error("Calibration not loaded");
  }

  yolo_ = std::make_unique<YoloDetect>(pkg_path + std::string(ONNX_YOLO_PATH),
                                       static_cast<float>(conf_threshold), backend);
  RCLCPP_INFO(get_logger(), "px2: inference provider = %s (requested %s)",
              yolo_->activeProvider().c_str(), backend.provider.c_str());

  timer_ = create_wall_timer(50ms, std::bind(&OnnxInferenceNode::callbackInference, this));
}

void OnnxInferenceNode::publishWakeup(bool on) {
  std_msgs::msg::Bool m;
  m.data = on;
  wakeup_pub_->publish(m);
}

// WATCHモードに遷移。wakeup_requested_ をクリアしてPIR待機状態に戻る。
void OnnxInferenceNode::enterWatchMode() {
  mode_ = Mode::WATCH;
  no_det_frames_ = 0;
  wakeup_requested_.store(false); // 次のPIR検知まで誤トリガーを防ぐ
  publishWakeup(false);
  RCLCPP_INFO(get_logger(), "px2: enter WATCH mode");
}

// DETECT_TRACKモードに遷移。PDコントローラーをリセットしwakeup状態をpublish。
void OnnxInferenceNode::enterDetectTrackMode() {
  mode_ = Mode::DETECT_TRACK;
  no_det_frames_ = 0;
  first_detect_frame_ = true;
  yaw_axis_.reset(0.0);
  pitch_axis_.reset(0.0);
  vision::capture::applyCameraSettingsToCap(cap_, get_logger(), active_width_, active_height_,
                                            active_fps_); // ★復活

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
    RCLCPP_INFO(get_logger(), "yaw_rad=%.3f | err_deg=%.2f", yaw_rad, err_deg);
    return err_deg;
  } else {
    const cv::Point2f p_tgt(p_meas.x, static_cast<float>(active_height_ * kTargetYRatio));
    const double pitch_rad = control::pitchErrFromPixels(p_meas, p_tgt, calib_.K, calib_.D);
    const double err_deg = pitch_axis_.errorDeg(pitch_rad);
    RCLCPP_INFO(get_logger(), "pitch_rad=%.3f | err_deg=%.2f", pitch_rad, err_deg);
    return err_deg;
  }
}

void OnnxInferenceNode::publishState(float yaw_deg, float pitch_deg) {
  custom_msgs::msg::AbsResult m;
  m.x_angle = yaw_deg;
  m.y_angle = pitch_deg;
  RCLCPP_INFO(get_logger(), "px2: publishState X=%.2f Y=%.2f", m.x_angle, m.y_angle);
  pub_abs_->publish(m);
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
      std::filesystem::path(pkg_path) / "data" / (name_prefix + std::to_string(frame_id) + ".png");
  cv::imwrite(out.string(), vis);
  RCLCPP_INFO(get_logger(), "px2: saved image -> %s", out.string().c_str());
}

void OnnxInferenceNode::callbackInference() {
  if (started_)
    return;
  started_ = true;
  pipeline_thread_ = std::thread(&OnnxInferenceNode::runPipelineLoop, this);
}

// WATCH / DETECT_TRACK のステートマシン本体。
void OnnxInferenceNode::runPipelineLoop() {
  RCLCPP_INFO(get_logger(), "[px2] pipeline start.");

  int frame_id = 0;
  cv::Mat frame;
  auto t_prev = std::chrono::steady_clock::now();

  auto t_iter_end = std::chrono::steady_clock::now(); // 直前の DETECT 処理が終わった時刻
  while (rclcpp::ok() && cap_.read(frame)) {
    const auto t_frame = std::chrono::steady_clock::now(); // フレームを受け取った時刻
    if (frame.empty())
      continue;

    // ==================================================
    //  WATCH mode
    // ==================================================
    if (mode_ == Mode::WATCH) {
      const auto t0 = std::chrono::steady_clock::now();

      // PIR 経由の /px_wakeup=true により wakeup_requested_ がセットされる
      const bool ext_wakeup = wakeup_requested_.exchange(false);
      if (ext_wakeup && px3_ready_.load()) {
        enterDetectTrackMode();
        continue;
      }

      // 低 fps を維持してフレームを読み捨て（カメラバッファを空にするため必要）
      const double fps = (watch_fps_ > 0.0 ? watch_fps_ : 2.0);
      const auto period = std::chrono::duration<double>(1.0 / fps);
      const auto dt = std::chrono::steady_clock::now() - t0;
      if (dt < period)
        std::this_thread::sleep_for(period - dt);

      ++frame_id;
      continue;
    }

    // ===== DETECT+TRACK =====
    const auto t_now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(t_now - t_prev).count();
    t_prev = t_now;

    const bool is_first_frame = first_detect_frame_;
    const cv::Size imsz = frame.size();
    const auto tp0 = std::chrono::steady_clock::now();
    cv::Mat inputImage = yolo_->preprocess(frame, YOLO_INPUT_H, YOLO_INPUT_W);
    const auto tp1 = std::chrono::steady_clock::now();
    std::vector<Ort::Value> outputTensors = yolo_->RunInference(inputImage);
    const auto tp2 = std::chrono::steady_clock::now();
    std::vector<Result> results = yolo_->postprocess(imsz, outputTensors);
    const auto tp3 = std::chrono::steady_clock::now();
    using ms = std::chrono::duration<double, std::milli>;
    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 1000,
                         "px2[timing] provider=%s pre=%.1fms infer=%.1fms post=%.1fms loop=%.1fms",
                         yolo_->activeProvider().c_str(), ms(tp1 - tp0).count(),
                         ms(tp2 - tp1).count(), ms(tp3 - tp2).count(), dt * 1000.0);

    RCLCPP_INFO(get_logger(), "px2[debug]: results.size()=%zu", results.size());

    cv::Rect2d box = vision::bbox::findBiggestBBox(results);
    RCLCPP_INFO(
        get_logger(),
        "px2[debug]: box.area()=%.2f box=(x=%.1f,y=%.1f,w=%.1f,h=%.1f) kMinBoxArea=%.2f -> %s",
        box.area(), box.x, box.y, box.width, box.height, kMinBoxArea,
        (box.area() >= kMinBoxArea ? "PASS(will track/save)" : "FAIL(no detection this frame)"));
    saveFrameIfNeeded(frame, box, frame_id, "undetect_");
    if (box.area() >= kMinBoxArea) {
      no_det_frames_ = 0;
      // 初回フレームは dt が不定のため微分項をスキップする
      const double effective_dt = first_detect_frame_ ? 0.0 : dt;
      first_detect_frame_ = false;

      const double yaw_err_deg = computeAxisErrorDeg(Axis::YAW, box);
      const double pitch_err_deg = computeAxisErrorDeg(Axis::PITCH, box);
      const bool yaw_in_dead = std::abs(yaw_err_deg) < kYawDeadZoneDeg;
      const bool pitch_in_dead = std::abs(pitch_err_deg) < kPitchDeadZoneDeg;

      if (!yaw_in_dead) {
        servo_x_deg_ = yaw_axis_.commit(yaw_err_deg, effective_dt);
      }
      if (!pitch_in_dead) {
        servo_y_deg_ = pitch_axis_.commit(pitch_err_deg, effective_dt);
      }
      if (!yaw_in_dead || !pitch_in_dead) {
        publishState(static_cast<float>(servo_x_deg_), static_cast<float>(servo_y_deg_));
      }

      // Detection log: bboxセンターと画像中央のズレを出力
      const cv::Point2f bbox_center = vision::bbox::bboxCenter(box);
      const cv::Point2f img_center(static_cast<float>(active_width_ / 2.0),
                                   static_cast<float>(active_height_ / 2.0));
      const float dist = cv::norm(bbox_center - img_center);
      RCLCPP_INFO(get_logger(), "px2: bbox_center=(%.1f,%.1f) img_center=(%.1f,%.1f) dist=%.1fpx",
                  bbox_center.x, bbox_center.y, img_center.x, img_center.y, dist);

      saveFrameIfNeeded(frame, box, frame_id, "detect_");

    } else {
      no_det_frames_++;
      if (no_det_frames_ >= lost_max_frames_)
        enterWatchMode();
    }

    // ---------- 計測 CSV ----------
    const auto tp4 = std::chrono::steady_clock::now();
    if (timing_csv_.is_open()) {
      float best_conf = 0.0f;
      for (const auto& r : results)
        best_conf = std::max(best_conf, r.accuracy);
      timing_csv_ << std::chrono::duration<double>(tp4 - timing_t0_).count() << ',' << frame_id
                  << ',' << yolo_->activeProvider() << ',' << (timing_fp16_ ? 1 : 0) << ','
                  << (is_first_frame ? 1 : 0) << ',' << ms(t_frame - t_iter_end).count() << ','
                  << ms(tp1 - tp0).count() << ',' << ms(tp2 - tp1).count() << ','
                  << ms(tp3 - tp2).count() << ',' << ms(tp4 - tp3).count() << ',' << dt * 1000.0
                  << ',' << results.size() << ',' << best_conf << ',' << box.area() << '\n';
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
//    px3_node (servo + PIR 管理)
//    px2_node (YOLO 推論 + DETECT+TRACK)
//    watch_node は別プロセス (px3 パッケージ)
// ============================================================
int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  try {
    auto px3_node = std::make_shared<px3_embedded::Px3ServoNode>(
        rclcpp::NodeOptions().use_global_arguments(false));
    auto px2_node = std::make_shared<onnx_inference::OnnxInferenceNode>();

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

#ifndef ONNX_INFERENCE_HPP_
#define ONNX_INFERENCE_HPP_

// ============================================================
//  inference.h  — OnnxInferenceNode (px2)
//
//  WATCH モード: PIR wakeup (/px_wakeup) を polling し待機
//  DETECT_TRACK モード: YOLO 推論 → PD 制御 → サーボ指令
// ============================================================

// --- Standard library ---
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// --- OpenCV ---
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

// --- ROS2 ---
#include "custom_msgs/msg/abs_result.hpp"
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/bool.hpp"

// --- Project ---
#include "control/axis_pd_controller.hpp"
#include "vision/bbox_geometry.hpp"
#include "vision/camera_calibration.hpp"
#include "vision/capture_settings.hpp"
#include "yolo_inference.h"

namespace onnx_inference {

static constexpr const char* kPkgPath = "/ros2_ws/work/src/px2/";

class OnnxInferenceNode : public rclcpp::Node {
public:
  explicit OnnxInferenceNode();
  ~OnnxInferenceNode() {
    if (pipeline_thread_.joinable())
      pipeline_thread_.join();
  }

private:
  enum class Mode { WATCH, DETECT_TRACK };
  enum class Axis { YAW, PITCH };

  // ===== ROS I/O =====
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::Publisher<custom_msgs::msg::AbsResult>::SharedPtr pub_abs_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr px3_ready_sub_;

  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr wakeup_pub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr wakeup_sub_;
  std::atomic<bool> wakeup_requested_{false};

  // ===== Config / resources =====
  std::string pkg_path = kPkgPath;
  std::string device_path;
  cv::VideoCapture cap_;
  std::unique_ptr<YoloDetect> yolo_;

  // ===== Flow / state =====
  std::atomic<bool> px3_ready_{false};
  bool started_ = false;
  std::thread pipeline_thread_;
  Mode mode_{Mode::WATCH};

  // Camera settings
  // watch_fps_: WATCH ループのスロットリング用（解像度は変更しない）
  double watch_fps_ = 10.0;

  // active_*: DETECT 時に適用する固定解像度 / fps
  int active_width_ = 1920;
  int active_height_ = 1080;
  double active_fps_ = 30.0; // メンバ化: コンストラクタと enterDetectTrackMode で同じ値を使う

  // COOLDOWN
  int no_det_frames_ = 0;

  // Servo state
  double servo_x_deg_ = 90.0;
  double servo_y_deg_ = 90.0;
  double x_center_deg_ = 90.0;
  double y_center_deg_ = 90.0;
  double y_max_ = 90.0;
  double y_min_ = 20.0;
  double x_max_ = 90.0;
  double x_min_ = 20.0;

  control::AxisPdController yaw_axis_;
  control::AxisPdController pitch_axis_;
  vision::CameraCalib calib_;

  // ===== Control params =====
  int lost_max_frames_ = 15;
  bool save_frames_ = false;

  bool first_detect_frame_{true}; // DETECT遷移直後の初回フレームフラグ

  // ===== 計測（timing_csv パラメータが空でなければ 1 フレーム 1 行を書き出す）=====
  std::ofstream timing_csv_;
  bool timing_fp16_ = false;
  int timing_rows_ = 0;
  std::chrono::steady_clock::time_point timing_t0_;
  static constexpr double kMinBoxArea = 1.0;
  static constexpr double kTargetYRatio = 0.5;
  static constexpr double kYawDeadZoneDeg = 2.0;   // 1.5 → 2.0
  static constexpr double kPitchDeadZoneDeg = 2.0; // 1.5 → 2.0

  // ===== Main pipeline =====
  void callbackInference();
  void runPipelineLoop();

  // helpers
  void enterWatchMode();
  void enterDetectTrackMode();
  void publishWakeup(bool on);
  // 軸指定でPD制御を計算（yaw/pitch共通処理）
  double computeAxisErrorDeg(Axis axis, const cv::Rect2d& box); // 戻り値: err_deg（副作用なし）
  void publishState(float yaw_deg, float pitch_deg);
  void saveFrameIfNeeded(const cv::Mat& frame, const cv::Rect2d& roi_img, int frame_id,
                         const std::string& name_prefix);
};

} // namespace onnx_inference
#endif // ONNX_INFERENCE_HPP_

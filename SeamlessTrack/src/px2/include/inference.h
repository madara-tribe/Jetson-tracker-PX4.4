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
#include <cstdint>
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
#include "control/command_history.hpp"
#include "vision/bbox_geometry.hpp"
#include "vision/camera_calibration.hpp"
#include "vision/capture_settings.hpp"
#include "vision/latest_frame_grabber.hpp"
#include "yolo_inference.h"

namespace onnx_inference {

class OnnxInferenceNode : public rclcpp::Node {
public:
  explicit OnnxInferenceNode();
  ~OnnxInferenceNode() {
    // 処理スレッドを先に止める（読み取りスレッドの結果を待っているため）。そのあと読み取りを止める
    if (pipeline_thread_.joinable())
      pipeline_thread_.join();
    grabber_.stop();
  }

  // 同じプロセスに同居する px3 に渡すパラメータ。
  // px3 は use_global_arguments(false) で作るので、launch の値は px3 に直接は届かない。
  // launch → px2 → （この関数）→ px3 の順で渡す。
  std::vector<rclcpp::Parameter> px3ParameterOverrides() const;

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
  // モデル・校正ファイル・検出画像の保存先。パラメータ model_path / calib_path / save_dir で
  // 指定する。空のときは、インストール先の share/px2/weights/ と /tmp/px2_frames を使う。
  // （以前は /ros2_ws/work/src/px2/ に固定で、コンテナの外では動かなかった）
  std::string model_path_;
  std::string calib_path_;
  std::string save_dir_;
  std::string device_path;
  cv::VideoCapture cap_;
  // カメラを別スレッドで読み続け、最新の 1 枚だけを使う（パラメータ capture_thread）
  bool use_capture_thread_ = true;
  vision::LatestFrameGrabber grabber_;
  uint64_t grab_seq_ = 0; // 最後に受け取ったフレームの通し番号
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
  int capture_buffer_size_ = 1; // カメラ側に溜めるフレーム数（1 = 最新に近いフレームを使う）

  // 見失い判定（秒）。最後に検出した時刻からこの時間が過ぎたら WATCH に戻る。
  // フレーム数ではなく時間で数えるので、ループの速さが変わっても同じ長さになる。
  double lost_timeout_s_ = 1.0;
  std::chrono::steady_clock::time_point last_det_t_;

  // px3 に渡すだけの値（px2 自身は使わない）
  std::string px3_serial_port_ = "/dev/ttyACM0";
  int px3_baudrate_ = 115200;
  int px3_pir_pin_ = 2;

  // 要求した推論デバイス（"trt" など）。CPU に落ちたことを警告するために覚えておく
  std::string requested_provider_;
  bool provider_fell_back_ = false;

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
  // 指令角の履歴。フレームを撮った時刻のサーボ角を引くために使う（段階 11 の準備）
  control::CommandHistory cmd_history_;
  // サーボが指令に追いつくまでの遅れ（ms）。履歴を引く時刻をこの分だけ前にずらす
  double servo_lag_ms_ = 0.0;
  vision::CameraCalib calib_;

  // ===== Control params =====
  bool save_frames_ = false;
  // 死帯（度）。誤差がこの値より小さければ動かさない。
  // サーボへの指令が整数（1° 刻み）のうちは 1.0 未満にしない。
  double yaw_dead_zone_deg_ = 1.0;
  double pitch_dead_zone_deg_ = 1.0;

  bool first_detect_frame_{true}; // DETECT遷移直後の初回フレームフラグ
  // 直前のフレームで検出できたか。できていなければ「前フレームとの差」が意味を持たないので
  // D 項を使わない（DETECT に入った直後、見失ったあとの最初の検出）。
  bool prev_frame_detected_{false};

  // ===== 計測（timing_csv パラメータが空でなければ 1 フレーム 1 行を書き出す）=====
  std::ofstream timing_csv_;
  bool timing_fp16_ = false;
  int timing_rows_ = 0;
  int session_id_ = 0; // DETECT に入るたびに 1 増える（CSV の session_id 列）
  std::chrono::steady_clock::time_point timing_t0_;
  static constexpr double kMinBoxArea = 1.0;
  static constexpr double kTargetYRatio = 0.5;

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
  // 次のフレームを受け取る。戻り値 false = カメラが読めなくなった（ループを抜ける）。
  // got=false は「まだ新しいフレームが無い」（WATCH 中、またはタイムアウト）。
  bool nextFrame(cv::Mat& frame, std::chrono::steady_clock::time_point& t_capture, bool& got);
  void saveFrameIfNeeded(const cv::Mat& frame, const cv::Rect2d& roi_img, int frame_id,
                         const std::string& name_prefix);
};

} // namespace onnx_inference
#endif // ONNX_INFERENCE_HPP_

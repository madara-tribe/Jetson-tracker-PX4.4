#pragma once

#include <algorithm>
#include <cmath>
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace control {

// Portable PI constant. Prefer this over M_PI (not guaranteed on all platforms).
inline constexpr double PX_PI = 3.14159265358979323846;

struct AxisPdController {
  double Kp = 0.0;
  double Kd = 0.0;
  double max_step_deg = 5.0;

  double center_deg = 90.0;
  double min_deg = 0.0;
  double max_deg = 180.0;

  double pos_deg = 0.0;
  double prev_err_deg = 0.0;

  // ---------------------------------------------------------------------------
  // validate() : call once after setting all parameters, before the first
  //              reset() / updateFromErrorRad() call.
  //              Throws std::invalid_argument if any parameter is ill-formed.
  // ---------------------------------------------------------------------------
  void validate(const std::string& axis_name = "") const {
    const std::string tag = axis_name.empty() ? "AxisPdController" : axis_name;

    if (Kp < 0.0)
      throw std::invalid_argument(tag + ": Kp must be >= 0, got " + std::to_string(Kp));

    if (Kd < 0.0)
      throw std::invalid_argument(tag + ": Kd must be >= 0, got " + std::to_string(Kd));

    if (max_step_deg <= 0.0)
      throw std::invalid_argument(tag + ": max_step_deg must be > 0, got " +
                                  std::to_string(max_step_deg));

    if (min_deg >= max_deg)
      throw std::invalid_argument(tag + ": min_deg (" + std::to_string(min_deg) +
                                  ") must be < max_deg (" + std::to_string(max_deg) + ")");

    if (center_deg < min_deg || center_deg > max_deg)
      throw std::invalid_argument(tag + ": center_deg (" + std::to_string(center_deg) +
                                  ") must be in [min_deg, max_deg]");
  }

  void reset(double pos_deg_init = 0.0) {
    pos_deg = pos_deg_init;
    prev_err_deg = 0.0;
  }

  // updateFromErrorRad()の「誤差計算(副作用なし)」と「状態コミット(副作用あり)」を分離
  double errorDeg(double err_rad) const {
    return err_rad * 180.0 / PX_PI;
  }

  // dead zone外と判定した時だけ呼ぶ。pos_deg/prev_err_degを実際に進める。
  double commit(double err_deg, double dt = 1.0) {
    const double d_term = (dt > 0.0) ? Kd * (err_deg - prev_err_deg) / dt : 0.0;

    double delta_deg = Kp * err_deg + d_term;
    delta_deg = std::clamp(delta_deg, -max_step_deg, max_step_deg);

    pos_deg += delta_deg; // ← dead zone内では呼ばれない = 実送信角度と一致し続ける
    prev_err_deg = err_deg;
    const double servo = center_deg + pos_deg;
    return std::clamp(servo, min_deg, max_deg);
  }
};

// ---------------------------------------------------------------------------
// Free functions — "inline" is sufficient for header-only definitions.
// "static inline" is redundant (static gives C-style internal linkage which
//  is already implied per-TU when the function is inline in a header).
// ---------------------------------------------------------------------------

inline double yawErrFromPixels(const cv::Point2f& pt_meas, const cv::Point2f& pt_tgt,
                               const cv::Mat& K, const cv::Mat& D) {
  std::vector<cv::Point2f> src{pt_meas, pt_tgt}, und;
  cv::undistortPoints(src, und, K, D, cv::noArray(), cv::noArray());
  const float dx = und[1].x - und[0].x; // positive = target is to the right
  return std::atan2(dx, 1.0);
}

inline double pitchErrFromPixels(const cv::Point2f& pt_meas, const cv::Point2f& pt_tgt,
                                 const cv::Mat& K, const cv::Mat& D) {
  std::vector<cv::Point2f> src{pt_meas, pt_tgt}, und;
  cv::undistortPoints(src, und, K, D, cv::noArray(), cv::noArray());
  const float dy = und[1].y - und[0].y; // positive = target is below
  return std::atan2(dy, 1.0);
}

} // namespace control

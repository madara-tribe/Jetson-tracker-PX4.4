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

  // 今のサーボ角（絶対角）から再開する。再検出のたびに中央へ戻らないようにするため。
  void resetAtServoDeg(double servo_deg) {
    pos_deg = std::clamp(servo_deg, min_deg, max_deg) - center_deg;
    prev_err_deg = 0.0;
  }

  // 今の指令角（絶対角）。commit() の戻り値と同じ値。
  double servoDeg() const {
    return std::clamp(center_deg + pos_deg, min_deg, max_deg);
  }

  double errorDeg(double err_rad) const {
    return err_rad * 180.0 / PX_PI;
  }

  // 死帯の中で動かさなかったフレームでも、誤差だけは覚えておく。
  // （覚えないと、死帯を出た最初のフレームで「古い誤差との差」が D 項に入って跳ねる）
  void observe(double err_deg) {
    prev_err_deg = err_deg;
  }

  // 死帯の外と判定したときだけ呼ぶ。pos_deg / prev_err_deg を進める。
  //   delta = Kp * err + Kd * (err - 前フレームの err)
  //
  // この制御は「位置に足していく」形なので、位置で見ると次のように効く。
  //   Kp : 実質的に積分のゲイン。サーボが動き終わるまで同じ誤差を毎フレーム足すので、
  //        遅延が長いほど行き過ぎる。上げる前に遅延を測ること
  //        （目安: 遅延 100 ms 以下なら 0.4 まで、165 ms を超えるなら 0.2。モデルでの見積もり）
  //   Kd : 実質的に比例のゲイン。0〜0.6 の範囲では追従誤差への影響は小さい
  //
  // Kd は「1 フレームあたりの誤差の変化」に掛ける（dt では割らない）。
  //   以前は dt で割っていたため、30 Hz では Kd=0.2 が実質 6.0 になり、
  //   ループが速いほど強くなっていた。
  // use_d=false: D 項を使わない（DETECT に入った最初のフレーム用）。
  double commit(double err_deg, bool use_d = true) {
    const double d_term = use_d ? Kd * (err_deg - prev_err_deg) : 0.0;

    double delta_deg = Kp * err_deg + d_term;
    delta_deg = std::clamp(delta_deg, -max_step_deg, max_step_deg);

    // 可動範囲の端で pos_deg だけが増え続けないよう、範囲内に収める（ワインドアップ防止）
    pos_deg = std::clamp(pos_deg + delta_deg, min_deg - center_deg, max_deg - center_deg);
    prev_err_deg = err_deg;
    return center_deg + pos_deg;
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

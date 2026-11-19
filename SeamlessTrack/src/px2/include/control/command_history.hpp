#pragma once
// ============================================================
//  command_history.hpp — サーボへの指令角の履歴
//
//  ねらい: 「フレームを撮った時刻に、サーボがどこを向いていたか」を引けるようにする。
//          撮影時のサーボ角 + 画像上の誤差 = 人の絶対角。
//          段階 11（予測と追跡）で、この絶対角を状態にして先を狙う。
//
//  注意: サーボは位置を読めないので、ここにあるのは「指令した角度」。
//        実際の角度は、指令より少し遅れる。その遅れは at() に渡す時刻をずらして補う
//        （px2 のパラメータ servo_lag_ms）。
//  スレッド: push() と at() は同じスレッド（px2 の処理スレッド）から呼ぶこと。
// ============================================================
#include <chrono>
#include <cstddef>
#include <deque>

namespace control {

class CommandHistory {
public:
  using Clock = std::chrono::steady_clock;

  struct Sample {
    Clock::time_point t;
    double x_deg;
    double y_deg;
  };

  explicit CommandHistory(double keep_seconds = 2.0, std::size_t max_samples = 512)
      : keep_(std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(keep_seconds))),
        max_samples_(max_samples) {
  }

  // 指令を送った時刻と角度を残す。時刻は前回以降であること。
  void push(Clock::time_point t, double x_deg, double y_deg) {
    samples_.push_back({t, x_deg, y_deg});
    // 古いものを捨てる。ただし最後の 1 件は必ず残す（長く動かなかったときも引けるように）
    while (samples_.size() > 1 && (samples_.size() > max_samples_ || t - samples_[1].t > keep_)) {
      samples_.pop_front();
    }
  }

  // 時刻 t の時点で有効だった指令（t 以前でいちばん新しいもの）。
  // t が履歴より前なら、いちばん古いものを返す。履歴が空なら false。
  bool at(Clock::time_point t, double& x_deg, double& y_deg) const {
    if (samples_.empty())
      return false;
    const Sample* best = &samples_.front();
    for (const auto& s : samples_) {
      if (s.t > t)
        break;
      best = &s;
    }
    x_deg = best->x_deg;
    y_deg = best->y_deg;
    return true;
  }

  std::size_t size() const {
    return samples_.size();
  }

private:
  Clock::duration keep_;
  std::size_t max_samples_;
  std::deque<Sample> samples_;
};

} // namespace control

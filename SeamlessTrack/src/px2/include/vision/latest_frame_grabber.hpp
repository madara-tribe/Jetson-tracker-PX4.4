#pragma once
// ============================================================
//  latest_frame_grabber.hpp — カメラを別スレッドで読み続け、最新の 1 枚だけを渡す
//
//  ねらい: 推論が 1 フレームより長くかかっても、カメラ側に溜まった古いフレームを
//          使わない。処理が終わった時点で「いちばん新しいフレーム」を受け取る。
//
//  - start() でスレッドが cap.read() を回し始める。stop() で止まる
//  - waitNext() は、前回渡したものより新しいフレームが来るまで待つ
//  - setDecode(false) の間は grab() だけ行う（JPEG の展開をしない）。
//    WATCH 中の CPU 負荷を抑えつつ、カメラ側のバッファを空にし続けるため
//  - スレッドが動いている間は、ほかのスレッドから cap を触らないこと
// ============================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace vision {

class LatestFrameGrabber {
public:
  using Clock = std::chrono::steady_clock;
  enum class Result { kFrame, kTimeout, kEnded };

  LatestFrameGrabber() = default;
  ~LatestFrameGrabber() {
    stop();
  }
  LatestFrameGrabber(const LatestFrameGrabber&) = delete;
  LatestFrameGrabber& operator=(const LatestFrameGrabber&) = delete;

  void start(cv::VideoCapture* cap) {
    if (thread_.joinable())
      return;
    cap_ = cap;
    run_.store(true);
    ended_.store(false);
    thread_ = std::thread(&LatestFrameGrabber::loop, this);
  }

  void stop() {
    run_.store(false);
    cv_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  bool running() const {
    return thread_.joinable() && !ended_.load();
  }

  // false にすると、フレームを取り出すだけで展開しない（waitNext には何も渡らない）
  void setDecode(bool on) {
    decode_.store(on);
  }

  // last_seq より新しいフレームを待つ。
  //   kFrame   : out / t_capture / last_seq を更新した
  //   kTimeout : timeout までに新しいフレームが来なかった
  //   kEnded   : カメラが読めなくなった（切断、動画の終わり）
  // t_capture は、読み取りスレッドが cap.read() から戻った時刻（撮影時刻にいちばん近い値）。
  Result waitNext(cv::Mat& out, Clock::time_point& t_capture, uint64_t& last_seq,
                  std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mtx_);
    const bool got =
        cv_.wait_for(lk, timeout, [&] { return seq_ > last_seq || ended_.load() || !run_.load(); });
    if (seq_ > last_seq) {
      front_.copyTo(out); // ロックの中でコピーする（読み取りスレッドと同じメモリを共有しない）
      t_capture = t_front_;
      skipped_ += (seq_ - last_seq - 1);
      last_seq = seq_;
      return Result::kFrame;
    }
    if (ended_.load() || !run_.load())
      return Result::kEnded;
    (void)got;
    return Result::kTimeout;
  }

  // 展開したが、使われずに次のフレームで上書きされた枚数（処理が追いつかなかった分）
  uint64_t skippedFrames() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return skipped_;
  }

private:
  void loop() {
    while (run_.load()) {
      if (!decode_.load()) {
        // WATCH 中: 取り出すだけ。カメラ側に古いフレームを溜めない
        if (!cap_->grab()) {
          ended_.store(true);
          break;
        }
        continue;
      }
      // back_ は常にこのスレッドだけが持つメモリ。読み終わってから front_ と入れ替える
      if (!cap_->read(back_) || back_.empty()) {
        ended_.store(true);
        break;
      }
      const auto t = Clock::now();
      {
        std::lock_guard<std::mutex> lk(mtx_);
        std::swap(front_, back_);
        t_front_ = t;
        ++seq_;
      }
      cv_.notify_all();
    }
    cv_.notify_all();
  }

  cv::VideoCapture* cap_ = nullptr;
  std::thread thread_;
  std::atomic<bool> run_{false};
  std::atomic<bool> ended_{false};
  std::atomic<bool> decode_{true};

  mutable std::mutex mtx_;
  std::condition_variable cv_;
  cv::Mat front_; // 最新のフレーム（mtx_ で保護）
  cv::Mat back_;  // 読み取り中のフレーム（読み取りスレッド専用）
  Clock::time_point t_front_{};
  uint64_t seq_ = 0;
  uint64_t skipped_ = 0;
};

} // namespace vision

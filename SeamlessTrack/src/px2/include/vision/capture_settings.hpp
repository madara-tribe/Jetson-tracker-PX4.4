#pragma once

#include <opencv2/videoio.hpp>
#include <rclcpp/rclcpp.hpp>

namespace vision::capture {

// buffer_size: カメラ側に溜めるフレーム数。1 にすると古いフレームが溜まりにくい。
//              0 以下なら指定しない（ドライバの既定値のまま）。
inline bool applyCameraSettingsToCap(cv::VideoCapture& cap, const rclcpp::Logger& logger, int width,
                                     int height, double fps, int buffer_size = 0) {
  if (!cap.isOpened()) {
    RCLCPP_WARN(logger, "Camera is not opened. applyCameraSettingsToCap skipped.");
    return false;
  }

  cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
  if (fps > 0.0)
    cap.set(cv::CAP_PROP_FPS, fps);
  if (width > 0)
    cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
  if (height > 0)
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);

  bool buffer_ok = true;
  if (buffer_size > 0)
    buffer_ok = cap.set(cv::CAP_PROP_BUFFERSIZE, buffer_size);

  const int rw = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  const int rh = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
  const double rfps = cap.get(cv::CAP_PROP_FPS);

  RCLCPP_INFO(logger, "Camera settings: %dx%d @ %.2f (requested %dx%d @ %.2f)", rw, rh, rfps, width,
              height, fps);
  if (buffer_size > 0) {
    if (buffer_ok) {
      RCLCPP_INFO(logger, "Camera buffer size: %d", buffer_size);
    } else {
      RCLCPP_WARN(logger, "Camera buffer size %d was not accepted by the driver", buffer_size);
    }
  }
  if (rw != width || rh != height) {
    RCLCPP_WARN(logger, "Camera did not accept %dx%d (got %dx%d)", width, height, rw, rh);
  }
  return true;
}

} // namespace vision::capture

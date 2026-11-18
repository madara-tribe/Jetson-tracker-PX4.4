#pragma once

#include <opencv2/videoio.hpp>
#include <rclcpp/rclcpp.hpp>

namespace vision::capture {

inline bool applyCameraSettingsToCap(cv::VideoCapture& cap, const rclcpp::Logger& logger, int width,
                                     int height, double fps) {
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

  const int rw = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  const int rh = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
  const double rfps = cap.get(cv::CAP_PROP_FPS);

  RCLCPP_INFO(logger, "Camera settings: %dx%d @ %.2f (requested %dx%d @ %.2f)", rw, rh, rfps, width,
              height, fps);
  return true;
}

} // namespace vision::capture

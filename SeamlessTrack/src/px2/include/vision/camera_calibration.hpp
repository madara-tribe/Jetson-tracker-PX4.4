#pragma once
#include <opencv2/core.hpp>
#include <opencv2/core/persistence.hpp>
#include <string>

namespace vision {

struct CameraCalib {
  cv::Mat K;       // scaled intrinsics for runtime size
  cv::Mat D;       // dist coeffs
  int calib_w = 0; // original calib image size
  int calib_h = 0;
  bool loaded = false;
};

inline bool loadCalibrationFile(const std::string& yaml_path, int runtime_w, int runtime_h,
                                CameraCalib& out) {
  cv::FileStorage fs(yaml_path, cv::FileStorage::READ);
  if (!fs.isOpened())
    return false;

  cv::Mat Kraw, Draw;
  int w = 0, h = 0;
  fs["camera_matrix"] >> Kraw;
  fs["dist_coeffs"] >> Draw;
  fs["image_width"] >> w;
  fs["image_height"] >> h;
  fs.release();

  if (Kraw.empty() || Draw.empty() || w <= 0 || h <= 0)
    return false;

  const double sx = double(runtime_w) / double(w);
  const double sy = double(runtime_h) / double(h);

  cv::Mat Ks = Kraw.clone();
  Ks.at<double>(0, 0) *= sx; // fx
  Ks.at<double>(1, 1) *= sy; // fy
  Ks.at<double>(0, 2) *= sx; // cx
  Ks.at<double>(1, 2) *= sy; // cy

  out.K = Ks;
  out.D = Draw.clone();
  out.calib_w = w;
  out.calib_h = h;
  out.loaded = true;
  return true;
}

} // namespace vision

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
  // キャリブレーション時と実行時で縦横比が違う（例: 4:3 で撮って 16:9 で使う）。
  // その場合は単純な拡大縮小では換算できず、角度がずれる。
  bool aspect_mismatch = false;
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
  out.aspect_mismatch =
      (static_cast<long long>(runtime_w) * h != static_cast<long long>(runtime_h) * w);
  out.loaded = true;
  return true;
}

} // namespace vision

#pragma once

#include <algorithm>
#include <cmath>
#include <opencv2/core.hpp>
#include <vector>

namespace vision::bbox {

// bbox center
inline cv::Point2f bboxCenter(const cv::Rect2d& b) {
  return {static_cast<float>(b.x + 0.5 * b.width), static_cast<float>(b.y + 0.5 * b.height)};
}

// Largest bbox selector by area.
// T must have members: x1,y1,x2,y2 (numeric).
template <class T> inline cv::Rect2d findBiggestBBox(const std::vector<T>& results) {
  if (results.empty())
    return cv::Rect2d(0, 0, 0, 0);

  cv::Rect2d best_bbox(0, 0, 0, 0);
  double maxArea = -1.0;

  for (const auto& r : results) {
    const double width = std::abs(static_cast<double>(r.x2) - static_cast<double>(r.x1));
    const double height = std::abs(static_cast<double>(r.y2) - static_cast<double>(r.y1));
    const double area = width * height;

    if (area > maxArea) {
      maxArea = area;
      best_bbox = cv::Rect2d(static_cast<double>(std::min(r.x1, r.x2)),
                             static_cast<double>(std::min(r.y1, r.y2)), width, height);
    }
  }
  return best_bbox;
}

} // namespace vision::bbox

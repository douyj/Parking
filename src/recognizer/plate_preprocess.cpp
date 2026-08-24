#include "plate_recognizer_internal.h"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

namespace plate_internal {

cv::Mat letterbox_rgb(const cv::Mat &bgr, float &ratio, int &left, int &top) {
  ratio = std::min(kDetectSize / static_cast<float>(bgr.rows),
                   kDetectSize / static_cast<float>(bgr.cols));
  const int new_width = static_cast<int>(std::round(bgr.cols * ratio));
  const int new_height = static_cast<int>(std::round(bgr.rows * ratio));
  left = static_cast<int>(
      std::round((kDetectSize - new_width) / 2.0F - 0.1F));
  top = static_cast<int>(
      std::round((kDetectSize - new_height) / 2.0F - 0.1F));
  const int right = kDetectSize - new_width - left;
  const int bottom = kDetectSize - new_height - top;

  cv::Mat resized;
  cv::Mat padded;
  cv::Mat rgb;
  cv::resize(bgr, resized, {new_width, new_height}, 0, 0, cv::INTER_LINEAR);
  cv::copyMakeBorder(resized, padded, top, bottom, left, right,
                     cv::BORDER_CONSTANT, {114, 114, 114});
  cv::cvtColor(padded, rgb, cv::COLOR_BGR2RGB);
  return rgb;
}

cv::Mat rectify_plate(const cv::Mat &image, const cv::Point2f points[4]) {
  const int width = std::max(1, static_cast<int>(std::max(
      cv::norm(points[2] - points[3]), cv::norm(points[1] - points[0]))));
  const int height = std::max(1, static_cast<int>(std::max(
      cv::norm(points[1] - points[2]), cv::norm(points[0] - points[3]))));
  const cv::Point2f destination[4] = {
      {0, 0}, {static_cast<float>(width - 1), 0},
      {static_cast<float>(width - 1), static_cast<float>(height - 1)},
      {0, static_cast<float>(height - 1)}};
  const cv::Mat transform = cv::getPerspectiveTransform(points, destination);
  cv::Mat output;
  cv::warpPerspective(image, output, transform, {width, height});
  return output;
}

cv::Mat merge_double_plate(const cv::Mat &image) {
  const int upper_height = std::max(1, image.rows * 5 / 12);
  const int lower_y = std::min(image.rows - 1, image.rows / 3);
  const cv::Mat upper = image(cv::Rect(0, 0, image.cols, upper_height));
  const cv::Mat lower = image(
      cv::Rect(0, lower_y, image.cols, image.rows - lower_y));
  cv::Mat resized_upper;
  cv::Mat result;
  cv::resize(upper, resized_upper, lower.size());
  cv::hconcat(resized_upper, lower, result);
  return result;
}

}  // namespace plate_internal

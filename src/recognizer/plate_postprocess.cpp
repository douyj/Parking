#include "plate_recognizer_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace plate_internal {
namespace {

const char *const kPlateCharacters[kRecClasses] = {
    "#", "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑", "苏", "浙",
    "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤", "桂", "琼", "川", "贵", "云",
    "藏", "陕", "甘", "青", "宁", "新", "学", "警", "港", "澳", "挂", "使", "领",
    "民", "航", "危", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A",
    "B", "C", "D", "E", "F", "G", "H", "J", "K", "L", "M", "N", "P", "Q", "R",
    "S", "T", "U", "V", "W", "X", "Y", "Z", "险", "品"};
const char *const kColorNames[kColorClasses] = {
    "黑色", "蓝色", "绿色", "白色", "黄色"};

float softmax_confidence(const std::vector<float> &values, int best_index) {
  const float maximum = *std::max_element(values.begin(), values.end());
  float sum = 0.0F;

  for (float value : values) {
    sum += std::exp(value - maximum);
  }

  return std::exp(values[best_index] - maximum) / sum;
}

}  // namespace

bool parse_detection(const float *prediction, float threshold, int image_width,
                     int image_height, float ratio, int left, int top,
                     plate_result_t &result, cv::Point2f points[4]) {
  if (prediction[4] < threshold) {
    return false;
  }

  const int plate_type = static_cast<int>(std::round(prediction[5]));

  if (plate_type != 0 && plate_type != 1) {
    return false;
  }

  result.x1 = std::clamp(
      static_cast<int>((prediction[0] - left) / ratio),
      0,
      image_width - 1);
  result.y1 = std::clamp(
      static_cast<int>((prediction[1] - top) / ratio),
      0,
      image_height - 1);
  result.x2 = std::clamp(
      static_cast<int>((prediction[2] - left) / ratio),
      0,
      image_width - 1);
  result.y2 = std::clamp(
      static_cast<int>((prediction[3] - top) / ratio),
      0,
      image_height - 1);
  result.detect_confidence = prediction[4];
  result.plate_type = plate_type;

  for (int index = 0; index < 4; ++index) {
    const float x = std::clamp((prediction[6 + index * 2] - left) / ratio,
                               0.0F, static_cast<float>(image_width - 1));
    const float y = std::clamp((prediction[7 + index * 2] - top) / ratio,
                               0.0F, static_cast<float>(image_height - 1));
    points[index].x = static_cast<int>(x);
    points[index].y = static_cast<int>(y);
    result.landmarks[index][0] = points[index].x;
    result.landmarks[index][1] = points[index].y;
  }
  return true;
}

std::string decode_plate(const std::vector<float> &character_output) {
  std::string plate_number;
  int previous_class = 0;
  for (int step = 0; step < kRecSteps; ++step) {
    const float *scores = character_output.data() + step * kRecClasses;
    const int best_class = static_cast<int>(
        std::max_element(scores, scores + kRecClasses) - scores);
    if (best_class != 0 && best_class != previous_class) {
      plate_number += kPlateCharacters[best_class];
    }
    previous_class = best_class;
  }
  return plate_number;
}

void decode_color(const std::vector<float> &color_output, plate_result_t &result) {
  const int color_index = static_cast<int>(
      std::max_element(color_output.begin(), color_output.end()) - color_output.begin());
  result.color = static_cast<plate_color_t>(color_index);
  result.color_confidence = softmax_confidence(color_output, color_index);
  std::snprintf(result.color_name, sizeof(result.color_name), "%s", kColorNames[color_index]);
}

}  // namespace plate_internal

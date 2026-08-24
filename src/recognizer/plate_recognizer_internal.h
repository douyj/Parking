#ifndef PLATE_RECOGNIZER_INTERNAL_H
#define PLATE_RECOGNIZER_INTERNAL_H

#include "plate_recognizer.h"
#include "rknn_api.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

// 内部命名空间，用于隐藏实现细节
namespace plate_internal {

constexpr int kDetectSize = 640;
constexpr int kDetectChannels = 14;
constexpr int kDetectRows = 300;
constexpr int kRecWidth = 168;
constexpr int kRecHeight = 48;
constexpr int kRecSteps = 21;
constexpr int kRecClasses = 78;
constexpr int kColorClasses = 5;

// RKNN模型类
class RknnModel {
 public:
  RknnModel() = default;
  ~RknnModel();

  RknnModel(const RknnModel &) = delete;
  RknnModel &operator=(const RknnModel &) = delete;

  bool load(const char *path, rknn_core_mask core, std::string &error);   // 加载模型

  // 推理模型
  bool infer(const cv::Mat &nhwc_u8,
             std::vector<std::vector<float>> &outputs,
             std::string &error);
  
  // 获取模型输出数量
  uint32_t output_count() const { return output_count_; }

 private:
  rknn_context context_ = 0;
  uint32_t output_count_ = 0;
};

// 字体类
struct Font {
  struct Impl;
  std::unique_ptr<Impl> implementation;

  Font();
  ~Font();
  Font(const Font &) = delete;
  Font &operator=(const Font &) = delete;

  bool load(const char *path);
  bool ready() const;
};

cv::Mat letterbox_rgb(const cv::Mat &bgr, float &ratio, int &left, int &top);
cv::Mat rectify_plate(const cv::Mat &image, const cv::Point2f points[4]);
cv::Mat merge_double_plate(const cv::Mat &image);

bool parse_detection(const float *prediction, float threshold, int image_width,
                     int image_height, float ratio, int left, int top,
                     plate_result_t &result, cv::Point2f points[4]);
std::string decode_plate(const std::vector<float> &character_output);
void decode_color(const std::vector<float> &color_output,
                  plate_result_t &result);

int draw_results(Font &font, unsigned char *bgr, int width, int height,
                 int stride, const plate_result_t *results, int count);

}  // namespace plate_internal

struct plate_recognizer {
  plate_internal::RknnModel detect;
  plate_internal::RknnModel recognition;
  plate_internal::Font font;
  std::string error;
};

#endif

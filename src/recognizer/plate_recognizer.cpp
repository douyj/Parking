#include "plate_recognizer_internal.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include <opencv2/imgproc.hpp>

using namespace plate_internal;

/*
  @brief 创建车牌识别器
  @param detect_path 检测模型路径
  @param recognition_path 识别模型路径
  @param font_path 字体路径
  @return 车牌识别器指针
  @note 模型路径和字体路径必须是有效的文件路径
*/
extern "C" plate_recognizer_t *plate_recognizer_create(
    const char *detect_path,
    const char *recognition_path,
    const char *font_path) {

  if (!detect_path || !recognition_path) {
    return nullptr;
  }

  auto recognizer = std::make_unique<plate_recognizer>(); //  智能指针

  // 加载检测模型
  if (!recognizer->detect.load(
          detect_path,
          RKNN_NPU_CORE_0,
          recognizer->error)) {
    return nullptr;
  }

  // 加载识别模型
  if (!recognizer->recognition.load(
          recognition_path,
          RKNN_NPU_CORE_1,
          recognizer->error)) {
    return nullptr;
  }

  // 检查模型输出数量
  if (recognizer->detect.output_count() != 1 ||
      recognizer->recognition.output_count() != 2) {
    return nullptr;
  }

  // 加载字体
  if (font_path && !recognizer->font.load(font_path)) {
    std::fprintf(
        stderr,
        "warning: cannot load font %s; visualization fallback is ASCII\n",
        font_path);
  }

  return recognizer.release();
}

/*
  @brief 整个识别过程函数
  @param recognizer 车牌识别器指针
  @param bgr 输入图像指针，BGR格式
  @param width 输入图像宽度
  @param height 输入图像高度
  @param stride 输入图像步长
  @param threshold 置信阈值
  @param results 输出结果数组指针
  @param max_results 最大结果数量
  @return 识别结果数量
*/
extern "C" int plate_recognizer_process(
    plate_recognizer_t *recognizer, const unsigned char *bgr, int width,
    int height, int stride, float threshold, plate_result_t *results,
    int max_results) {

      // 检查输入参数
  if (!recognizer || !bgr || width <= 0 || height <= 0 ||
      stride < width * 3 || !results || max_results <= 0) {
    return -1;
  }

  // 封装为 OpenCV 图像
  cv::Mat wrapped(
      height,
      width,
      CV_8UC3,    // 每个通道8位，3通道BGR
      const_cast<unsigned char *>(bgr), 
      stride);

  // 处理图像
  cv::Mat image = wrapped.isContinuous() ? wrapped : wrapped.clone();

  // 检测模型预处理
  float ratio = 0.0F;
  int left = 0;
  int top = 0;
  cv::Mat detect_input = letterbox_rgb(image, ratio, left, top);    //letterbox_rgb() 会将原始图片按比例缩放到检测模型要求的尺寸，同时填充空白区域。

  // 执行车牌检测
  std::vector<std::vector<float>> detect_outputs;
  if (!recognizer->detect.infer(detect_input, detect_outputs, recognizer->error))   //这里调用检测模型进行 RKNN NPU 推理
  {
    return -2;
  }

  // 检查检测模型输出尺寸
  if (detect_outputs.empty() || detect_outputs[0].size() != kDetectRows * kDetectChannels) 
  {
    const size_t actual = detect_outputs.empty() ? 0 : detect_outputs[0].size();
    recognizer->error = "unexpected detector output size: " + std::to_string(actual);
    return -3;
  }

  // 遍历检测结果
  int count = 0;
  const int limit = std::min(max_results, PLATE_MAX_RESULTS);
  for (int row = 0; row < kDetectRows && count < limit; ++row) {
    const float *prediction = detect_outputs[0].data() + row * kDetectChannels;
    plate_result_t result{};
    cv::Point2f points[4];
    if (!parse_detection(   //parse_detection() 负责解析检测模型输出，提取车牌位置和类型信息, 填充 result 和 points
            prediction,
            threshold,
            width,
            height,
            ratio,
            left,
            top,
            result,
            points)) {
      continue;
    }

    // 透视矫正车牌
    cv::Mat plate_image = rectify_plate(image, points);

    if (plate_image.empty()) {
      continue;
    }

    // 处理双层车牌
    if (result.plate_type == 1) {
      plate_image = merge_double_plate(plate_image);
    }

    // 调整识别模型输入尺寸
    cv::Mat recognition_input;
    cv::resize(plate_image, recognition_input, {kRecWidth, kRecHeight});
    //  执行字符和颜色识别
    std::vector<std::vector<float>> recognition_outputs;
    if (!recognizer->recognition.infer(
            recognition_input,
            recognition_outputs,
            recognizer->error)) {
      return -4;
    }

    // 识别模型一次推理产生两个输出，0：字符预测，1：颜色预测
    if (recognition_outputs.size() != 2 ||
        recognition_outputs[0].size() != kRecSteps * kRecClasses ||
        recognition_outputs[1].size() != kColorClasses) {
      recognizer->error = "unexpected recognizer output size";
      return -5;
    }

    // 解码字符和颜色
    const std::string plate_number = decode_plate(recognition_outputs[0]);
    decode_color(recognition_outputs[1], result);
    std::snprintf(result.plate_number, sizeof(result.plate_number), "%s",
                  plate_number.c_str());
    results[count++] = result;
  }
  return count;
}

/*
  @brief 绘制车牌识别结果
  @param recognizer 车牌识别器指针
  @param bgr 输入图像指针，BGR格式
  @param width 输入图像宽度
  @param height 输入图像高度
  @param stride 输入图像步长
  @param results 识别结果数组指针
  @param count 识别结果数量
  @return 0 成功，-1 失败
*/
extern "C" int plate_recognizer_draw(
    plate_recognizer_t *recognizer, unsigned char *bgr, int width, int height,
    int stride, const plate_result_t *results, int count) {

  if (!recognizer) {
    return -1;
  }

  return draw_results(
      recognizer->font,
      bgr,
      width,
      height,
      stride,
      results,
      count);
}

/*
  @brief 获取车牌识别器的错误信息
  @param recognizer 车牌识别器指针
  @return 错误信息字符串指针，必须由调用者释放
*/
extern "C" const char *plate_recognizer_last_error(
    const plate_recognizer_t *recognizer) {
  return recognizer ? recognizer->error.c_str() : "recognizer is null";
}

/*
  @brief 销毁车牌识别器
  @param recognizer 车牌识别器指针
*/
extern "C" void plate_recognizer_destroy(plate_recognizer_t *recognizer) {
  delete recognizer;
}

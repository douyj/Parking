#include "plate_recognizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>
#include "rknn_api.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

constexpr int kDetectSize = 640;
constexpr int kRecWidth = 168;
constexpr int kRecHeight = 48;
constexpr int kDetectChannels = 14;
constexpr int kDetectRows = 300;
constexpr int kRecSteps = 21;
constexpr int kRecClasses = 78;

const char *kPlateChars[kRecClasses] = {
    "#", "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑", "苏", "浙",
    "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤", "桂", "琼", "川", "贵", "云",
    "藏", "陕", "甘", "青", "宁", "新", "学", "警", "港", "澳", "挂", "使", "领",
    "民", "航", "危", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A",
    "B", "C", "D", "E", "F", "G", "H", "J", "K", "L", "M", "N", "P", "Q", "R",
    "S", "T", "U", "V", "W", "X", "Y", "Z", "险", "品"};
const char *kColorNames[5] = {"黑色", "蓝色", "绿色", "白色", "黄色"};

struct Model {
  rknn_context ctx = 0;
  uint32_t output_count = 0;

  ~Model() { if (ctx) rknn_destroy(ctx); }

  bool load(const char *path, rknn_core_mask core, std::string &error) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { error = std::string("cannot open model: ") + path; return false; }
    const auto length = stream.tellg();
    if (length <= 0) { error = std::string("empty model: ") + path; return false; }
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), length);
    int ret = rknn_init(&ctx, bytes.data(), static_cast<uint32_t>(bytes.size()), 0, nullptr);
    if (ret != RKNN_SUCC) { error = "rknn_init failed: " + std::to_string(ret); return false; }
    ret = rknn_set_core_mask(ctx, core);
    if (ret != RKNN_SUCC) std::fprintf(stderr, "warning: rknn_set_core_mask failed: %d\n", ret);
    rknn_input_output_num io{};
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    if (ret != RKNN_SUCC || io.n_input != 1) { error = "unexpected RKNN model I/O"; return false; }
    output_count = io.n_output;
    return true;
  }

  bool infer(const cv::Mat &nhwc_u8, std::vector<std::vector<float>> &result, std::string &error) {
    if (!nhwc_u8.isContinuous() || nhwc_u8.type() != CV_8UC3) {
      error = "RKNN input must be continuous CV_8UC3";
      return false;
    }
    rknn_input input{};
    input.index = 0;
    input.buf = nhwc_u8.data;
    input.size = static_cast<uint32_t>(nhwc_u8.total() * nhwc_u8.elemSize());
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    int ret = rknn_inputs_set(ctx, 1, &input);
    if (ret != RKNN_SUCC) { error = "rknn_inputs_set failed: " + std::to_string(ret); return false; }
    ret = rknn_run(ctx, nullptr);
    if (ret != RKNN_SUCC) { error = "rknn_run failed: " + std::to_string(ret); return false; }
    std::vector<rknn_output> outputs(output_count);
    for (uint32_t i = 0; i < output_count; ++i) { outputs[i].index = i; outputs[i].want_float = 1; }
    ret = rknn_outputs_get(ctx, output_count, outputs.data(), nullptr);
    if (ret != RKNN_SUCC) { error = "rknn_outputs_get failed: " + std::to_string(ret); return false; }
    result.clear();
    result.reserve(output_count);
    for (const auto &output : outputs) {
      const size_t count = output.size / sizeof(float);
      const float *data = static_cast<const float *>(output.buf);
      result.emplace_back(data, data + count);
    }
    rknn_outputs_release(ctx, output_count, outputs.data());
    return true;
  }
};

struct Font {
  std::vector<unsigned char> bytes;
  stbtt_fontinfo info{};
  bool ready = false;

  bool load(const char *path) {
    if (!path || !*path) return false;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    auto length = stream.tellg();
    bytes.resize(static_cast<size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), length);
    ready = stbtt_InitFont(&info, bytes.data(), stbtt_GetFontOffsetForIndex(bytes.data(), 0)) != 0;
    return ready;
  }
};

uint32_t next_utf8(const char *&p) {
  const unsigned char c = static_cast<unsigned char>(*p++);
  if (c < 0x80) return c;
  if ((c >> 5) == 0x6) { uint32_t r = c & 0x1f; r = (r << 6) | (*p++ & 0x3f); return r; }
  if ((c >> 4) == 0xe) { uint32_t r = c & 0x0f; r = (r << 6) | (*p++ & 0x3f); r = (r << 6) | (*p++ & 0x3f); return r; }
  if ((c >> 3) == 0x1e) { uint32_t r = c & 7; for (int i = 0; i < 3; ++i) r = (r << 6) | (*p++ & 0x3f); return r; }
  return '?';
}

int text_width(Font &font, const std::string &text, int pixel_height) {
  if (!font.ready) return static_cast<int>(text.size()) * pixel_height / 2;
  float scale = stbtt_ScaleForPixelHeight(&font.info, static_cast<float>(pixel_height));
  int width = 0;
  const char *p = text.c_str();
  while (*p) { int advance, bearing; stbtt_GetCodepointHMetrics(&font.info, next_utf8(p), &advance, &bearing); width += static_cast<int>(advance * scale); }
  return width;
}

void draw_utf8(cv::Mat &image, Font &font, const std::string &text, int x, int baseline,
               int pixel_height, const cv::Scalar &bgr) {
  if (!font.ready) { cv::putText(image, text, {x, baseline}, cv::FONT_HERSHEY_SIMPLEX, 0.55, bgr, 1, cv::LINE_AA); return; }
  float scale = stbtt_ScaleForPixelHeight(&font.info, static_cast<float>(pixel_height));
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &gap);
  const char *p = text.c_str();
  int pen_x = x;
  while (*p) {
    uint32_t cp = next_utf8(p);
    int x0, y0, x1, y1, advance, bearing;
    stbtt_GetCodepointBitmapBox(&font.info, static_cast<int>(cp), scale, scale, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;
    if (gw > 0 && gh > 0) {
      std::vector<unsigned char> bitmap(static_cast<size_t>(gw * gh));
      stbtt_MakeCodepointBitmap(&font.info, bitmap.data(), gw, gh, gw, scale, scale, static_cast<int>(cp));
      int start_x = pen_x + x0, start_y = baseline + y0;
      for (int yy = 0; yy < gh; ++yy) for (int xx = 0; xx < gw; ++xx) {
        int px = start_x + xx, py = start_y + yy;
        if (px < 0 || py < 0 || px >= image.cols || py >= image.rows) continue;
        float a = bitmap[yy * gw + xx] / 255.0f;
        auto &dst = image.at<cv::Vec3b>(py, px);
        for (int c = 0; c < 3; ++c) dst[c] = static_cast<unsigned char>(dst[c] * (1 - a) + bgr[c] * a);
      }
    }
    stbtt_GetCodepointHMetrics(&font.info, static_cast<int>(cp), &advance, &bearing);
    pen_x += static_cast<int>(advance * scale);
  }
}

cv::Mat letterbox_rgb(const cv::Mat &bgr, float &ratio, int &left, int &top) {
  ratio = std::min(kDetectSize / static_cast<float>(bgr.rows), kDetectSize / static_cast<float>(bgr.cols));
  int nw = static_cast<int>(std::round(bgr.cols * ratio));
  int nh = static_cast<int>(std::round(bgr.rows * ratio));
  left = static_cast<int>(std::round((kDetectSize - nw) / 2.0f - 0.1f));
  top = static_cast<int>(std::round((kDetectSize - nh) / 2.0f - 0.1f));
  int right = kDetectSize - nw - left, bottom = kDetectSize - nh - top;
  cv::Mat resized, padded, rgb;
  cv::resize(bgr, resized, {nw, nh}, 0, 0, cv::INTER_LINEAR);
  cv::copyMakeBorder(resized, padded, top, bottom, left, right, cv::BORDER_CONSTANT, {114, 114, 114});
  cv::cvtColor(padded, rgb, cv::COLOR_BGR2RGB);
  return rgb;
}

cv::Mat rectify(const cv::Mat &image, const cv::Point2f p[4]) {
  int width = std::max(1, static_cast<int>(std::max(cv::norm(p[2] - p[3]), cv::norm(p[1] - p[0]))));
  int height = std::max(1, static_cast<int>(std::max(cv::norm(p[1] - p[2]), cv::norm(p[0] - p[3]))));
  cv::Point2f dst[4] = {{0, 0}, {static_cast<float>(width - 1), 0},
                         {static_cast<float>(width - 1), static_cast<float>(height - 1)},
                         {0, static_cast<float>(height - 1)}};
  cv::Mat transform = cv::getPerspectiveTransform(p, dst), output;
  cv::warpPerspective(image, output, transform, {width, height});
  return output;
}

cv::Mat merge_double(const cv::Mat &image) {
  int upper_h = std::max(1, image.rows * 5 / 12);
  int lower_y = std::min(image.rows - 1, image.rows / 3);
  cv::Mat upper = image(cv::Rect(0, 0, image.cols, upper_h));
  cv::Mat lower = image(cv::Rect(0, lower_y, image.cols, image.rows - lower_y));
  cv::Mat resized, result;
  cv::resize(upper, resized, lower.size());
  cv::hconcat(resized, lower, result);
  return result;
}

float softmax_conf(const std::vector<float> &v, int best) {
  float maximum = *std::max_element(v.begin(), v.end()), sum = 0;
  for (float x : v) sum += std::exp(x - maximum);
  return std::exp(v[best] - maximum) / sum;
}

} // namespace

struct plate_recognizer {
  Model detect;
  Model rec;
  Font font;
  std::string error;
};

extern "C" plate_recognizer_t *plate_recognizer_create(const char *detect_path,
                                                          const char *rec_path,
                                                          const char *font_path) {
  if (!detect_path || !rec_path) return nullptr;
  auto recognizer = std::make_unique<plate_recognizer>();
  if (!recognizer->detect.load(detect_path, RKNN_NPU_CORE_0, recognizer->error)) return nullptr;
  if (!recognizer->rec.load(rec_path, RKNN_NPU_CORE_1, recognizer->error)) return nullptr;
  if (recognizer->detect.output_count != 1 || recognizer->rec.output_count != 2) return nullptr;
  if (font_path && !recognizer->font.load(font_path))
    std::fprintf(stderr, "warning: cannot load font %s; visualization fallback is ASCII\n", font_path);
  return recognizer.release();
}

extern "C" int plate_recognizer_process(plate_recognizer_t *r, const unsigned char *bgr,
                                          int width, int height, int stride, float threshold,
                                          plate_result_t *results, int max_results) {
  if (!r || !bgr || width <= 0 || height <= 0 || stride < width * 3 || !results || max_results <= 0) return -1;
  cv::Mat wrapped(height, width, CV_8UC3, const_cast<unsigned char *>(bgr), stride);
  cv::Mat image = wrapped.isContinuous() ? wrapped : wrapped.clone();
  float ratio; int left, top;
  cv::Mat input = letterbox_rgb(image, ratio, left, top);
  std::vector<std::vector<float>> detect_outputs;
  if (!r->detect.infer(input, detect_outputs, r->error)) return -2;
  if (detect_outputs.empty() || detect_outputs[0].size() != kDetectRows * kDetectChannels) {
    r->error = "unexpected detector output size: " + std::to_string(detect_outputs.empty() ? 0 : detect_outputs[0].size());
    return -3;
  }

  int count = 0;
  const auto &pred = detect_outputs[0];
  for (int row = 0; row < kDetectRows && count < std::min(max_results, PLATE_MAX_RESULTS); ++row) {
    const float *v = pred.data() + row * kDetectChannels;
    if (v[4] < threshold) continue;
    int type = static_cast<int>(std::round(v[5]));
    if (type != 0 && type != 1) continue;
    plate_result_t out{};
    out.x1 = std::clamp(static_cast<int>((v[0] - left) / ratio), 0, width - 1);
    out.y1 = std::clamp(static_cast<int>((v[1] - top) / ratio), 0, height - 1);
    out.x2 = std::clamp(static_cast<int>((v[2] - left) / ratio), 0, width - 1);
    out.y2 = std::clamp(static_cast<int>((v[3] - top) / ratio), 0, height - 1);
    out.detect_confidence = v[4]; out.plate_type = type;
    cv::Point2f points[4];
    for (int k = 0; k < 4; ++k) {
      points[k].x = std::clamp((v[6 + k * 2] - left) / ratio, 0.0f, static_cast<float>(width - 1));
      points[k].y = std::clamp((v[7 + k * 2] - top) / ratio, 0.0f, static_cast<float>(height - 1));
      points[k].x = static_cast<int>(points[k].x); points[k].y = static_cast<int>(points[k].y);
      out.landmarks[k][0] = points[k].x; out.landmarks[k][1] = points[k].y;
    }
    cv::Mat roi = rectify(image, points);
    if (roi.empty()) continue;
    if (type == 1) roi = merge_double(roi);
    cv::Mat rec_input;
    cv::resize(roi, rec_input, {kRecWidth, kRecHeight});
    std::vector<std::vector<float>> rec_outputs;
    if (!r->rec.infer(rec_input, rec_outputs, r->error)) return -4;
    if (rec_outputs.size() != 2 || rec_outputs[0].size() != kRecSteps * kRecClasses || rec_outputs[1].size() != 5) {
      r->error = "unexpected recognizer output size"; return -5;
    }
    std::string plate;
    int previous = 0;
    for (int step = 0; step < kRecSteps; ++step) {
      const float *logits = rec_outputs[0].data() + step * kRecClasses;
      int best = static_cast<int>(std::max_element(logits, logits + kRecClasses) - logits);
      if (best != 0 && best != previous) plate += kPlateChars[best];
      previous = best;
    }
    int color = static_cast<int>(std::max_element(rec_outputs[1].begin(), rec_outputs[1].end()) - rec_outputs[1].begin());
    out.color = static_cast<plate_color_t>(color);
    out.color_confidence = softmax_conf(rec_outputs[1], color);
    std::snprintf(out.plate_number, sizeof(out.plate_number), "%s", plate.c_str());
    std::snprintf(out.color_name, sizeof(out.color_name), "%s", kColorNames[color]);
    results[count++] = out;
  }
  return count;
}

extern "C" int plate_recognizer_draw(plate_recognizer_t *r, unsigned char *bgr,
                                       int width, int height, int stride,
                                       const plate_result_t *results, int count) {
  if (!r || !bgr || !results || count < 0) return -1;
  cv::Mat image(height, width, CV_8UC3, bgr, stride);
  const cv::Scalar point_colors[4] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 255, 255}};
  for (int i = 0; i < count; ++i) {
    const auto &d = results[i];
    cv::Scalar color = d.color == PLATE_COLOR_GREEN ? cv::Scalar(128, 222, 74) :
                       d.color == PLATE_COLOR_BLUE ? cv::Scalar(250, 165, 96) : cv::Scalar(21, 204, 250);
    cv::Mat glow = cv::Mat::zeros(image.size(), image.type());
    cv::rectangle(glow, {d.x1, d.y1}, {d.x2, d.y2}, color, 3);
    cv::GaussianBlur(glow, glow, {0, 0}, 2.0);
    cv::addWeighted(glow, 0.5, image, 1.0, 0, image);
    cv::rectangle(image, {d.x1, d.y1}, {d.x2, d.y2}, color, 2);
    int corner = std::clamp(std::min(d.x2 - d.x1, d.y2 - d.y1) * 3 / 10, 8, 20);
    for (int sx : {-1, 1}) for (int sy : {-1, 1}) {
      int x = sx < 0 ? d.x1 : d.x2, y = sy < 0 ? d.y1 : d.y2;
      cv::line(image, {x, y}, {x - sx * corner, y}, color, 3);
      cv::line(image, {x, y}, {x, y - sy * corner}, color, 3);
    }
    for (int k = 0; k < 4; ++k) cv::circle(image, {static_cast<int>(d.landmarks[k][0]), static_cast<int>(d.landmarks[k][1])}, 4, point_colors[k], -1);
    char confidence[16]; std::snprintf(confidence, sizeof(confidence), "%.1f%%", d.detect_confidence * 100.0f);
    std::string label = std::string(d.plate_number) + " | " + d.color_name + " | " + confidence;
    int font_size = 23, card_h = 38, card_w = std::min(width - 4, std::max(180, text_width(r->font, label, font_size) + 20));
    int card_x = std::clamp((d.x1 + d.x2 - card_w) / 2, 2, std::max(2, width - card_w - 2));
    int card_y = d.y1 >= card_h + 5 ? d.y1 - card_h - 3 : std::min(height - card_h - 2, d.y2 + 3);
    cv::Mat overlay = image.clone();
    cv::rectangle(overlay, {card_x, card_y}, {card_x + card_w, card_y + card_h}, {0, 49, 74}, -1);
    cv::addWeighted(overlay, 0.78, image, 0.22, 0, image);
    cv::rectangle(image, {card_x, card_y}, {card_x + card_w, card_y + card_h}, color, 1);
    draw_utf8(image, r->font, label, card_x + 10, card_y + 29, font_size, {195, 249, 254});
  }
  return 0;
}

extern "C" const char *plate_recognizer_last_error(const plate_recognizer_t *r) {
  return r ? r->error.c_str() : "recognizer is null";
}

extern "C" void plate_recognizer_destroy(plate_recognizer_t *r) { delete r; }

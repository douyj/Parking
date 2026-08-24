#include "plate_recognizer_internal.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include <opencv2/imgproc.hpp>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace plate_internal {

struct Font::Impl {
  std::vector<unsigned char> bytes;
  stbtt_fontinfo info{};
  bool loaded = false;
};

Font::Font() : implementation(std::make_unique<Impl>()) {}
Font::~Font() = default;

bool Font::load(const char *path) {
  if (!path || !*path) {
    return false;
  }

  std::ifstream stream(path, std::ios::binary | std::ios::ate);

  if (!stream) {
    return false;
  }

  const std::streamsize length = stream.tellg();
  implementation->bytes.resize(static_cast<size_t>(length));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(implementation->bytes.data()), length);
  implementation->loaded = stbtt_InitFont(
      &implementation->info, implementation->bytes.data(),
      stbtt_GetFontOffsetForIndex(implementation->bytes.data(), 0)) != 0;
  return implementation->loaded;
}

bool Font::ready() const { return implementation->loaded; }

namespace {

uint32_t next_utf8(const char *&text) {
  const unsigned char first = static_cast<unsigned char>(*text++);

  if (first < 0x80) {
    return first;
  }

  if ((first >> 5) == 0x6) {
    uint32_t result = first & 0x1f;
    return (result << 6) | (*text++ & 0x3f);
  }

  if ((first >> 4) == 0xe) {
    uint32_t result = first & 0x0f;
    result = (result << 6) | (*text++ & 0x3f);
    return (result << 6) | (*text++ & 0x3f);
  }
  if ((first >> 3) == 0x1e) {
    uint32_t result = first & 7;

    for (int index = 0; index < 3; ++index) {
      result = (result << 6) | (*text++ & 0x3f);
    }

    return result;
  }

  return '?';
}

int text_width(Font &font, const std::string &text, int pixel_height) {
  if (!font.ready()) {
    return static_cast<int>(text.size()) * pixel_height / 2;
  }

  const float scale = stbtt_ScaleForPixelHeight(
      &font.implementation->info, static_cast<float>(pixel_height));
  int width = 0;
  const char *cursor = text.c_str();
  while (*cursor) {
    int advance = 0;
    int bearing = 0;
    stbtt_GetCodepointHMetrics(&font.implementation->info, next_utf8(cursor),
                               &advance, &bearing);
    width += static_cast<int>(advance * scale);
  }
  return width;
}

void draw_utf8(cv::Mat &image, Font &font, const std::string &text, int x,
               int baseline, int pixel_height, const cv::Scalar &bgr) {
  if (!font.ready()) {
    cv::putText(image, text, {x, baseline}, cv::FONT_HERSHEY_SIMPLEX, 0.55,
                bgr, 1, cv::LINE_AA);
    return;
  }
  const float scale = stbtt_ScaleForPixelHeight(
      &font.implementation->info, static_cast<float>(pixel_height));
  const char *cursor = text.c_str();
  int pen_x = x;
  while (*cursor) {
    const uint32_t codepoint = next_utf8(cursor);
    int x0, y0, x1, y1, advance, bearing;
    stbtt_GetCodepointBitmapBox(&font.implementation->info, codepoint, scale,
                                scale, &x0, &y0, &x1, &y1);
    const int glyph_width = x1 - x0;
    const int glyph_height = y1 - y0;
    if (glyph_width > 0 && glyph_height > 0) {
      std::vector<unsigned char> bitmap(glyph_width * glyph_height);
      stbtt_MakeCodepointBitmap(&font.implementation->info, bitmap.data(),
          glyph_width, glyph_height, glyph_width, scale, scale, codepoint);
      for (int yy = 0; yy < glyph_height; ++yy) {
        for (int xx = 0; xx < glyph_width; ++xx) {
          const int px = pen_x + x0 + xx;
          const int py = baseline + y0 + yy;

          if (px < 0 || py < 0 || px >= image.cols || py >= image.rows) {
            continue;
          }

          const float alpha = bitmap[yy * glyph_width + xx] / 255.0F;
          cv::Vec3b &pixel = image.at<cv::Vec3b>(py, px);
          for (int channel = 0; channel < 3; ++channel) {
            pixel[channel] = static_cast<unsigned char>(
                pixel[channel] * (1 - alpha) + bgr[channel] * alpha);
          }
        }
      }
    }
    stbtt_GetCodepointHMetrics(&font.implementation->info, codepoint,
                               &advance, &bearing);
    pen_x += static_cast<int>(advance * scale);
  }
}

}  // namespace

int draw_results(Font &font, unsigned char *bgr, int width, int height,
                 int stride, const plate_result_t *results, int count) {
  if (!bgr || !results || count < 0) {
    return -1;
  }

  cv::Mat image(height, width, CV_8UC3, bgr, stride);
  const cv::Scalar point_colors[4] = {
      {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 255, 255}};

  for (int index = 0; index < count; ++index) {
    const plate_result_t &result = results[index];
    const cv::Scalar color = result.color == PLATE_COLOR_GREEN
        ? cv::Scalar(128, 222, 74)
        : result.color == PLATE_COLOR_BLUE ? cv::Scalar(250, 165, 96)
                                           : cv::Scalar(21, 204, 250);
    cv::Mat glow = cv::Mat::zeros(image.size(), image.type());
    cv::rectangle(
        glow,
        {result.x1, result.y1},
        {result.x2, result.y2},
        color,
        3);
    cv::GaussianBlur(glow, glow, {0, 0}, 2.0);
    cv::addWeighted(glow, 0.5, image, 1.0, 0, image);
    cv::rectangle(
        image,
        {result.x1, result.y1},
        {result.x2, result.y2},
        color,
        2);

    const int corner = std::clamp(
        std::min(result.x2 - result.x1, result.y2 - result.y1) * 3 / 10, 8, 20);
    for (int sx : {-1, 1}) {
      for (int sy : {-1, 1}) {
        const int x = sx < 0 ? result.x1 : result.x2;
        const int y = sy < 0 ? result.y1 : result.y2;
        cv::line(image, {x, y}, {x - sx * corner, y}, color, 3);
        cv::line(image, {x, y}, {x, y - sy * corner}, color, 3);
      }
    }
    for (int point = 0; point < 4; ++point) {
      cv::circle(image, {static_cast<int>(result.landmarks[point][0]),
                         static_cast<int>(result.landmarks[point][1])},
                 4, point_colors[point], -1);
    }

    char confidence[16];
    std::snprintf(confidence, sizeof(confidence), "%.1f%%",
                  result.detect_confidence * 100.0F);
    const std::string label = std::string(result.plate_number) + " | " +
                              result.color_name + " | " + confidence;
    const int font_size = 23;
    const int card_height = 38;
    const int card_width = std::min(
        width - 4, std::max(180, text_width(font, label, font_size) + 20));
    const int card_x = std::clamp((result.x1 + result.x2 - card_width) / 2,
                                  2, std::max(2, width - card_width - 2));
    const int card_y = result.y1 >= card_height + 5
        ? result.y1 - card_height - 3
        : std::min(height - card_height - 2, result.y2 + 3);
    cv::Mat overlay = image.clone();
    cv::rectangle(overlay, {card_x, card_y},
                  {card_x + card_width, card_y + card_height}, {0, 49, 74}, -1);
    cv::addWeighted(overlay, 0.78, image, 0.22, 0, image);
    cv::rectangle(image, {card_x, card_y},
                  {card_x + card_width, card_y + card_height}, color, 1);
    draw_utf8(image, font, label, card_x + 10, card_y + 29, font_size,
              {195, 249, 254});
  }
  return 0;
}

}  // namespace plate_internal

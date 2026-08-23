#include "plate_recognizer.h"

#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {
void swap_rgb_bgr(unsigned char *data, int width, int height)
{
    for (int i = 0; i < width * height; ++i) {
        const unsigned char temporary = data[i * 3];
        data[i * 3] = data[i * 3 + 2];
        data[i * 3 + 2] = temporary;
    }
}
} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "用法: %s <测试图片> [输出图片]\n", argv[0]);
        return 1;
    }
    const char *output_path = argc == 3 ? argv[2] : "recognition_result.jpg";
    int width = 0, height = 0, channels = 0;
    unsigned char *image = stbi_load(argv[1], &width, &height, &channels, 3);
    if (image == nullptr) {
        std::fprintf(stderr, "读取图片失败: %s\n", argv[1]);
        return 1;
    }
    swap_rgb_bgr(image, width, height);

    plate_recognizer_t *recognizer = plate_recognizer_create(
        "models/yolo26s-plate-detect-rk3576.rknn",
        "models/plate_rec_color-rk3576.rknn", "fonts/platech.ttf");
    if (recognizer == nullptr) {
        std::fprintf(stderr, "创建车牌识别器失败\n");
        stbi_image_free(image);
        return 2;
    }

    plate_result_t results[PLATE_MAX_RESULTS];
    const int count = plate_recognizer_process(
        recognizer, image, width, height, width * 3, 0.30F,
        results, PLATE_MAX_RESULTS);
    if (count < 0) {
        std::fprintf(stderr, "识别失败: code=%d, error=%s\n", count,
                     plate_recognizer_last_error(recognizer));
        plate_recognizer_destroy(recognizer);
        stbi_image_free(image);
        return 3;
    }

    std::printf("检测到 %d 个车牌\n", count);
    for (int i = 0; i < count; ++i) {
        const plate_result_t &result = results[i];
        std::printf("[%d] 车牌=%s, 颜色=%s, 检测置信度=%.2f, 颜色置信度=%.2f\n",
                    i, result.plate_number, result.color_name,
                    result.detect_confidence, result.color_confidence);
    }

    plate_recognizer_draw(recognizer, image, width, height, width * 3, results, count);
    swap_rgb_bgr(image, width, height);
    if (!stbi_write_jpg(output_path, width, height, 3, image, 95))
        std::fprintf(stderr, "保存识别结果失败: %s\n", output_path);
    else
        std::printf("结果已保存到 %s\n", output_path);

    plate_recognizer_destroy(recognizer);
    stbi_image_free(image);
    return 0;
}

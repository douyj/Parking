#include "image_convert.h"
#include "log.h"

#include <limits.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int clamp_color(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return value;
}

static int calculate_bgr_size(unsigned int width, unsigned int height,
                              size_t *size, unsigned int *stride)
{
    if (width == 0 || height == 0 || width > UINT_MAX / 3U ||
        (size_t)width > SIZE_MAX / height / 3U) {
        return -1;
    }
    *stride = width * 3U;
    *size = (size_t)(*stride) * height;
    return 0;
}

static int mjpeg_to_bgr(const CameraFrame *source,
                        image_bgr_frame_t *destination)
{
    int decoded_width;
    int decoded_height;
    int channels;
    unsigned char *bgr;
    size_t bgr_size;
    unsigned int stride;

    if (source->size == 0 || source->size > INT_MAX) {
        LOG_ERROR("MJPEG 帧长度无效: %zu", source->size);
        return -1;
    }
    bgr = stbi_load_from_memory((const stbi_uc *)source->data,
                                (int)source->size,
                                &decoded_width, &decoded_height,
                                &channels, 3);
    if (bgr == NULL || decoded_width <= 0 || decoded_height <= 0) {
        const char *reason = stbi_failure_reason();
        LOG_ERROR("MJPEG 解码失败: %s",
                  reason != NULL ? reason : "未知错误");
        stbi_image_free(bgr);
        return -1;
    }
    if (calculate_bgr_size((unsigned int)decoded_width,
                           (unsigned int)decoded_height,
                           &bgr_size, &stride) != 0) {
        LOG_ERROR("MJPEG 解码后的图像尺寸无效: %dx%d",
                  decoded_width, decoded_height);
        stbi_image_free(bgr);
        return -1;
    }

    for (size_t offset = 0; offset < bgr_size; offset += 3U) {
        unsigned char red = bgr[offset];
        bgr[offset] = bgr[offset + 2U];
        bgr[offset + 2U] = red;
    }

    destination->data = bgr;
    destination->size = bgr_size;
    destination->width = (unsigned int)decoded_width;
    destination->height = (unsigned int)decoded_height;
    destination->stride = stride;
    return 0;
}

static int yuyv_to_bgr(const CameraFrame *source,
                       image_bgr_frame_t *destination)
{
    unsigned char *bgr;
    size_t bgr_size;
    size_t required_source_size;
    unsigned int stride;

    if ((source->width & 1U) != 0 ||
        source->width > UINT_MAX / 2U ||
        source->bytes_per_line < source->width * 2U ||
        source->height > SIZE_MAX / source->bytes_per_line ||
        calculate_bgr_size(source->width, source->height,
                           &bgr_size, &stride) != 0) {
        LOG_ERROR("YUYV 帧参数无效");
        return -1;
    }
    required_source_size =
        (size_t)source->bytes_per_line * source->height;
    if (source->size < required_source_size) {
        LOG_ERROR("YUYV 帧长度不足: bytes=%zu, required=%zu",
                  source->size, required_source_size);
        return -1;
    }

    bgr = malloc(bgr_size);
    if (bgr == NULL) {
        LOG_ERROR("分配 BGR 缓冲区失败: %zu bytes", bgr_size);
        return -1;
    }

    for (unsigned int row = 0; row < source->height; ++row) {
        const unsigned char *input =
            (const unsigned char *)source->data +
            (size_t)row * source->bytes_per_line;
        unsigned char *output = bgr + (size_t)row * stride;

        for (unsigned int column = 0;
             column < source->width; column += 2U) {
            const int y0 = input[0];
            const int u = input[1] - 128;
            const int y1 = input[2];
            const int v = input[3] - 128;
            const int red_offset = (359 * v) >> 8;
            const int green_offset = (-88 * u - 183 * v) >> 8;
            const int blue_offset = (454 * u) >> 8;

            output[0] = (unsigned char)clamp_color(y0 + blue_offset);
            output[1] = (unsigned char)clamp_color(y0 + green_offset);
            output[2] = (unsigned char)clamp_color(y0 + red_offset);
            output[3] = (unsigned char)clamp_color(y1 + blue_offset);
            output[4] = (unsigned char)clamp_color(y1 + green_offset);
            output[5] = (unsigned char)clamp_color(y1 + red_offset);
            input += 4;
            output += 6;
        }
    }

    destination->data = bgr;
    destination->size = bgr_size;
    destination->width = source->width;
    destination->height = source->height;
    destination->stride = stride;
    return 0;
}

int image_convert_to_bgr(const CameraFrame *source,
                         image_bgr_frame_t *destination)
{
    if (destination == NULL) {
        LOG_ERROR("图像转换输出参数无效");
        return -1;
    }
    memset(destination, 0, sizeof(*destination));
    if (source == NULL || source->data == NULL ||
        source->width == 0 || source->height == 0) {
        LOG_ERROR("图像转换参数无效");
        return -1;
    }

    if (source->pixel_format == V4L2_PIX_FMT_MJPEG)
        return mjpeg_to_bgr(source, destination);
    if (source->pixel_format == V4L2_PIX_FMT_YUYV)
        return yuyv_to_bgr(source, destination);

    LOG_ERROR("暂不支持摄像头像素格式: 0x%08x",
              source->pixel_format);
    return -1;
}

void image_bgr_frame_release(image_bgr_frame_t *frame)
{
    if (frame == NULL)
        return;
    free(frame->data);
    memset(frame, 0, sizeof(*frame));
}

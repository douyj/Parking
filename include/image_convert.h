#ifndef IMAGE_CONVERT_H
#define IMAGE_CONVERT_H

#include "camera.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned char *data;
    size_t size;
    unsigned int width;
    unsigned int height;
    unsigned int stride;
} image_bgr_frame_t;

/* 将 MJPG 或 YUYV CameraFrame 转换为调用者拥有的 BGR888 图像。 */
int image_convert_to_bgr(const CameraFrame *source,
                         image_bgr_frame_t *destination);

/* 释放 image_convert_to_bgr() 分配的图像数据并清空结构体。 */
void image_bgr_frame_release(image_bgr_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif

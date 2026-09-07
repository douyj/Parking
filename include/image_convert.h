#ifndef IMAGE_CONVERT_H
#define IMAGE_CONVERT_H

#include "camera.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// BGR888 图像数据结构体
typedef struct {
    unsigned char *data;        // 真正的图像像素数据
    size_t size;                // 整张图像数据一共有多少字节
    unsigned int width;         // 图像宽度
    unsigned int height;        // 图像高度
    unsigned int stride;        // 图像每一行在内存里实际占多少字节
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

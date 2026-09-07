#ifndef PIPELINE_FRAME_H
#define PIPELINE_FRAME_H

#include "image_convert.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    image_bgr_frame_t image;     // 图像数据
    uint64_t started_us;         // 开始时间，单位微秒
    uint64_t convert_us;        // 转换时间，单位微秒
    uint64_t recognize_us;      // 识别时间，单位微秒
} pipeline_frame_t;

/* 释放帧持有的 BGR 图像，并将整个结构体清零。 */
void pipeline_frame_release(pipeline_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif

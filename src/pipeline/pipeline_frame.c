#include "pipeline_frame.h"

#include <string.h>

void pipeline_frame_release(pipeline_frame_t *frame)
{
    if (frame == NULL)
        return;

    image_bgr_frame_release(&frame->image);     // 释放帧持有的 BGR 图像
    memset(frame, 0, sizeof(*frame));
}
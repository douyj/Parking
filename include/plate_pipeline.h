#ifndef PLATE_PIPELINE_H
#define PLATE_PIPELINE_H

#include "camera.h"
#include "pipeline_frame.h"
#include "plate_confirmation.h"
#include "plate_recognizer.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct plate_pipeline plate_pipeline_t;

typedef enum {
    PLATE_PIPELINE_ERROR = -1,
    PLATE_PIPELINE_CLOSED = 0,
    PLATE_PIPELINE_OK = 1,
    PLATE_PIPELINE_TIMEOUT = 2
} plate_pipeline_result_t;

// 创建整个车牌 pipeline 时需要的配置
typedef struct {
    CameraConfig camera;

    const char *detect_model_path;          // 检测模型路径
    const char *recognition_model_path;     // 识别模型路径
    const char *font_path;                  // 字体路径

    float confidence_threshold;             // 置信阈值
    plate_confirmation_config_t confirmation;   // 车牌确认配置

    int camera_timeout_ms;                // 相机超时时间（毫秒）
    int frame_wait_ms;                    // pipeline 内部识别线程等待“最新帧”的时间
} plate_pipeline_config_t;

// 已经经过多帧确认后的稳定车牌事件
typedef struct {
    plate_result_t result;             // 识别结果
    uint64_t confirmed_at_us;          // 确认时间戳（微秒）
} plate_pipeline_event_t;

// 性能统计
typedef struct {
    uint64_t captured_frames;            // 已捕获的帧数
    uint64_t recognized_frames;          // 已识别的帧数
    uint64_t confirmed_events;           // 已确认的事件数

    uint64_t total_convert_us;           // 总转换时间（微秒）
    uint64_t total_recognize_us;         // 总识别时间（微秒）

    uint64_t capture_dropped_frames;     // 已丢弃的捕获帧数    
    uint64_t output_dropped_frames;      // 已丢弃的输出帧数
} plate_pipeline_stats_t;

plate_pipeline_t *plate_pipeline_create(const plate_pipeline_config_t *config);

int plate_pipeline_start(plate_pipeline_t *pipeline);

void plate_pipeline_stop(plate_pipeline_t *pipeline);

/*
 * 成功后，frame 中的 BGR 图像归调用者所有。
 * 调用者用完后必须调用 pipeline_frame_release()。
 */
int plate_pipeline_take_frame(plate_pipeline_t *pipeline, pipeline_frame_t *frame, int timeout_ms);

/* 非阻塞取出一个稳定车牌事件：1 有事件，0 无事件，-1 错误。 */
int plate_pipeline_take_event(plate_pipeline_t *pipeline, plate_pipeline_event_t *event);

/*
 * 返回最近一次“任意有效车牌识别”的单调时钟时间。
 * 尚未识别到有效车牌时返回 0。
 */
uint64_t
plate_pipeline_last_plate_seen_us(plate_pipeline_t *pipeline);

int plate_pipeline_get_stats(plate_pipeline_t *pipeline, plate_pipeline_stats_t *stats);

int plate_pipeline_failed(plate_pipeline_t *pipeline);

/* 把错误复制到调用者缓冲区，避免暴露跨线程修改的内部字符串。 */
int plate_pipeline_copy_last_error(plate_pipeline_t *pipeline, char *buffer, size_t capacity);

void plate_pipeline_destroy(plate_pipeline_t *pipeline);

#ifdef __cplusplus
}
#endif

#endif
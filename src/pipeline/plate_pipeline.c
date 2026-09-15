#include "plate_pipeline.h"

#include "image_convert.h"
#include "latest_frame_slot.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ERROR_MESSAGE_CAPACITY 256

struct plate_pipeline{
    plate_pipeline_config_t config;

    /* 深拷贝配置中的路径，避免保存调用者的临时指针。 */
    char camera_device[PATH_MAX];
    char detect_model_path[PATH_MAX];
    char recognition_model_path[PATH_MAX];
    char font_path[PATH_MAX];

    Camera camera;
    plate_recognizer_t *recognizer;
    plate_confirmation_t confirmation;

    latest_frame_slot_t *captured_frames;   // 已捕获的帧
    latest_frame_slot_t *annotated_frames;  // 画框后的帧

    pthread_t capture_thread;       // 捕获线程
    pthread_t recognition_thread;   // 识别线程

    pthread_mutex_t state_mutex;    // 状态互斥锁

    plate_pipeline_event_t pending_event;       // 待处理事件
    plate_pipeline_stats_t stats;               // 性能统计

    uint64_t last_plate_seen_us;              // 最后一次看到车牌的时间戳（微秒）

    char last_error[ERROR_MESSAGE_CAPACITY];     // 最后一次错误信息

    int has_pending_event;                  // 是否有待处理事件
    int stop_requested;                     // 是否请求停止
    int failed;                             // 是否 failed

    int mutex_initialized;                  // 是否初始化互斥锁
    int camera_opened;                      // 是否打开相机
    int camera_started;                   // 是否启动相机
    int capture_thread_started;             // 是否启动捕获线程
    int recognition_thread_started;         // 是否启动识别线程
    int start_called;                       // 是否调用 start 方法
};

static uint64_t monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;

    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static int copy_config_string(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (destination == NULL || capacity == 0 ||
        source == NULL || source[0] == '\0')
        return -1;

    length = strlen(source);
    if (length >= capacity)
        return -1;

    memcpy(destination, source, length + 1U);
    return 0;
}

static int validate_config(const plate_pipeline_config_t *config)
{
    if (config == NULL || config->camera.device == NULL ||
        config->detect_model_path == NULL ||
        config->recognition_model_path == NULL ||
        config->font_path == NULL)
        return -1;

    if (config->camera.device[0] == '\0' ||
        config->detect_model_path[0] == '\0' ||
        config->recognition_model_path[0] == '\0' ||
        config->font_path[0] == '\0')
        return -1;

    if (config->camera.width == 0 || config->camera.height == 0 ||
        config->camera.fps == 0)
        return -1;

    if (config->camera.pixel_format != V4L2_PIX_FMT_MJPEG &&
        config->camera.pixel_format != V4L2_PIX_FMT_YUYV)
        return -1;

    if (config->confidence_threshold < 0.0F ||
        config->confidence_threshold > 1.0F)
        return -1;

    if (config->camera_timeout_ms <= 0 || config->frame_wait_ms <= 0)
        return -1;

    if (config->confirmation.required_frames == 0 ||
        config->confirmation.max_missed_frames == 0)
        return -1;

    return 0;
}

static int pipeline_should_stop(plate_pipeline_t *pipeline)
{
    int stop_requested;

    pthread_mutex_lock(&pipeline->state_mutex);
    stop_requested = pipeline->stop_requested;
    pthread_mutex_unlock(&pipeline->state_mutex);

    return stop_requested;
}

static void pipeline_request_stop(plate_pipeline_t *pipeline, int failed,
                                  const char *message)
{
    if (pipeline == NULL)
        return;

    pthread_mutex_lock(&pipeline->state_mutex);
    pipeline->stop_requested = 1;
    if (failed) {
        pipeline->failed = 1;
        if (message != NULL) {
            snprintf(pipeline->last_error, sizeof(pipeline->last_error),
                     "%s", message);
        }
    }
    pthread_mutex_unlock(&pipeline->state_mutex);

    /* 关闭帧槽会唤醒正在等待的工作线程。 */
    latest_frame_slot_close(pipeline->captured_frames);
    latest_frame_slot_close(pipeline->annotated_frames);
}

static const plate_result_t *find_best_result(
    const plate_result_t *results, int result_count)
{
    const plate_result_t *best = NULL;
    int i;

    for (i = 0; i < result_count; ++i) {
        if (results[i].plate_number[0] == '\0')
            continue;
        if (best == NULL ||
            results[i].detect_confidence > best->detect_confidence)
            best = &results[i];
    }

    return best;
}

static void pipeline_record_capture(plate_pipeline_t *pipeline,
                                    uint64_t convert_us)
{
    pthread_mutex_lock(&pipeline->state_mutex);
    ++pipeline->stats.captured_frames;
    pipeline->stats.total_convert_us += convert_us;
    pthread_mutex_unlock(&pipeline->state_mutex);
}

static void pipeline_record_recognition(plate_pipeline_t *pipeline,
                                        uint64_t recognize_us)
{
    pthread_mutex_lock(&pipeline->state_mutex);
    ++pipeline->stats.recognized_frames;
    pipeline->stats.total_recognize_us += recognize_us;
    pthread_mutex_unlock(&pipeline->state_mutex);
}

static void pipeline_mark_plate_seen(plate_pipeline_t *pipeline,
                                     uint64_t seen_us)
{
    pthread_mutex_lock(&pipeline->state_mutex);
    pipeline->last_plate_seen_us = seen_us;
    pthread_mutex_unlock(&pipeline->state_mutex);
}

static void pipeline_publish_plate_event(plate_pipeline_t *pipeline,
                                         const plate_result_t *result,
                                         uint64_t confirmed_at_us)
{
    pthread_mutex_lock(&pipeline->state_mutex);
    pipeline->pending_event.result = *result;
    pipeline->pending_event.confirmed_at_us = confirmed_at_us;
    pipeline->has_pending_event = 1;
    ++pipeline->stats.confirmed_events;
    pthread_mutex_unlock(&pipeline->state_mutex);
}

static void *capture_thread_main(void *argument)
{
    plate_pipeline_t *pipeline = argument;

    while (!pipeline_should_stop(pipeline)) {
        CameraFrame camera_frame;
        pipeline_frame_t output;
        uint64_t convert_started_us;
        uint64_t convert_us;
        int convert_result;
        int release_result;
        int push_result;

        memset(&camera_frame, 0, sizeof(camera_frame));
        memset(&output, 0, sizeof(output));
        output.started_us = monotonic_us();

        if (camera_get_frame(&pipeline->camera, &camera_frame,
                             pipeline->config.camera_timeout_ms) != 0) {
            if (!pipeline_should_stop(pipeline)) {
                LOG_ERROR("获取摄像头帧失败");
                pipeline_request_stop(pipeline, 1,
                                      "获取摄像头帧失败");
            }
            break;
        }

        convert_started_us = monotonic_us();
        convert_result = image_convert_to_bgr(&camera_frame, &output.image);
        output.convert_us = monotonic_us() - convert_started_us;
        release_result = camera_release_frame(&pipeline->camera,
                                              &camera_frame);

        if (convert_result != 0 || release_result != 0) {
            pipeline_frame_release(&output);
            LOG_ERROR("转换图像或归还摄像头缓冲区失败");
            pipeline_request_stop(
                pipeline, 1, "转换图像或归还摄像头缓冲区失败");
            break;
        }

        convert_us = output.convert_us;
        push_result = latest_frame_slot_push(pipeline->captured_frames,
                                             &output);
        if (push_result != LATEST_FRAME_SLOT_OK) {
            pipeline_frame_release(&output);
            if (push_result == LATEST_FRAME_SLOT_ERROR) {
                LOG_ERROR("写入采集帧槽失败");
                pipeline_request_stop(pipeline, 1,
                                      "写入采集帧槽失败");
            }
            break;
        }

        pipeline_record_capture(pipeline, convert_us);
    }

    latest_frame_slot_close(pipeline->captured_frames);
    return NULL;
}

// 识别线程主函数-消费者线程
static void *recognition_thread_main(void *argument)
{
    plate_pipeline_t *pipeline = argument;

    while (!pipeline_should_stop(pipeline)) {
        pipeline_frame_t frame;                         // 采集帧
        plate_result_t results[PLATE_MAX_RESULTS];      // 识别结果
        const plate_result_t *best_result;       // 最佳识别结果
        uint64_t recognize_started_us;           // 识别开始时间戳（微秒）
        uint64_t recognized_at_us;               // 识别结束时间戳（微秒）
        int take_result;                        // 采集线程读取采集帧槽的结果
        int result_count;                       // 识别结果数量
        int confirmation_result;                // 确认结果
        int push_result;                        // 输出线程写入输出帧槽的结果

        memset(&frame, 0, sizeof(frame));
        take_result = latest_frame_slot_take(pipeline->captured_frames, &frame, pipeline->config.frame_wait_ms);

        if (take_result == LATEST_FRAME_SLOT_TIMEOUT)
            continue;

        if (take_result != LATEST_FRAME_SLOT_OK) {
            if (take_result == LATEST_FRAME_SLOT_ERROR) {
                LOG_ERROR("读取采集帧槽失败");
                pipeline_request_stop(pipeline, 1, "读取采集帧槽失败");
            }
            break;
        }

        if (frame.image.width > INT_MAX || frame.image.height > INT_MAX || frame.image.stride > INT_MAX) {
            pipeline_frame_release(&frame);
            LOG_ERROR("图像尺寸超出识别器支持范围");
            pipeline_request_stop(pipeline, 1, "图像尺寸超出识别器支持范围");
            break;
        }

        recognize_started_us = monotonic_us();

        result_count = plate_recognizer_process(pipeline->recognizer, frame.image.data,
            (int)frame.image.width, (int)frame.image.height, (int)frame.image.stride,
            pipeline->config.confidence_threshold, results, PLATE_MAX_RESULTS);

        frame.recognize_us = monotonic_us() - recognize_started_us;

        if (result_count < 0) {
            const char *recognizer_error = plate_recognizer_last_error(pipeline->recognizer);
            char message[ERROR_MESSAGE_CAPACITY];

            snprintf(message, sizeof(message), "车牌识别失败: %s", recognizer_error != NULL ? recognizer_error : "未知错误");
            pipeline_frame_release(&frame);
            LOG_ERROR("%s", message);
            pipeline_request_stop(pipeline, 1, message);
            break;
        }

        pipeline_record_recognition(pipeline, frame.recognize_us);
        best_result = find_best_result(results, result_count);
        recognized_at_us = monotonic_us();
        if (best_result != NULL)
            pipeline_mark_plate_seen(pipeline, recognized_at_us);

        confirmation_result = plate_confirmation_update(&pipeline->confirmation, best_result, recognized_at_us);
        if (confirmation_result < 0) {
            pipeline_frame_release(&frame);
            LOG_ERROR("更新车牌确认器失败");
            pipeline_request_stop(pipeline, 1, "更新车牌确认器失败");
            break;
        }
        
        if (confirmation_result == 1) {
            pipeline_publish_plate_event(pipeline, best_result, recognized_at_us);
        }

        if (plate_recognizer_draw(
                pipeline->recognizer, frame.image.data,
                (int)frame.image.width, (int)frame.image.height,
                (int)frame.image.stride, results, result_count) != 0) {
            const char *recognizer_error = plate_recognizer_last_error(pipeline->recognizer);
            char message[ERROR_MESSAGE_CAPACITY];

            snprintf(message, sizeof(message), "绘制识别结果失败: %s", recognizer_error != NULL ? recognizer_error : "未知错误");
            pipeline_frame_release(&frame);
            LOG_ERROR("%s", message);
            pipeline_request_stop(pipeline, 1, message);
            break;
        }

        push_result = latest_frame_slot_push(pipeline->annotated_frames, &frame);
        if (push_result != LATEST_FRAME_SLOT_OK) {
            pipeline_frame_release(&frame);
            if (push_result == LATEST_FRAME_SLOT_ERROR) {
                LOG_ERROR("写入绘制帧槽失败");
                pipeline_request_stop(pipeline, 1,
                                      "写入绘制帧槽失败");
            }
            break;
        }
    }

    latest_frame_slot_close(pipeline->annotated_frames);
    return NULL;
}

// 创建车牌 pipeline
plate_pipeline_t *plate_pipeline_create(const plate_pipeline_config_t *config)
{
    plate_pipeline_t *pipeline;
    int error;

    if (validate_config(config) != 0) {
        LOG_ERROR("无效的车牌管线配置");
        return NULL;
    }

    pipeline = calloc(1, sizeof(*pipeline));    // 分配内存
    if (pipeline == NULL) {
        LOG_ERROR("分配车牌管线失败");
        return NULL;
    }

    // 初始化状态互斥锁
    error = pthread_mutex_init(&pipeline->state_mutex, NULL);
    if (error != 0) {
        LOG_ERROR("初始化车牌管线状态锁失败: %s", strerror(error));
        free(pipeline);
        return NULL;
    }
    pipeline->mutex_initialized = 1;

    // 复制地址
    if (copy_config_string(pipeline->camera_device, sizeof(pipeline->camera_device), config->camera.device) != 0 ||
        copy_config_string(pipeline->detect_model_path, sizeof(pipeline->detect_model_path), config->detect_model_path) != 0 ||
        copy_config_string(pipeline->recognition_model_path, sizeof(pipeline->recognition_model_path), config->recognition_model_path) != 0 ||
        copy_config_string(pipeline->font_path, sizeof(pipeline->font_path), config->font_path) != 0) 
    {
        LOG_ERROR("车牌管线路径为空或过长");
        goto fail;
    }

    pipeline->config = *config;
    pipeline->config.camera.device = pipeline->camera_device;
    pipeline->config.detect_model_path = pipeline->detect_model_path;
    pipeline->config.recognition_model_path = pipeline->recognition_model_path;
    pipeline->config.font_path = pipeline->font_path;

    // 创建帧槽
    pipeline->captured_frames = latest_frame_slot_create();
    if (pipeline->captured_frames == NULL)
        goto fail;

    // 创建绘制帧槽
    pipeline->annotated_frames = latest_frame_slot_create();
    if (pipeline->annotated_frames == NULL)
        goto fail;

    // 初始化车牌确认器
    if (plate_confirmation_init(&pipeline->confirmation, &pipeline->config.confirmation) != 0) {
        LOG_ERROR("初始化车牌确认器失败");
        goto fail;
    }

    pipeline->recognizer = plate_recognizer_create(pipeline->config.detect_model_path, pipeline->config.recognition_model_path, pipeline->config.font_path);
    if (pipeline->recognizer == NULL) {
        LOG_ERROR("创建车牌识别器失败");
        goto fail;
    }

    if (camera_open(&pipeline->camera, &pipeline->config.camera) != 0)
        goto fail;
    pipeline->camera_opened = 1;

    if (pipeline->camera.pixel_format != V4L2_PIX_FMT_MJPEG &&
        pipeline->camera.pixel_format != V4L2_PIX_FMT_YUYV) {
        LOG_ERROR("摄像头实际格式不受支持: 0x%08x",
                  pipeline->camera.pixel_format);
        goto fail;
    }

    return pipeline;

fail:
    plate_pipeline_destroy(pipeline);
    return NULL;
}


int plate_pipeline_start(plate_pipeline_t *pipeline)
{
    char message[ERROR_MESSAGE_CAPACITY];
    int error;

    if (pipeline == NULL)
        return PLATE_PIPELINE_ERROR;

    pthread_mutex_lock(&pipeline->state_mutex);
    if (pipeline->start_called || pipeline->stop_requested || pipeline->failed) {
        pthread_mutex_unlock(&pipeline->state_mutex);
        return PLATE_PIPELINE_ERROR;
    }
    pipeline->start_called = 1;
    pthread_mutex_unlock(&pipeline->state_mutex);

    if (camera_start(&pipeline->camera) != 0) {
        LOG_ERROR("启动摄像头失败");
        pipeline_request_stop(pipeline, 1, "启动摄像头失败");
        return PLATE_PIPELINE_ERROR;
    }
    pipeline->camera_started = 1;

    // 创建识别线程
    error = pthread_create(&pipeline->recognition_thread, NULL, recognition_thread_main, pipeline);
    if (error != 0) {
        snprintf(message, sizeof(message), "创建识别线程失败: %s", strerror(error));
        LOG_ERROR("%s", message);
        pipeline_request_stop(pipeline, 1, message);
        plate_pipeline_stop(pipeline);
        return PLATE_PIPELINE_ERROR;
    }
    pipeline->recognition_thread_started = 1;

    // 创建采集线程
    error = pthread_create(&pipeline->capture_thread, NULL, capture_thread_main, pipeline);
    if (error != 0) {
        snprintf(message, sizeof(message), "创建采集线程失败: %s", strerror(error));
        LOG_ERROR("%s", message);
        pipeline_request_stop(pipeline, 1, message);
        plate_pipeline_stop(pipeline);
        return PLATE_PIPELINE_ERROR;
    }
    pipeline->capture_thread_started = 1;

    return PLATE_PIPELINE_OK;
}

void plate_pipeline_stop(plate_pipeline_t *pipeline)
{
    int error;

    if (pipeline == NULL)
        return;

    pipeline_request_stop(pipeline, 0, NULL);

    if (pipeline->capture_thread_started) {
        error = pthread_join(pipeline->capture_thread, NULL);
        if (error != 0)
            LOG_ERROR("等待采集线程结束失败: %s", strerror(error));
        pipeline->capture_thread_started = 0;
    }

    if (pipeline->recognition_thread_started) {
        error = pthread_join(pipeline->recognition_thread, NULL);
        if (error != 0)
            LOG_ERROR("等待识别线程结束失败: %s", strerror(error));
        pipeline->recognition_thread_started = 0;
    }

    if (pipeline->camera_started) {
        if (camera_stop(&pipeline->camera) != 0) {
            LOG_ERROR("停止摄像头失败");
            pipeline_request_stop(pipeline, 1, "停止摄像头失败");
        }
        pipeline->camera_started = 0;
    }
}

// 从 pipeline 中获取一帧绘制后的视频帧
int plate_pipeline_take_frame(plate_pipeline_t *pipeline, pipeline_frame_t *frame, int timeout_ms)
{
    int result;

    if (pipeline == NULL || frame == NULL)
        return PLATE_PIPELINE_ERROR;

    result = latest_frame_slot_take(pipeline->annotated_frames, frame, timeout_ms);
    switch (result) {
        case LATEST_FRAME_SLOT_OK:
            return PLATE_PIPELINE_OK;
        case LATEST_FRAME_SLOT_TIMEOUT:
            return PLATE_PIPELINE_TIMEOUT;
        case LATEST_FRAME_SLOT_CLOSED:
            return PLATE_PIPELINE_CLOSED;
        default:
            return PLATE_PIPELINE_ERROR;
    }
}


int plate_pipeline_take_event(plate_pipeline_t *pipeline, plate_pipeline_event_t *event)
{
    int has_event;

    if (pipeline == NULL || event == NULL)
        return PLATE_PIPELINE_ERROR;

    pthread_mutex_lock(&pipeline->state_mutex);
    has_event = pipeline->has_pending_event;
    if (has_event) {
        *event = pipeline->pending_event;
        memset(&pipeline->pending_event, 0, sizeof(pipeline->pending_event));
        pipeline->has_pending_event = 0;
    }
    pthread_mutex_unlock(&pipeline->state_mutex);

    return has_event;
}

uint64_t plate_pipeline_last_plate_seen_us(plate_pipeline_t *pipeline)
{
    uint64_t seen_us;

    if (pipeline == NULL)
        return 0;

    pthread_mutex_lock(&pipeline->state_mutex);
    seen_us = pipeline->last_plate_seen_us;
    pthread_mutex_unlock(&pipeline->state_mutex);

    return seen_us;
}

int plate_pipeline_get_stats(plate_pipeline_t *pipeline,
                             plate_pipeline_stats_t *stats)
{
    if (pipeline == NULL || stats == NULL)
        return PLATE_PIPELINE_ERROR;

    pthread_mutex_lock(&pipeline->state_mutex);
    *stats = pipeline->stats;
    pthread_mutex_unlock(&pipeline->state_mutex);

    stats->capture_dropped_frames =
        latest_frame_slot_dropped(pipeline->captured_frames);
    stats->output_dropped_frames =
        latest_frame_slot_dropped(pipeline->annotated_frames);

    return PLATE_PIPELINE_OK;
}

int plate_pipeline_failed(plate_pipeline_t *pipeline)
{
    int failed;

    if (pipeline == NULL)
        return 1;

    pthread_mutex_lock(&pipeline->state_mutex);
    failed = pipeline->failed;
    pthread_mutex_unlock(&pipeline->state_mutex);

    return failed;
}

int plate_pipeline_copy_last_error(plate_pipeline_t *pipeline,
                                   char *buffer, size_t capacity)
{
    if (pipeline == NULL || buffer == NULL || capacity == 0)
        return PLATE_PIPELINE_ERROR;

    pthread_mutex_lock(&pipeline->state_mutex);
    snprintf(buffer, capacity, "%s", pipeline->last_error);
    pthread_mutex_unlock(&pipeline->state_mutex);

    return PLATE_PIPELINE_OK;
}

void plate_pipeline_destroy(plate_pipeline_t *pipeline)
{
    if (pipeline == NULL)
        return;

    if (pipeline->mutex_initialized)
        plate_pipeline_stop(pipeline);

    if (pipeline->camera_opened)
        camera_close(&pipeline->camera);

    plate_recognizer_destroy(pipeline->recognizer);
    latest_frame_slot_destroy(pipeline->annotated_frames);
    latest_frame_slot_destroy(pipeline->captured_frames);

    if (pipeline->mutex_initialized)
        pthread_mutex_destroy(&pipeline->state_mutex);

    free(pipeline);
}

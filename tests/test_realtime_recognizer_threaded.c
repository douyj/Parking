#include "display.h"
#include "log.h"
#include "plate_pipeline.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define DEFAULT_CAMERA_DEVICE "/dev/video0"
#define DEFAULT_FRAMEBUFFER_DEVICE "/dev/fb0"
#define DEFAULT_CAMERA_WIDTH 1280U
#define DEFAULT_CAMERA_HEIGHT 720U
#define DEFAULT_CAMERA_FPS 30U
#define DEFAULT_DETECT_MODEL \
    "models/yolo26s-plate-detect-rk3576.rknn"
#define DEFAULT_RECOGNITION_MODEL \
    "models/plate_rec_color-rk3576.rknn"
#define DEFAULT_FONT_PATH "fonts/platech.ttf"
#define DEFAULT_CONFIDENCE_THRESHOLD 0.30F
#define REQUIRED_CONFIRM_FRAMES 3U
#define MAX_MISSED_FRAMES 3U
#define SAME_PLATE_COOLDOWN_US 10000000ULL
#define CAMERA_TIMEOUT_MS 1000
#define QUEUE_WAIT_MS 100
#define ERROR_MESSAGE_CAPACITY 256

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static uint64_t monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        LOG_ERROR("安装退出信号处理失败: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void log_pipeline_error(plate_pipeline_t *pipeline)
{
    char message[ERROR_MESSAGE_CAPACITY];

    if (pipeline == NULL)
        return;

    memset(message, 0, sizeof(message));
    if (plate_pipeline_copy_last_error(
            pipeline, message, sizeof(message)) == PLATE_PIPELINE_OK &&
        message[0] != '\0') {
        LOG_ERROR("车牌管线错误: %s", message);
    }
}

static int drain_plate_events(plate_pipeline_t *pipeline)
{
    for (;;) {
        plate_pipeline_event_t event;
        int result;

        memset(&event, 0, sizeof(event));
        result = plate_pipeline_take_event(pipeline, &event);
        if (result == 0)
            return 0;
        if (result < 0)
            return -1;

        LOG_INFO("确认车牌=%s, 颜色=%s, 检测置信度=%.2f, "
                 "颜色置信度=%.2f",
                 event.result.plate_number,
                 event.result.color_name,
                 event.result.detect_confidence,
                 event.result.color_confidence);
    }
}

static int display_frames(plate_pipeline_t *pipeline, display_t *display)
{
    uint64_t report_started_us = monotonic_us();
    uint64_t total_convert_us = 0;
    uint64_t total_recognize_us = 0;
    uint64_t total_display_us = 0;
    uint64_t total_latency_us = 0;
    uint64_t previous_capture_drops = 0;
    uint64_t previous_output_drops = 0;
    unsigned int displayed_frames = 0;

    while (running) {
        pipeline_frame_t frame;
        display_frame_t display_frame;
        plate_pipeline_stats_t stats;
        uint64_t display_started_us;
        uint64_t display_us;
        uint64_t now_us;
        int take_result;

        if (drain_plate_events(pipeline) != 0)
            return -1;

        memset(&frame, 0, sizeof(frame));
        take_result = plate_pipeline_take_frame(
            pipeline, &frame, QUEUE_WAIT_MS);
        if (take_result == PLATE_PIPELINE_TIMEOUT)
            continue;
        if (take_result == PLATE_PIPELINE_CLOSED)
            break;
        if (take_result != PLATE_PIPELINE_OK) {
            LOG_ERROR("从车牌管线取得绘制帧失败");
            return -1;
        }

        display_frame.data = frame.image.data;
        display_frame.width = frame.image.width;
        display_frame.height = frame.image.height;
        display_frame.stride = frame.image.stride;
        display_frame.pixel_format = DISPLAY_PIXEL_FORMAT_BGR888;

        display_started_us = monotonic_us();
        if (display_present(display, &display_frame) != 0) {
            pipeline_frame_release(&frame);
            return -1;
        }
        display_us = monotonic_us() - display_started_us;
        now_us = monotonic_us();

        total_convert_us += frame.convert_us;
        total_recognize_us += frame.recognize_us;
        total_display_us += display_us;
        total_latency_us += now_us - frame.started_us;
        ++displayed_frames;
        pipeline_frame_release(&frame);

        if (now_us - report_started_us >= 1000000ULL) {
            double elapsed_seconds;

            if (plate_pipeline_get_stats(pipeline, &stats) !=
                PLATE_PIPELINE_OK) {
                LOG_ERROR("读取车牌管线统计失败");
                return -1;
            }

            elapsed_seconds =
                (double)(now_us - report_started_us) / 1000000.0;

            LOG_INFO("FPS=%.1f, 转换=%.1f ms, 识别=%.1f ms, "
                     "显示=%.1f ms, 延迟=%.1f ms, "
                     "丢帧(采集/显示)=%llu/%llu",
                     displayed_frames / elapsed_seconds,
                     (double)total_convert_us /
                         displayed_frames / 1000.0,
                     (double)total_recognize_us /
                         displayed_frames / 1000.0,
                     (double)total_display_us /
                         displayed_frames / 1000.0,
                     (double)total_latency_us /
                         displayed_frames / 1000.0,
                     (unsigned long long)
                         (stats.capture_dropped_frames -
                          previous_capture_drops),
                     (unsigned long long)
                         (stats.output_dropped_frames -
                          previous_output_drops));

            report_started_us = now_us;
            previous_capture_drops = stats.capture_dropped_frames;
            previous_output_drops = stats.output_dropped_frames;
            total_convert_us = 0;
            total_recognize_us = 0;
            total_display_us = 0;
            total_latency_us = 0;
            displayed_frames = 0;
        }
    }

    if (drain_plate_events(pipeline) != 0)
        return -1;

    return plate_pipeline_failed(pipeline) ? -1 : 0;
}

int main(void)
{
    plate_pipeline_config_t pipeline_config;
    display_config_t display_config;
    plate_pipeline_t *pipeline = NULL;
    display_t *display = NULL;
    int exit_code = 1;

    if (install_signal_handlers() != 0)
        return 1;

    memset(&display_config, 0, sizeof(display_config));
    display_config.device = DEFAULT_FRAMEBUFFER_DEVICE;
    display_config.keep_aspect_ratio = 1;
    display = display_create(&display_config);
    if (display == NULL)
        goto cleanup;

    memset(&pipeline_config, 0, sizeof(pipeline_config));
    pipeline_config.camera.device = DEFAULT_CAMERA_DEVICE;
    pipeline_config.camera.width = DEFAULT_CAMERA_WIDTH;
    pipeline_config.camera.height = DEFAULT_CAMERA_HEIGHT;
    pipeline_config.camera.fps = DEFAULT_CAMERA_FPS;
    pipeline_config.camera.pixel_format = V4L2_PIX_FMT_MJPEG;
    pipeline_config.detect_model_path = DEFAULT_DETECT_MODEL;
    pipeline_config.recognition_model_path =
        DEFAULT_RECOGNITION_MODEL;
    pipeline_config.font_path = DEFAULT_FONT_PATH;
    pipeline_config.confidence_threshold =
        DEFAULT_CONFIDENCE_THRESHOLD;
    pipeline_config.confirmation.required_frames =
        REQUIRED_CONFIRM_FRAMES;
    pipeline_config.confirmation.max_missed_frames =
        MAX_MISSED_FRAMES;
    pipeline_config.confirmation.same_plate_cooldown_us =
        SAME_PLATE_COOLDOWN_US;
    pipeline_config.camera_timeout_ms = CAMERA_TIMEOUT_MS;
    pipeline_config.frame_wait_ms = QUEUE_WAIT_MS;

    pipeline = plate_pipeline_create(&pipeline_config);
    if (pipeline == NULL)
        goto cleanup;

    if (plate_pipeline_start(pipeline) != PLATE_PIPELINE_OK) {
        log_pipeline_error(pipeline);
        goto cleanup;
    }

    LOG_INFO("开始多线程实时车牌识别: 请求参数=%ux%u@%u，"
             "按 Ctrl+C 退出",
             pipeline_config.camera.width,
             pipeline_config.camera.height,
             pipeline_config.camera.fps);

    exit_code = display_frames(pipeline, display) == 0 ? 0 : 1;
    if (exit_code != 0 && plate_pipeline_failed(pipeline))
        log_pipeline_error(pipeline);

cleanup:
    plate_pipeline_stop(pipeline);
    plate_pipeline_destroy(pipeline);
    display_destroy(display);
    return exit_code;
}

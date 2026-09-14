#include "display.h"
#include "gate.h"
#include "log.h"
#include "plate_pipeline.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
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
#define GATE_AUTO_CLOSE_IDLE_US 10000000ULL
#define CAMERA_TIMEOUT_MS 1000
#define QUEUE_WAIT_MS 100
#define GATE_INITIALIZE_TIMEOUT_MS 5000U
#define ERROR_MESSAGE_CAPACITY 256

#define GATE_PWMCHIP_PATH "/sys/class/pwm/pwmchip2"
#define GATE_PWM_CHANNEL 0U
#define GATE_PERIOD_NS 20000000ULL
#define GATE_OPEN_PULSE_NS 2400000ULL
#define GATE_CLOSE_PULSE_NS 1400000ULL
#define GATE_MOVEMENT_TIME_MS 800U
#define GATE_HOLD_AFTER_MOVE 0

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

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec duration;

    duration.tv_sec = milliseconds / 1000U;
    duration.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&duration, &duration) < 0 && errno == EINTR) {
        if (!running)
            break;
    }
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

static const char *gate_state_to_string(gate_state_t state)
{
    switch (state) {
    case GATE_STATE_CLOSED:
        return "CLOSED";
    case GATE_STATE_OPENING:
        return "OPENING";
    case GATE_STATE_OPEN:
        return "OPEN";
    case GATE_STATE_CLOSING:
        return "CLOSING";
    case GATE_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static int confirm_real_gate(void)
{
    char answer[32];

    fprintf(stderr,
            "\n警告：即将启用真实 Gate。gate_create() 会立即执行关闸。\n"
            "当前系统没有限位、地感或防砸传感器；连续 10 秒没有识别到车牌时会自动关闸。\n"
            "确认周围无人、无车且机构可以安全运动后，输入 ENABLE 继续：");
    fflush(stderr);
    if (fgets(answer, sizeof(answer), stdin) == NULL)
        return 0;
    answer[strcspn(answer, "\r\n")] = '\0';
    return strcmp(answer, "ENABLE") == 0;
}

static gate_t *create_real_gate(void)
{
    gate_config_t config;
    gate_t *gate;
    uint64_t started_us;

    memset(&config, 0, sizeof(config));
    config.pwmchip_path = GATE_PWMCHIP_PATH;
    config.channel = GATE_PWM_CHANNEL;
    config.period_ns = GATE_PERIOD_NS;
    config.open_pulse_ns = GATE_OPEN_PULSE_NS;
    config.close_pulse_ns = GATE_CLOSE_PULSE_NS;
    config.movement_time_ms = GATE_MOVEMENT_TIME_MS;
    config.hold_after_move = GATE_HOLD_AFTER_MOVE;

    LOG_INFO("启用真实 Gate: %s/pwm%u, period=%llu ns, "
             "open=%llu ns, close=%llu ns, movement=%u ms",
             config.pwmchip_path, config.channel,
             (unsigned long long)config.period_ns,
             (unsigned long long)config.open_pulse_ns,
             (unsigned long long)config.close_pulse_ns,
             config.movement_time_ms);
    gate = gate_create(&config);
    if (gate == NULL) {
        LOG_ERROR("创建真实 Gate 失败");
        return NULL;
    }

    started_us = monotonic_us();
    while (running &&
           (gate_get_state(gate) == GATE_STATE_OPENING ||
            gate_get_state(gate) == GATE_STATE_CLOSING)) {
        if (gate_update(gate) != GATE_OK) {
            LOG_ERROR("初始化 Gate 失败: %s", gate_last_error(gate));
            gate_destroy(gate);
            return NULL;
        }
        if (monotonic_us() - started_us >=
            (uint64_t)GATE_INITIALIZE_TIMEOUT_MS * 1000ULL) {
            LOG_ERROR("等待 Gate 初始化超时");
            gate_destroy(gate);
            return NULL;
        }
        sleep_ms(20U);
    }

    if (!running) {
        gate_destroy(gate);
        return NULL;
    }

    LOG_INFO("Gate 初始化完成，状态=%s",
             gate_state_to_string(gate_get_state(gate)));
    return gate;
}

static int handle_gate(plate_pipeline_t *pipeline, gate_t *gate,
                       int enable_gate, int *dry_gate_open)
{
    plate_pipeline_event_t event;
    uint64_t last_seen_us;
    uint64_t now_us;
    int event_result;

    if (enable_gate && gate_update(gate) != GATE_OK) {
        LOG_ERROR("更新 Gate 状态失败: %s", gate_last_error(gate));
        return -1;
    }

    for (;;) {
        memset(&event, 0, sizeof(event));
        event_result = plate_pipeline_take_event(pipeline, &event);
        if (event_result == 0)
            break;
        if (event_result < 0) {
            LOG_ERROR("读取稳定车牌事件失败");
            return -1;
        }

        if (!enable_gate) {
            LOG_INFO("[DRY-RUN] 稳定车牌 %s -> 模拟开闸",
                     event.result.plate_number);
            *dry_gate_open = 1;
            continue;
        }

        if (gate_get_state(gate) != GATE_STATE_CLOSED) {
            LOG_INFO("车牌 %s 请求开闸，但 Gate 状态=%s，忽略重复请求",
                     event.result.plate_number,
                     gate_state_to_string(gate_get_state(gate)));
            continue;
        }

        if (gate_open(gate) != GATE_OK) {
            LOG_ERROR("车牌 %s 请求开闸失败: %s",
                      event.result.plate_number, gate_last_error(gate));
            return -1;
        }

        LOG_INFO("车牌 %s 已触发真实开闸；连续 10 秒未识别到车牌将自动关闸",
                 event.result.plate_number);
    }

    last_seen_us = plate_pipeline_last_plate_seen_us(pipeline);
    now_us = monotonic_us();
    if (last_seen_us == 0 ||
        now_us - last_seen_us < GATE_AUTO_CLOSE_IDLE_US)
        return 0;

    if (!enable_gate) {
        if (*dry_gate_open) {
            LOG_INFO("[DRY-RUN] 连续 10 秒未识别到车牌 -> 模拟关闸");
            *dry_gate_open = 0;
        }
        return 0;
    }

    if (gate_get_state(gate) == GATE_STATE_OPEN) {
        if (gate_close(gate) != GATE_OK) {
            LOG_ERROR("自动关闸失败: %s", gate_last_error(gate));
            return -1;
        }
        LOG_INFO("连续 10 秒未识别到车牌，已触发自动关闸");
    }

    return 0;
}

static int display_frames(plate_pipeline_t *pipeline, display_t *display,
                          gate_t *gate, int enable_gate)
{
    uint64_t report_started_us = monotonic_us();
    uint64_t total_convert_us = 0;
    uint64_t total_recognize_us = 0;
    uint64_t total_display_us = 0;
    uint64_t total_latency_us = 0;
    uint64_t previous_capture_drops = 0;
    uint64_t previous_output_drops = 0;
    unsigned int displayed_frames = 0;
    int dry_gate_open = 0;

    while (running) {
        pipeline_frame_t frame;
        display_frame_t display_frame;
        plate_pipeline_stats_t stats;
        uint64_t display_started_us;
        uint64_t display_us;
        uint64_t now_us;
        int take_result;

        if (handle_gate(pipeline, gate, enable_gate,
                        &dry_gate_open) != 0)
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

    return plate_pipeline_failed(pipeline) ? -1 : 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [--enable-gate]\n"
            "  默认模式       模拟开闸，并在 10 秒无车牌后模拟关闸\n"
            "  --enable-gate  人工确认后驱动真实 Gate，并启用定时关闸\n",
            program);
}

int main(int argc, char **argv)
{
    plate_pipeline_config_t pipeline_config;
    display_config_t display_config;
    plate_pipeline_t *pipeline = NULL;
    display_t *display = NULL;
    gate_t *gate = NULL;
    int enable_gate = 0;
    int exit_code = 1;

    if (argc == 2 && strcmp(argv[1], "--enable-gate") == 0) {
        enable_gate = 1;
    } else if (argc == 2 &&
               (strcmp(argv[1], "--help") == 0 ||
                strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    } else if (argc != 1) {
        print_usage(argv[0]);
        return 1;
    }

    if (install_signal_handlers() != 0)
        return 1;

    if (enable_gate && !confirm_real_gate()) {
        LOG_INFO("未确认启用真实 Gate，程序退出");
        return 1;
    }

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

    if (enable_gate) {
        gate = create_real_gate();
        if (gate == NULL)
            goto cleanup;
    }

    if (plate_pipeline_start(pipeline) != PLATE_PIPELINE_OK) {
        log_pipeline_error(pipeline);
        goto cleanup;
    }

    LOG_INFO("开始车牌/Gate 联动测试: 请求参数=%ux%u@%u，模式=%s，"
             "按 Ctrl+C 退出",
             pipeline_config.camera.width,
             pipeline_config.camera.height,
             pipeline_config.camera.fps,
             enable_gate ? "真实 Gate（10 秒无车牌自动关闸）" :
                           "DRY-RUN");

    exit_code = display_frames(pipeline, display, gate,
                               enable_gate) == 0 ? 0 : 1;
    if (exit_code != 0 && plate_pipeline_failed(pipeline))
        log_pipeline_error(pipeline);

cleanup:
    plate_pipeline_stop(pipeline);
    plate_pipeline_destroy(pipeline);
    display_destroy(display);
    gate_destroy(gate);
    return exit_code;
}

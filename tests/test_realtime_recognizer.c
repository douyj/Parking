#include "camera.h"
#include "display.h"
#include "image_convert.h"
#include "log.h"
#include "plate_recognizer.h"

#include <errno.h>
#include <limits.h>
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

typedef struct {
    char candidate_plate[PLATE_TEXT_CAPACITY];
    unsigned int consecutive_frames;
    unsigned int missed_frames;
    int candidate_confirmed;
    char last_confirmed_plate[PLATE_TEXT_CAPACITY];
    uint64_t last_confirmed_us;
} plate_confirmation_t;

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

static const plate_result_t *find_best_result(
    const plate_result_t *results, int result_count)
{
    const plate_result_t *best = NULL;

    for (int i = 0; i < result_count; ++i) {
        if (results[i].plate_number[0] == '\0')
            continue;
        if (best == NULL ||
            results[i].detect_confidence > best->detect_confidence)
            best = &results[i];
    }
    return best;
}

static void copy_plate_number(char destination[PLATE_TEXT_CAPACITY],
                              const char *source)
{
    strncpy(destination, source, PLATE_TEXT_CAPACITY - 1U);
    destination[PLATE_TEXT_CAPACITY - 1U] = '\0';
}

/* 返回 1 表示本帧产生了一次新的确认车牌事件。 */
static int update_plate_confirmation(plate_confirmation_t *state,
                                     const plate_result_t *result,
                                     uint64_t now_us)
{
    if (result == NULL) {
        if (state->missed_frames < MAX_MISSED_FRAMES)
            ++state->missed_frames;
        if (state->missed_frames >= MAX_MISSED_FRAMES) {
            state->candidate_plate[0] = '\0';
            state->consecutive_frames = 0;
            state->candidate_confirmed = 0;
        }
        return 0;
    }

    state->missed_frames = 0;
    if (strcmp(state->candidate_plate, result->plate_number) == 0) {
        if (state->consecutive_frames < REQUIRED_CONFIRM_FRAMES)
            ++state->consecutive_frames;
    } else {
        copy_plate_number(state->candidate_plate, result->plate_number);
        state->consecutive_frames = 1;
        state->candidate_confirmed = 0;
    }

    if (state->consecutive_frames < REQUIRED_CONFIRM_FRAMES ||
        state->candidate_confirmed)
        return 0;

    if (strcmp(state->last_confirmed_plate, result->plate_number) == 0 &&
        now_us - state->last_confirmed_us < SAME_PLATE_COOLDOWN_US)
        return 0;

    copy_plate_number(state->last_confirmed_plate, result->plate_number);
    state->last_confirmed_us = now_us;
    state->candidate_confirmed = 1;
    return 1;
}

int main(void)
{
    Camera camera;
    CameraConfig camera_config;
    display_config_t display_config;
    plate_confirmation_t confirmation;
    display_t *display = NULL;
    plate_recognizer_t *recognizer = NULL;
    int camera_opened = 0;
    int camera_started = 0;
    int exit_code = 1;
    uint64_t report_started_us;          // 记录上次报告的时间戳（微秒级）
    uint64_t total_convert_us = 0;       // 累计转换时间（微秒级）
    uint64_t total_recognize_us = 0;     // 累计识别时间（微秒级）
    uint64_t total_frame_us = 0;         // 累计帧时间（微秒级）
    unsigned int processed_frames = 0;

    memset(&camera, 0, sizeof(camera));
    memset(&confirmation, 0, sizeof(confirmation));
    if (install_signal_handlers() != 0)
        return 1;

    recognizer = plate_recognizer_create(
        DEFAULT_DETECT_MODEL,
        DEFAULT_RECOGNITION_MODEL,
        DEFAULT_FONT_PATH);
    if (recognizer == NULL) {
        LOG_ERROR("创建车牌识别器失败");
        goto cleanup;
    }

    display_config.device = DEFAULT_FRAMEBUFFER_DEVICE;
    display_config.keep_aspect_ratio = 1;
    display = display_create(&display_config);
    if (display == NULL)
        goto cleanup;

    camera_config.device = DEFAULT_CAMERA_DEVICE;
    camera_config.width = DEFAULT_CAMERA_WIDTH;
    camera_config.height = DEFAULT_CAMERA_HEIGHT;
    camera_config.fps = DEFAULT_CAMERA_FPS;
    camera_config.pixel_format = V4L2_PIX_FMT_MJPEG;
    if (camera_open(&camera, &camera_config) != 0)
        goto cleanup;
    camera_opened = 1;

    if (camera.pixel_format != V4L2_PIX_FMT_MJPEG &&
        camera.pixel_format != V4L2_PIX_FMT_YUYV) {
        LOG_ERROR("摄像头实际格式不受支持: 0x%08x",
                  camera.pixel_format);
        goto cleanup;
    }
    if (camera_start(&camera) != 0)
        goto cleanup;
    camera_started = 1;

    LOG_INFO("开始实时车牌识别: %ux%u@%u，按 Ctrl+C 退出",
             camera.width, camera.height, camera.fps);
    exit_code = 0;
    report_started_us = monotonic_us();

    while (running) {
        CameraFrame camera_frame;
        image_bgr_frame_t bgr_frame;
        plate_result_t results[PLATE_MAX_RESULTS];
        display_frame_t display_frame;
        uint64_t frame_started_us = monotonic_us();
        uint64_t stage_started_us;
        uint64_t convert_us;
        uint64_t recognize_us;
        uint64_t frame_us;
        int convert_result;
        int result_count;
        const plate_result_t *best_result;

        memset(&camera_frame, 0, sizeof(camera_frame));
        memset(&bgr_frame, 0, sizeof(bgr_frame));

        if (camera_get_frame(&camera, &camera_frame, 1000) != 0) {
            if (!running)
                break;
            exit_code = 1;
            break;
        }

        stage_started_us = monotonic_us();
        convert_result = image_convert_to_bgr(&camera_frame, &bgr_frame);
        convert_us = monotonic_us() - stage_started_us;

        if (camera_release_frame(&camera, &camera_frame) != 0) {
            image_bgr_frame_release(&bgr_frame);
            exit_code = 1;
            break;
        }
        if (convert_result != 0) {
            image_bgr_frame_release(&bgr_frame);
            exit_code = 1;
            break;
        }
        if (bgr_frame.width > INT_MAX || bgr_frame.height > INT_MAX ||
            bgr_frame.stride > INT_MAX) {
            LOG_ERROR("图像尺寸超出识别器支持范围");
            image_bgr_frame_release(&bgr_frame);
            exit_code = 1;
            break;
        }

        stage_started_us = monotonic_us();
        result_count = plate_recognizer_process(
            recognizer, bgr_frame.data,
            (int)bgr_frame.width, (int)bgr_frame.height,
            (int)bgr_frame.stride, DEFAULT_CONFIDENCE_THRESHOLD,
            results, PLATE_MAX_RESULTS);
        recognize_us = monotonic_us() - stage_started_us;

        if (result_count < 0) {
            const char *error = plate_recognizer_last_error(recognizer);
            LOG_ERROR("车牌识别失败: %s",
                      error != NULL ? error : "未知错误");
            image_bgr_frame_release(&bgr_frame);
            exit_code = 1;
            break;
        }

        best_result = find_best_result(results, result_count);
        if (update_plate_confirmation(
                &confirmation, best_result, monotonic_us())) {
            LOG_INFO("确认车牌=%s, 颜色=%s, 检测置信度=%.2f, "
                     "颜色置信度=%.2f",
                     best_result->plate_number, best_result->color_name,
                     best_result->detect_confidence,
                     best_result->color_confidence);
        }

        if (plate_recognizer_draw(
                recognizer, bgr_frame.data,
                (int)bgr_frame.width, (int)bgr_frame.height,
                (int)bgr_frame.stride, results, result_count) != 0) {
            const char *error = plate_recognizer_last_error(recognizer);
            LOG_ERROR("绘制识别结果失败: %s",
                      error != NULL ? error : "未知错误");
            image_bgr_frame_release(&bgr_frame);
            exit_code = 1;
            break;
        }

        display_frame.data = bgr_frame.data;
        display_frame.width = bgr_frame.width;
        display_frame.height = bgr_frame.height;
        display_frame.stride = bgr_frame.stride;
        display_frame.pixel_format = DISPLAY_PIXEL_FORMAT_BGR888;
        if (display_present(display, &display_frame) != 0) {
            image_bgr_frame_release(&bgr_frame);
            exit_code = 1;
            break;
        }

        frame_us = monotonic_us() - frame_started_us;
        image_bgr_frame_release(&bgr_frame);

        total_convert_us += convert_us;
        total_recognize_us += recognize_us;
        total_frame_us += frame_us;
        ++processed_frames;

        if (monotonic_us() - report_started_us >= 1000000ULL) {
            uint64_t now_us = monotonic_us();
            double elapsed_seconds =
                (double)(now_us - report_started_us) / 1000000.0;

            LOG_INFO("FPS=%.1f, 平均转换=%.1f ms, "
                     "平均识别=%.1f ms, 平均整帧=%.1f ms",
                     processed_frames / elapsed_seconds,
                     (double)total_convert_us / processed_frames / 1000.0,
                     (double)total_recognize_us / processed_frames / 1000.0,
                     (double)total_frame_us / processed_frames / 1000.0);

            report_started_us = now_us;
            total_convert_us = 0;
            total_recognize_us = 0;
            total_frame_us = 0;
            processed_frames = 0;
        }
    }

cleanup:
    if (camera_started)
        (void)camera_stop(&camera);
    if (camera_opened)
        camera_close(&camera);
    display_destroy(display);
    plate_recognizer_destroy(recognizer);
    return exit_code;
}

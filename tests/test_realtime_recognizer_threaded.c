#include "camera.h"
#include "display.h"
#include "image_convert.h"
#include "log.h"
#include "plate_recognizer.h"

#include <errno.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <pthread.h>
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
#define QUEUE_WAIT_MS 100L

typedef struct {
    image_bgr_frame_t image;
    uint64_t started_us;
    uint64_t convert_us;
    uint64_t recognize_us;
} pipeline_frame_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pipeline_frame_t frame;
    uint64_t dropped_frames;
    int has_frame;
    int closed;
} latest_frame_slot_t;

typedef struct {
    char candidate_plate[PLATE_TEXT_CAPACITY];
    unsigned int consecutive_frames;
    unsigned int missed_frames;
    int candidate_confirmed;
    char last_confirmed_plate[PLATE_TEXT_CAPACITY];
    uint64_t last_confirmed_us;
} plate_confirmation_t;

typedef struct {
    Camera *camera;
    plate_recognizer_t *recognizer;
    latest_frame_slot_t captured_frames;
    latest_frame_slot_t annotated_frames;
    pthread_mutex_t state_mutex;
    int failed;
} pipeline_t;

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

static void pipeline_frame_release(pipeline_frame_t *frame)
{
    if (frame == NULL)
        return;
    image_bgr_frame_release(&frame->image);
    memset(frame, 0, sizeof(*frame));
}

static int latest_frame_slot_init(latest_frame_slot_t *slot)
{
    int error;

    memset(slot, 0, sizeof(*slot));
    error = pthread_mutex_init(&slot->mutex, NULL);
    if (error != 0) {
        LOG_ERROR("初始化帧槽互斥锁失败: %s", strerror(error));
        return -1;
    }
    error = pthread_cond_init(&slot->condition, NULL);
    if (error != 0) {
        LOG_ERROR("初始化帧槽条件变量失败: %s", strerror(error));
        pthread_mutex_destroy(&slot->mutex);
        return -1;
    }
    return 0;
}

static void latest_frame_slot_close(latest_frame_slot_t *slot)
{
    pthread_mutex_lock(&slot->mutex);
    slot->closed = 1;
    pthread_cond_broadcast(&slot->condition);
    pthread_mutex_unlock(&slot->mutex);
}

static void latest_frame_slot_destroy(latest_frame_slot_t *slot)
{
    pipeline_frame_release(&slot->frame);
    pthread_cond_destroy(&slot->condition);
    pthread_mutex_destroy(&slot->mutex);
}

/* 成功转移所有权返回 1；帧槽已关闭返回 0。 */
static int latest_frame_slot_push(latest_frame_slot_t *slot,
                                  pipeline_frame_t *frame)
{
    int error = pthread_mutex_lock(&slot->mutex);

    if (error != 0) {
        LOG_ERROR("锁定帧槽失败: %s", strerror(error));
        return -1;
    }
    if (slot->closed) {
        pthread_mutex_unlock(&slot->mutex);
        return 0;
    }
    if (slot->has_frame) {
        pipeline_frame_release(&slot->frame);
        ++slot->dropped_frames;
    }
    slot->frame = *frame;
    memset(frame, 0, sizeof(*frame));
    slot->has_frame = 1;
    pthread_cond_signal(&slot->condition);
    pthread_mutex_unlock(&slot->mutex);
    return 1;
}

static void make_wait_deadline(struct timespec *deadline)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_nsec += QUEUE_WAIT_MS * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

/* 取到帧返回 1；已关闭或程序退出返回 0。 */
static int latest_frame_slot_take(latest_frame_slot_t *slot,
                                  pipeline_frame_t *frame)
{
    int error = pthread_mutex_lock(&slot->mutex);

    if (error != 0) {
        LOG_ERROR("锁定帧槽失败: %s", strerror(error));
        return -1;
    }
    while (!slot->has_frame && !slot->closed && running) {
        struct timespec deadline;

        make_wait_deadline(&deadline);
        error = pthread_cond_timedwait(
            &slot->condition, &slot->mutex, &deadline);
        if (error != 0 && error != ETIMEDOUT) {
            pthread_mutex_unlock(&slot->mutex);
            LOG_ERROR("等待最新帧失败: %s", strerror(error));
            return -1;
        }
    }
    if (!slot->has_frame) {
        pthread_mutex_unlock(&slot->mutex);
        return 0;
    }
    *frame = slot->frame;
    memset(&slot->frame, 0, sizeof(slot->frame));
    slot->has_frame = 0;
    pthread_mutex_unlock(&slot->mutex);
    return 1;
}

static uint64_t latest_frame_slot_dropped(latest_frame_slot_t *slot)
{
    uint64_t dropped;

    pthread_mutex_lock(&slot->mutex);
    dropped = slot->dropped_frames;
    pthread_mutex_unlock(&slot->mutex);
    return dropped;
}

static void pipeline_request_stop(pipeline_t *pipeline, int failed)
{
    if (failed) {
        pthread_mutex_lock(&pipeline->state_mutex);
        pipeline->failed = 1;
        pthread_mutex_unlock(&pipeline->state_mutex);
    }
    running = 0;
    latest_frame_slot_close(&pipeline->captured_frames);
    latest_frame_slot_close(&pipeline->annotated_frames);
}

static int pipeline_failed(pipeline_t *pipeline)
{
    int failed;

    pthread_mutex_lock(&pipeline->state_mutex);
    failed = pipeline->failed;
    pthread_mutex_unlock(&pipeline->state_mutex);
    return failed;
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

static void *capture_thread_main(void *argument)
{
    pipeline_t *pipeline = argument;

    while (running) {
        CameraFrame camera_frame;
        pipeline_frame_t output;
        uint64_t convert_started_us;
        int convert_result;
        int release_result;
        int push_result;

        memset(&camera_frame, 0, sizeof(camera_frame));
        memset(&output, 0, sizeof(output));
        output.started_us = monotonic_us();

        if (camera_get_frame(pipeline->camera, &camera_frame, 1000) != 0) {
            if (!running)
                break;
            pipeline_request_stop(pipeline, 1);
            break;
        }

        convert_started_us = monotonic_us();
        convert_result = image_convert_to_bgr(
            &camera_frame, &output.image);
        output.convert_us = monotonic_us() - convert_started_us;
        release_result = camera_release_frame(
            pipeline->camera, &camera_frame);

        if (convert_result != 0 || release_result != 0) {
            pipeline_frame_release(&output);
            pipeline_request_stop(pipeline, 1);
            break;
        }

        push_result = latest_frame_slot_push(
            &pipeline->captured_frames, &output);
        if (push_result != 1) {
            pipeline_frame_release(&output);
            if (push_result < 0)
                pipeline_request_stop(pipeline, 1);
            break;
        }
    }

    latest_frame_slot_close(&pipeline->captured_frames);
    return NULL;
}

static void *recognition_thread_main(void *argument)
{
    pipeline_t *pipeline = argument;
    plate_confirmation_t confirmation;

    memset(&confirmation, 0, sizeof(confirmation));
    while (running) {
        pipeline_frame_t frame;
        plate_result_t results[PLATE_MAX_RESULTS];
        const plate_result_t *best_result;
        uint64_t recognize_started_us;
        int take_result;
        int result_count;
        int push_result;

        memset(&frame, 0, sizeof(frame));
        take_result = latest_frame_slot_take(
            &pipeline->captured_frames, &frame);
        if (take_result != 1) {
            if (take_result < 0)
                pipeline_request_stop(pipeline, 1);
            break;
        }

        if (frame.image.width > INT_MAX ||
            frame.image.height > INT_MAX ||
            frame.image.stride > INT_MAX) {
            LOG_ERROR("图像尺寸超出识别器支持范围");
            pipeline_frame_release(&frame);
            pipeline_request_stop(pipeline, 1);
            break;
        }

        recognize_started_us = monotonic_us();
        result_count = plate_recognizer_process(
            pipeline->recognizer, frame.image.data,
            (int)frame.image.width, (int)frame.image.height,
            (int)frame.image.stride, DEFAULT_CONFIDENCE_THRESHOLD,
            results, PLATE_MAX_RESULTS);
        frame.recognize_us = monotonic_us() - recognize_started_us;

        if (result_count < 0) {
            const char *error =
                plate_recognizer_last_error(pipeline->recognizer);
            LOG_ERROR("车牌识别失败: %s",
                      error != NULL ? error : "未知错误");
            pipeline_frame_release(&frame);
            pipeline_request_stop(pipeline, 1);
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
                pipeline->recognizer, frame.image.data,
                (int)frame.image.width, (int)frame.image.height,
                (int)frame.image.stride, results, result_count) != 0) {
            const char *error =
                plate_recognizer_last_error(pipeline->recognizer);
            LOG_ERROR("绘制识别结果失败: %s",
                      error != NULL ? error : "未知错误");
            pipeline_frame_release(&frame);
            pipeline_request_stop(pipeline, 1);
            break;
        }

        push_result = latest_frame_slot_push(
            &pipeline->annotated_frames, &frame);
        if (push_result != 1) {
            pipeline_frame_release(&frame);
            if (push_result < 0)
                pipeline_request_stop(pipeline, 1);
            break;
        }
    }

    latest_frame_slot_close(&pipeline->annotated_frames);
    return NULL;
}

static int display_frames(pipeline_t *pipeline, display_t *display)
{
    uint64_t report_started_us = monotonic_us();
    uint64_t total_convert_us = 0;
    uint64_t total_recognize_us = 0;
    uint64_t total_display_us = 0;
    uint64_t total_latency_us = 0;
    uint64_t previous_capture_drops = 0;
    uint64_t previous_display_drops = 0;
    unsigned int displayed_frames = 0;

    while (running) {
        pipeline_frame_t frame;
        display_frame_t display_frame;
        uint64_t display_started_us;
        uint64_t display_us;
        uint64_t now_us;
        int take_result;

        memset(&frame, 0, sizeof(frame));
        take_result = latest_frame_slot_take(
            &pipeline->annotated_frames, &frame);
        if (take_result != 1) {
            if (take_result < 0)
                pipeline_request_stop(pipeline, 1);
            break;
        }

        display_frame.data = frame.image.data;
        display_frame.width = frame.image.width;
        display_frame.height = frame.image.height;
        display_frame.stride = frame.image.stride;
        display_frame.pixel_format = DISPLAY_PIXEL_FORMAT_BGR888;

        display_started_us = monotonic_us();
        if (display_present(display, &display_frame) != 0) {
            pipeline_frame_release(&frame);
            pipeline_request_stop(pipeline, 1);
            break;
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
            uint64_t capture_drops = latest_frame_slot_dropped(
                &pipeline->captured_frames);
            uint64_t display_drops = latest_frame_slot_dropped(
                &pipeline->annotated_frames);
            double elapsed_seconds =
                (double)(now_us - report_started_us) / 1000000.0;

            LOG_INFO("FPS=%.1f, 转换=%.1f ms, 识别=%.1f ms, "
                     "显示=%.1f ms, 延迟=%.1f ms, "
                     "丢帧(采集/显示)=%llu/%llu",
                     displayed_frames / elapsed_seconds,
                     (double)total_convert_us / displayed_frames / 1000.0,
                     (double)total_recognize_us / displayed_frames / 1000.0,
                     (double)total_display_us / displayed_frames / 1000.0,
                     (double)total_latency_us / displayed_frames / 1000.0,
                     (unsigned long long)
                         (capture_drops - previous_capture_drops),
                     (unsigned long long)
                         (display_drops - previous_display_drops));

            report_started_us = now_us;
            previous_capture_drops = capture_drops;
            previous_display_drops = display_drops;
            total_convert_us = 0;
            total_recognize_us = 0;
            total_display_us = 0;
            total_latency_us = 0;
            displayed_frames = 0;
        }
    }
    return pipeline_failed(pipeline) ? -1 : 0;
}

int main(void)
{
    Camera camera;
    CameraConfig camera_config;
    display_config_t display_config;
    display_t *display = NULL;
    plate_recognizer_t *recognizer = NULL;
    pipeline_t pipeline;
    pthread_t capture_thread;
    pthread_t recognition_thread;
    int state_mutex_initialized = 0;
    int captured_slot_initialized = 0;
    int annotated_slot_initialized = 0;
    int camera_opened = 0;
    int camera_started = 0;
    int capture_thread_created = 0;
    int recognition_thread_created = 0;
    int exit_code = 1;
    int error;

    memset(&camera, 0, sizeof(camera));
    memset(&pipeline, 0, sizeof(pipeline));
    if (install_signal_handlers() != 0)
        return 1;

    error = pthread_mutex_init(&pipeline.state_mutex, NULL);
    if (error != 0) {
        LOG_ERROR("初始化管线状态锁失败: %s", strerror(error));
        goto cleanup;
    }
    state_mutex_initialized = 1;
    if (latest_frame_slot_init(&pipeline.captured_frames) != 0)
        goto cleanup;
    captured_slot_initialized = 1;
    if (latest_frame_slot_init(&pipeline.annotated_frames) != 0)
        goto cleanup;
    annotated_slot_initialized = 1;

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

    pipeline.camera = &camera;
    pipeline.recognizer = recognizer;

    error = pthread_create(&recognition_thread, NULL,
                           recognition_thread_main, &pipeline);
    if (error != 0) {
        LOG_ERROR("创建识别线程失败: %s", strerror(error));
        goto cleanup;
    }
    recognition_thread_created = 1;
    error = pthread_create(&capture_thread, NULL,
                           capture_thread_main, &pipeline);
    if (error != 0) {
        LOG_ERROR("创建采集线程失败: %s", strerror(error));
        pipeline_request_stop(&pipeline, 1);
        goto cleanup;
    }
    capture_thread_created = 1;

    LOG_INFO("开始多线程实时车牌识别: %ux%u@%u，"
             "按 Ctrl+C 退出",
             camera.width, camera.height, camera.fps);
    exit_code = display_frames(&pipeline, display) == 0 ? 0 : 1;

cleanup:
    if (captured_slot_initialized && annotated_slot_initialized)
        pipeline_request_stop(&pipeline, 0);
    else
        running = 0;
    if (capture_thread_created)
        pthread_join(capture_thread, NULL);
    if (recognition_thread_created)
        pthread_join(recognition_thread, NULL);
    if (camera_started)
        (void)camera_stop(&camera);
    if (camera_opened)
        camera_close(&camera);
    display_destroy(display);
    plate_recognizer_destroy(recognizer);
    if (annotated_slot_initialized)
        latest_frame_slot_destroy(&pipeline.annotated_frames);
    if (captured_slot_initialized)
        latest_frame_slot_destroy(&pipeline.captured_frames);
    if (state_mutex_initialized)
        pthread_mutex_destroy(&pipeline.state_mutex);
    return exit_code;
}

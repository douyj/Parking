#include "camera.h"
#include "display.h"
#include "image_convert.h"
#include "latest_frame_slot.h"
#include "log.h"
#include "plate_confirmation.h"
#include "pipeline_frame.h"
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
#define QUEUE_WAIT_MS 100

// 三线程管道结构体
typedef struct {
    Camera *camera;
    plate_recognizer_t *recognizer;
    latest_frame_slot_t *captured_frames;
    latest_frame_slot_t *annotated_frames;
    pthread_mutex_t state_mutex;
    int failed;
} pipeline_t;

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

// 用来测量程序运行耗时
static uint64_t monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

// 给程序注册退出信号处理函数
static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;      // 设置处理函数
    sigemptyset(&action.sa_mask);       // 不屏蔽任何信号
    if (sigaction(SIGINT, &action, NULL) < 0 ||     // ctrl + c
        sigaction(SIGTERM, &action, NULL) < 0) {    // kill 命令
        LOG_ERROR("安装退出信号处理失败: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void pipeline_request_stop(pipeline_t *pipeline, int failed)
{
    if (failed) {
        pthread_mutex_lock(&pipeline->state_mutex);
        pipeline->failed = 1;
        pthread_mutex_unlock(&pipeline->state_mutex);
    }
    running = 0;
    latest_frame_slot_close(pipeline->captured_frames);
    latest_frame_slot_close(pipeline->annotated_frames);
}

static int pipeline_failed(pipeline_t *pipeline)
{
    int failed;

    pthread_mutex_lock(&pipeline->state_mutex);
    failed = pipeline->failed;
    pthread_mutex_unlock(&pipeline->state_mutex);
    return failed;
}

// 从一次车牌识别得到的多个结果里，挑出“最可信”的那个结果
static const plate_result_t *find_best_result(const plate_result_t *results, int result_count)
{
    const plate_result_t *best = NULL;

    for (int i = 0; i < result_count; ++i) {
        if (results[i].plate_number[0] == '\0')
            continue;
        if (best == NULL ||results[i].detect_confidence > best->detect_confidence)
            best = &results[i];
    }
    return best;
}

// 捕获线程，负责从相机获取图像并转换为 BGR 格式
static void *capture_thread_main(void *argument)
{
    pipeline_t *pipeline = argument;

    while (running) {
        CameraFrame camera_frame;           // 相机获取的原始图像帧
        pipeline_frame_t output;            // 转换之后准备交给识别线程的帧
        uint64_t convert_started_us;        // 记录图像转换开始时间
        int convert_result;
        int release_result;
        int push_result;

        memset(&camera_frame, 0, sizeof(camera_frame));
        memset(&output, 0, sizeof(output));
        output.started_us = monotonic_us();         // 记录帧开始处理的时间戳

        if (camera_get_frame(pipeline->camera, &camera_frame, 1000) != 0) {
            if (!running)
                break;
            pipeline_request_stop(pipeline, 1);
            break;
        }

        convert_started_us = monotonic_us();
        convert_result = image_convert_to_bgr(&camera_frame, &output.image);
        output.convert_us = monotonic_us() - convert_started_us;
        release_result = camera_release_frame(pipeline->camera, &camera_frame);

        if (convert_result != 0 || release_result != 0) {
            pipeline_frame_release(&output);
            pipeline_request_stop(pipeline, 1);
            break;
        }

        push_result = latest_frame_slot_push(
            pipeline->captured_frames, &output);
        if (push_result != LATEST_FRAME_SLOT_OK) {
            pipeline_frame_release(&output);
            if (push_result == LATEST_FRAME_SLOT_ERROR)
                pipeline_request_stop(pipeline, 1);
            break;
        }
    }

    latest_frame_slot_close(pipeline->captured_frames);
    return NULL;
}


// 识别线程，负责对捕获到的图像进行车牌识别
static void *recognition_thread_main(void *argument)
{
    pipeline_t *pipeline = argument;
    plate_confirmation_config_t confirmation_config = {
        .required_frames = REQUIRED_CONFIRM_FRAMES,
        .max_missed_frames = MAX_MISSED_FRAMES,
        .same_plate_cooldown_us = SAME_PLATE_COOLDOWN_US
    };
    plate_confirmation_t confirmation;

    if (plate_confirmation_init(
            &confirmation, &confirmation_config) != 0) {
        LOG_ERROR("初始化车牌确认器失败");
        pipeline_request_stop(pipeline, 1);
        return NULL;
    }

    while (running) {
        pipeline_frame_t frame;                     // 这次要识别的图像
        plate_result_t results[PLATE_MAX_RESULTS];  // 模型识别出来的多个车牌结果
        const plate_result_t *best_result;          // 这些结果里置信度最高的那个
        uint64_t recognize_started_us;              // 开始识别的时间
        int take_result;                            // 从帧槽取帧是否成功   
        int result_count;
        int confirmation_result;
        int push_result;

        memset(&frame, 0, sizeof(frame));
        take_result = latest_frame_slot_take(
            pipeline->captured_frames, &frame, QUEUE_WAIT_MS);
        if (take_result == LATEST_FRAME_SLOT_TIMEOUT)
            continue;
        if (take_result != LATEST_FRAME_SLOT_OK) {
            if (take_result == LATEST_FRAME_SLOT_ERROR)
                pipeline_request_stop(pipeline, 1);
            break;
        }

        if (frame.image.width > INT_MAX || frame.image.height > INT_MAX || frame.image.stride > INT_MAX) {
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
        frame.recognize_us = monotonic_us() - recognize_started_us;     // 记录识别耗时

        if (result_count < 0) {
            const char *error = plate_recognizer_last_error(pipeline->recognizer);
            LOG_ERROR("车牌识别失败: %s", error != NULL ? error : "未知错误");
            pipeline_frame_release(&frame);
            pipeline_request_stop(pipeline, 1);
            break;
        }

        best_result = find_best_result(results, result_count);
        // 连续帧确认
        confirmation_result = plate_confirmation_update(
            &confirmation, best_result, monotonic_us());
        if (confirmation_result < 0) {
            LOG_ERROR("更新车牌确认器失败");
            pipeline_frame_release(&frame);
            pipeline_request_stop(pipeline, 1);
            break;
        }
        if (confirmation_result == 1) {
            LOG_INFO("确认车牌=%s, 颜色=%s, 检测置信度=%.2f, "
                     "颜色置信度=%.2f",
                     best_result->plate_number, best_result->color_name,
                     best_result->detect_confidence,
                     best_result->color_confidence);
        }

        // 在当前图像上画识别结果
        if (plate_recognizer_draw(
                pipeline->recognizer, frame.image.data,
                (int)frame.image.width, (int)frame.image.height,
                (int)frame.image.stride, results, result_count) != 0) {
            const char *error = plate_recognizer_last_error(pipeline->recognizer);
            LOG_ERROR("绘制识别结果失败: %s", error != NULL ? error : "未知错误");
            pipeline_frame_release(&frame);
            pipeline_request_stop(pipeline, 1);
            break;
        }

        push_result = latest_frame_slot_push(
            pipeline->annotated_frames, &frame);
        if (push_result != LATEST_FRAME_SLOT_OK) {
            pipeline_frame_release(&frame);
            if (push_result == LATEST_FRAME_SLOT_ERROR)
                pipeline_request_stop(pipeline, 1);
            break;
        }
    }

    latest_frame_slot_close(pipeline->annotated_frames);
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
            pipeline->annotated_frames, &frame, QUEUE_WAIT_MS);
        if (take_result == LATEST_FRAME_SLOT_TIMEOUT)
            continue;
        if (take_result != LATEST_FRAME_SLOT_OK) {
            if (take_result == LATEST_FRAME_SLOT_ERROR)
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
                pipeline->captured_frames);
            uint64_t display_drops = latest_frame_slot_dropped(
                pipeline->annotated_frames);
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
    pipeline.captured_frames = latest_frame_slot_create();
    if (pipeline.captured_frames == NULL)
        goto cleanup;
    pipeline.annotated_frames = latest_frame_slot_create();
    if (pipeline.annotated_frames == NULL)
        goto cleanup;

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
    if (pipeline.captured_frames != NULL ||
        pipeline.annotated_frames != NULL)
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
    latest_frame_slot_destroy(pipeline.annotated_frames);
    latest_frame_slot_destroy(pipeline.captured_frames);
    if (state_mutex_initialized)
        pthread_mutex_destroy(&pipeline.state_mutex);
    return exit_code;
}

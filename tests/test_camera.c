#include "camera.h"
#include "display.h"
#include "image_convert.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CAMERA_DEVICE "/dev/video0"
#define DEFAULT_FRAMEBUFFER_DEVICE "/dev/fb0"
#define DEFAULT_CAMERA_WIDTH 1280U
#define DEFAULT_CAMERA_HEIGHT 720U
#define DEFAULT_CAMERA_FPS 30U

static volatile sig_atomic_t running = 1;   // 程序是否继续运行的标志位

// 处理退出信号的函数
static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}


// 安装退出信号处理函数
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

// 获取当前时间戳（毫秒级）
static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000ULL +
           (uint64_t)now.tv_nsec / 1000000ULL;
}

// 解析无符号整数
static int parse_unsigned(const char *text, unsigned int *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed == 0 || parsed > UINT_MAX)
        return -1;
    *value = (unsigned int)parsed;
    return 0;
}

// 解析像素格式字符串
static int parse_pixel_format(const char *text, unsigned int *format)
{
    if (strcasecmp(text, "MJPG") == 0 || strcasecmp(text, "MJPEG") == 0) {
        *format = V4L2_PIX_FMT_MJPEG;
        return 0;
    }
    if (strcasecmp(text, "YUYV") == 0) {
        *format = V4L2_PIX_FMT_YUYV;
        return 0;
    }
    return -1;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [camera] [width] [height] [fps] "
            "[MJPG|YUYV] [framebuffer]\n",
            program);
}

int main(int argc, char *argv[])
{
    const char *camera_device =
        argc > 1 ? argv[1] : DEFAULT_CAMERA_DEVICE;
    const char *framebuffer_device =
        argc > 6 ? argv[6] : DEFAULT_FRAMEBUFFER_DEVICE;
    unsigned int width = DEFAULT_CAMERA_WIDTH;
    unsigned int height = DEFAULT_CAMERA_HEIGHT;
    unsigned int fps = DEFAULT_CAMERA_FPS;
    unsigned int pixel_format = V4L2_PIX_FMT_MJPEG;
    Camera camera;
    display_t *display;
    display_config_t display_config;
    CameraConfig config;
    int result = 1;

    if (argc > 7 ||
        (argc > 2 && parse_unsigned(argv[2], &width) != 0) ||
        (argc > 3 && parse_unsigned(argv[3], &height) != 0) ||
        (argc > 4 && parse_unsigned(argv[4], &fps) != 0) ||
        (argc > 5 && parse_pixel_format(argv[5], &pixel_format) != 0)) {
        print_usage(argv[0]);
        return 1;
    }

    if (install_signal_handlers() != 0) return 1;
    display_config.device = framebuffer_device;
    display_config.keep_aspect_ratio = 1;
    display = display_create(&display_config);
    if (display == NULL)
        return 1;

    config.device = camera_device;
    config.width = width;
    config.height = height;
    config.fps = fps;
    config.pixel_format = pixel_format;
    if (camera_open(&camera, &config) != 0)
        goto destroy_display;
    if (camera.pixel_format != V4L2_PIX_FMT_MJPEG &&
        camera.pixel_format != V4L2_PIX_FMT_YUYV) {
        LOG_ERROR("摄像头实际格式不受支持: 0x%08x",
                  camera.pixel_format);
        goto close_camera;
    }
    if (camera_start(&camera) != 0)
        goto close_camera;

    LOG_INFO("开始实时显示: %ux%u@%u，按 Ctrl+C 退出",
             camera.width, camera.height, camera.fps);
    result = 0;
    {
        uint64_t report_started_ms = monotonic_ms();
        unsigned int displayed_frames = 0;

        while (running) {
            CameraFrame frame;
            image_bgr_frame_t bgr_frame;
            display_frame_t display_frame;
            int convert_result;

            if (camera_get_frame(&camera, &frame, 1000) != 0) {
                if (!running)
                    break;
                result = 1;
                break;
            }
            convert_result = image_convert_to_bgr(&frame, &bgr_frame);
            if (camera_release_frame(&camera, &frame) != 0) {
                image_bgr_frame_release(&bgr_frame);
                result = 1;
                break;
            }
            if (convert_result != 0) {
                image_bgr_frame_release(&bgr_frame);
                result = 1;
                break;
            }
            display_frame.data = bgr_frame.data;
            display_frame.width = bgr_frame.width;
            display_frame.height = bgr_frame.height;
            display_frame.stride = bgr_frame.stride;
            display_frame.pixel_format = DISPLAY_PIXEL_FORMAT_BGR888;
            if (display_present(display, &display_frame) != 0) {
                image_bgr_frame_release(&bgr_frame);
                result = 1;
                break;
            }
            image_bgr_frame_release(&bgr_frame);

            ++displayed_frames;
            if (monotonic_ms() - report_started_ms >= 1000U) {
                uint64_t now_ms = monotonic_ms();
                double elapsed_seconds =
                    (double)(now_ms - report_started_ms) / 1000.0;
                LOG_INFO("显示 FPS: %.1f",
                         displayed_frames / elapsed_seconds);
                displayed_frames = 0;
                report_started_ms = now_ms;
            }
        }
    }

    (void)camera_stop(&camera);
close_camera:
    camera_close(&camera);
destroy_display:
    display_destroy(display);
    return result;
}

#include "parking_lvgl_demo.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_DRM_DEVICE "/dev/dri/card0"
#define DEFAULT_FONT_PATH "fonts/platech.ttf"
#define EXPECTED_WIDTH 1024
#define EXPECTED_HEIGHT 600

static volatile sig_atomic_t running = 1;

static void print_lvgl_log(lv_log_level_t level, const char *message)
{
    (void)level;

    if (message != NULL)
        fputs(message, stderr);
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        fprintf(stderr, "安装退出信号处理失败: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [--device PATH] [--connector ID] [--font PATH]\n"
            "  --device PATH   DRM 设备，默认 /dev/dri/card0\n"
            "  --connector ID  DRM connector ID，默认 -1 自动选择\n"
            "  --font PATH     中文 TTF 字体，默认 fonts/platech.ttf\n",
            program != NULL ? program : "test_lvgl_display");
}

static int parse_connector(const char *text, int64_t *connector_id)
{
    char *end = NULL;
    intmax_t value;

    errno = 0;
    value = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < INT64_MIN || value > INT64_MAX)
        return -1;

    *connector_id = (int64_t)value;
    return 0;
}

static int parse_arguments(int argc, char **argv,
                           const char **device,
                           int64_t *connector_id,
                           const char **font_path)
{
    int i;

    *device = DEFAULT_DRM_DEVICE;
    *connector_id = -1;
    *font_path = DEFAULT_FONT_PATH;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 1;
        }

        if (i + 1 >= argc) {
            print_usage(argv[0]);
            return -1;
        }

        if (strcmp(argv[i], "--device") == 0) {
            *device = argv[++i];
        } else if (strcmp(argv[i], "--connector") == 0) {
            if (parse_connector(argv[++i], connector_id) != 0) {
                print_usage(argv[0]);
                return -1;
            }
        } else if (strcmp(argv[i], "--font") == 0) {
            *font_path = argv[++i];
        } else {
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    parking_lvgl_demo_config_t demo_config;
    parking_lvgl_demo_t *demo = NULL;
    lv_display_t *display = NULL;
    const char *device;
    const char *font_path;
    int64_t connector_id;
    int argument_result;
    int exit_code = 1;

    argument_result = parse_arguments(argc, argv, &device,
                                      &connector_id, &font_path);
    if (argument_result > 0)
        return 0;
    if (argument_result < 0 || install_signal_handlers() != 0)
        return 1;

    lv_init();
    lv_log_register_print_cb(print_lvgl_log);

    display = lv_linux_drm_create();
    if (display == NULL) {
        fprintf(stderr, "创建 LVGL DRM 显示失败\n");
        goto cleanup;
    }

    if (lv_linux_drm_set_file(display, device, connector_id) !=
        LV_RESULT_OK) {
        fprintf(stderr, "打开 DRM 显示失败: %s, connector=%" PRId64 "\n",
                device, connector_id);
        goto cleanup;
    }

    fprintf(stderr, "LVGL DRM 初始化完成: %s, %" PRId32 "x%" PRId32 "\n",
            device,
            lv_display_get_horizontal_resolution(display),
            lv_display_get_vertical_resolution(display));
    if (lv_display_get_horizontal_resolution(display) != EXPECTED_WIDTH ||
        lv_display_get_vertical_resolution(display) != EXPECTED_HEIGHT) {
        fprintf(stderr,
                "警告: 显示模式不是预期的 %dx%d，界面将按实际宽度显示\n",
                EXPECTED_WIDTH, EXPECTED_HEIGHT);
    }

    memset(&demo_config, 0, sizeof(demo_config));
    demo_config.font_path = font_path;
    demo = parking_lvgl_demo_create(&demo_config);
    if (demo == NULL) {
        fprintf(stderr, "创建 LVGL 演示界面失败，请检查字体: %s\n",
                font_path);
        goto cleanup;
    }

    while (running) {
        uint32_t delay_ms = lv_timer_handler();

        if (delay_ms == LV_NO_TIMER_READY || delay_ms > 50U)
            delay_ms = 50U;
        if (delay_ms == 0U)
            delay_ms = 1U;
        lv_delay_ms(delay_ms);
    }

    exit_code = 0;

cleanup:
    parking_lvgl_demo_destroy(demo);
    if (display != NULL)
        lv_display_delete(display);
    lv_deinit();
    return exit_code;
}

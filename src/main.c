#include "display.h"
#include "log.h"
#include "plate_pipeline.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <string.h>

#define CAMERA_DEVICE           "/dev/video0"
#define FRAMEBUFFER_DEVICE      "/dev/fb0"

#define CAMERA_WIDTH            1280U
#define CAMERA_HEIGHT           720U
#define CAMERA_FPS              30U

#define DETECT_MODEL            "models/yolo26s-plate-detect-rk3576.rknn"
#define RECOGNITION_MODEL       "models/plate_rec_color-rk3576.rknn"
#define FONT_PATH               "fonts/platech.ttf"

#define PIPELINE_WAIT_MS        100U
#define ERROR_MESSAGE_CAPACITY  256U

static volatile sig_atomic_t running = 1;

// 信号处理函数，处理ctrl+c信号，kill等信号，退出程序
static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;       //当这个信号发生时，就调用 handle_signal()
    sigemptyset(&action.sa_mask);           // 信号处理期间不额外阻塞其他信号

    //如果是中断或终止信号，就调用 handle_signal() 函数
    if(sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0){
        LOG_ERROR("安装信号处理失败: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int run_loop(plate_pipeline_t *pipeline, display_t *display)
{
    while(running){
        plate_pipeline_event_t event;
        pipeline_frame_t frame;
        display_frame_t display_frame;
        int result;

        memset(&event, 0, sizeof(event));
        result = plate_pipeline_take_event(pipeline, &event);   //从管道中获取事件
        if(result < 0){
            LOG_ERROR("读取稳定车牌事件失败");
            return -1;
        }

        if(result == 1){
            LOG_INFO("稳定车牌: %s, 颜色：%s, 置信度：%.2f",
                event.result.plate_number, event.result.color_name, event.result.detect_confidence);
        }

        memset(&frame, 0, sizeof(frame));
        result = plate_pipeline_take_frame(pipeline, &frame, PIPELINE_WAIT_MS);

        if(result == PLATE_PIPELINE_TIMEOUT)
            continue;

        if(result == PLATE_PIPELINE_CLOSED)
            break;

        if(result != PLATE_PIPELINE_OK){
            LOG_ERROR("读取识别帧失败");
            return -1;
        }

        memset(&display_frame, 0, sizeof(display_frame));
        display_frame.data = frame.image.data;
        display_frame.width = frame.image.width;
        display_frame.height = frame.image.height;
        display_frame.stride = frame.image.stride;
        display_frame.pixel_format = DISPLAY_PIXEL_FORMAT_BGR888;

        if(display_present(display, &display_frame) != 0){
            pipeline_frame_release(&frame);
            LOG_ERROR("显示图像失败");
            return -1;
        }

        pipeline_frame_release(&frame);
    }

    return plate_pipeline_failed(pipeline) ? -1 : 0;
}


int main(void)
{
    plate_pipeline_config_t pipeline_config;
    display_config_t display_config;
    plate_pipeline_t *pipeline = NULL;
    display_t *display = NULL;
    int exit_code = 1;

    if(install_signal_handlers() != 0){
        return 1;
    }

    memset(&display_config, 0, sizeof(display_config));
    display_config.device = FRAMEBUFFER_DEVICE;
    display_config.keep_aspect_ratio = 1;

    display = display_create(&display_config);
    if(display == NULL){
        goto cleanup;
    }

    memset(&pipeline_config, 0, sizeof(pipeline_config));
    pipeline_config.camera.device = CAMERA_DEVICE;
    pipeline_config.camera.width = CAMERA_WIDTH;
    pipeline_config.camera.height = CAMERA_HEIGHT;
    pipeline_config.camera.fps = CAMERA_FPS;
    pipeline_config.camera.pixel_format = V4L2_PIX_FMT_MJPEG;

    pipeline_config.detect_model_path = DETECT_MODEL;
    pipeline_config.recognition_model_path = RECOGNITION_MODEL;
    pipeline_config.font_path = FONT_PATH;
    pipeline_config.confidence_threshold = 0.30F;

    pipeline_config.confirmation.required_frames = 3U;
    pipeline_config.confirmation.max_missed_frames = 3U;
    pipeline_config.confirmation.same_plate_cooldown_us = 10000000ULL;

    pipeline_config.camera_timeout_ms = 1000;
    pipeline_config.frame_wait_ms = PIPELINE_WAIT_MS;

    pipeline = plate_pipeline_create(&pipeline_config);
    if (pipeline == NULL)
        goto cleanup;

    if (plate_pipeline_start(pipeline) != PLATE_PIPELINE_OK)
        goto cleanup;

    LOG_INFO("Parking 主程序启动，按 Ctrl+C 退出");

    if (run_loop(pipeline, display) == 0)
        exit_code = 0;

cleanup:
    plate_pipeline_stop(pipeline);
    plate_pipeline_destroy(pipeline);
    display_destroy(display);

    return exit_code;
}

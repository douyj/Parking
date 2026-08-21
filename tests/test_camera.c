#include "camera.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAMERA_DEVICE "/dev/video1"
#define FRAMEBUFFER_DEVICE "/dev/fb0"
#define CAMERA_WIDTH 640U
#define CAMERA_HEIGHT 480U
#define CAMERA_FPS 30U

typedef struct {
    int fd;
    unsigned char *memory;
    size_t memory_size;
    struct fb_var_screeninfo variable;
    struct fb_fix_screeninfo fixed;
} Framebuffer;

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int framebuffer_open(Framebuffer *framebuffer)
{
    memset(framebuffer, 0, sizeof(*framebuffer));
    framebuffer->fd = -1;

    framebuffer->fd = open(FRAMEBUFFER_DEVICE, O_RDWR);
    if (framebuffer->fd < 0) {
        LOG_ERROR("打开 %s 失败: %s", FRAMEBUFFER_DEVICE, strerror(errno));
        return -1;
    }
    if (ioctl(framebuffer->fd, FBIOGET_VSCREENINFO,
              &framebuffer->variable) < 0 ||
        ioctl(framebuffer->fd, FBIOGET_FSCREENINFO,
              &framebuffer->fixed) < 0) {
        LOG_ERROR("读取 framebuffer 信息失败: %s", strerror(errno));
        close(framebuffer->fd);
        framebuffer->fd = -1;
        return -1;
    }
    if (framebuffer->variable.bits_per_pixel != 16) {
        LOG_ERROR("LCD 不是 RGB565: bits_per_pixel=%u",
                  framebuffer->variable.bits_per_pixel);
        close(framebuffer->fd);
        framebuffer->fd = -1;
        return -1;
    }

    framebuffer->memory_size = framebuffer->fixed.smem_len;
    framebuffer->memory = mmap(NULL, framebuffer->memory_size,
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               framebuffer->fd, 0);
    if (framebuffer->memory == MAP_FAILED) {
        framebuffer->memory = NULL;
        LOG_ERROR("映射 framebuffer 失败: %s", strerror(errno));
        close(framebuffer->fd);
        framebuffer->fd = -1;
        return -1;
    }

    LOG_INFO("LCD: %ux%u, virtual=%ux%u, stride=%u, bpp=%u",
             framebuffer->variable.xres, framebuffer->variable.yres,
             framebuffer->variable.xres_virtual,
             framebuffer->variable.yres_virtual,
             framebuffer->fixed.line_length,
             framebuffer->variable.bits_per_pixel);
    return 0;
}

static void framebuffer_close(Framebuffer *framebuffer)
{
    if (framebuffer->memory != NULL)
        munmap(framebuffer->memory, framebuffer->memory_size);
    if (framebuffer->fd >= 0)
        close(framebuffer->fd);
    memset(framebuffer, 0, sizeof(*framebuffer));
    framebuffer->fd = -1;
}

static int framebuffer_show_rgb565(const Framebuffer *framebuffer,
                                   const CameraFrame *frame)
{
    unsigned int copy_width;
    unsigned int copy_height;
    unsigned int source_x;
    unsigned int source_y;
    unsigned int destination_x;
    unsigned int destination_y;
    size_t required_size;

    if (frame->pixel_format != V4L2_PIX_FMT_RGB565) {
        LOG_ERROR("摄像头实际格式不是 RGB565: 0x%08x",
                  frame->pixel_format);
        return -1;
    }

    copy_width = frame->width < framebuffer->variable.xres
                     ? frame->width : framebuffer->variable.xres;
    copy_height = frame->height < framebuffer->variable.yres
                      ? frame->height : framebuffer->variable.yres;
    source_x = (frame->width - copy_width) / 2U;
    source_y = (frame->height - copy_height) / 2U;
    destination_x = (framebuffer->variable.xres - copy_width) / 2U;
    destination_y = (framebuffer->variable.yres - copy_height) / 2U;

    required_size = (size_t)frame->bytes_per_line * frame->height;
    if (frame->bytes_per_line < frame->width * 2U ||
        frame->size < required_size) {
        LOG_ERROR("RGB565 帧长度异常: bytes=%zu, required=%zu",
                  frame->size, required_size);
        return -1;
    }

    for (unsigned int row = 0; row < copy_height; ++row) {
        const unsigned char *source =
            (const unsigned char *)frame->data +
            (size_t)(source_y + row) * frame->bytes_per_line +
            (size_t)source_x * 2U;
        unsigned char *destination = framebuffer->memory +
            (size_t)(framebuffer->variable.yoffset + destination_y + row) *
                framebuffer->fixed.line_length +
            (size_t)(framebuffer->variable.xoffset + destination_x) * 2U;

        memcpy(destination, source, (size_t)copy_width * 2U);
    }
    return 0;
}

int main(void)
{
    Camera camera;
    Framebuffer framebuffer;
    CameraConfig config = {
        .device = CAMERA_DEVICE,
        .width = CAMERA_WIDTH,
        .height = CAMERA_HEIGHT,
        .fps = CAMERA_FPS,
        .pixel_format = V4L2_PIX_FMT_RGB565,
    };
    int result = 1;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (framebuffer_open(&framebuffer) != 0)
        return 1;
    if (camera_open(&camera, &config) != 0)
        goto close_framebuffer;
    if (camera.pixel_format != V4L2_PIX_FMT_RGB565) {
        LOG_ERROR("驱动没有接受 RGB565，实际 FOURCC=0x%08x",
                  camera.pixel_format);
        goto close_camera;
    }
    if (camera_start(&camera) != 0)
        goto close_camera;

    LOG_INFO("开始实时显示，按 Ctrl+C 退出");
    result = 0;
    while (running) {
        CameraFrame frame;

        if (camera_get_frame(&camera, &frame, 1000) != 0) {
            if (!running)
                break;
            result = 1;
            break;
        }
        if (framebuffer_show_rgb565(&framebuffer, &frame) != 0)
            result = 1;
        if (camera_release_frame(&camera, &frame) != 0) {
            result = 1;
            break;
        }
        if (result != 0)
            break;
    }

    camera_stop(&camera);
close_camera:
    camera_close(&camera);
close_framebuffer:
    framebuffer_close(&framebuffer);
    return result;
}

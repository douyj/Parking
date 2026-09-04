#include "display.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct display {
    int fd;                         // 显示设备文件描述符
    unsigned char *memory;          // mmap() 后得到的显存地址
    size_t memory_size;             // 映射的显存长度
    struct fb_var_screeninfo variable;      // 变量屏幕信息
    struct fb_fix_screeninfo fixed;          // 固定屏幕信息
    uint32_t red_values[256];               // 红色值缓存
    uint32_t green_values[256];             // 绿色值缓存
    uint32_t blue_values[256];              // 蓝色值缓存
    uint32_t opaque;                       // 不透明值
    int keep_aspect_ratio;                // 是否保持宽高比
};

// 检查颜色位域
static int bitfield_is_valid(const struct fb_bitfield *field,
                             unsigned int bits_per_pixel)
{
    return field->length <= bits_per_pixel &&
           field->offset <= bits_per_pixel - field->length;
}

// 把 8 位颜色转换成屏幕位域
static uint32_t pack_channel(unsigned char value,
                             const struct fb_bitfield *field)
{
    uint64_t maximum;
    uint64_t scaled;

    if (field->length == 0)
        return 0;
    if (field->length >= 32)
        maximum = UINT32_MAX;
    else
        maximum = (1ULL << field->length) - 1ULL;

    scaled = ((uint64_t)value * maximum + 127ULL) / 255ULL;
    return (uint32_t)(scaled << field->offset);
}


// 写入一个屏幕像素
static void write_pixel(unsigned char *destination,
                        unsigned int bytes_per_pixel, uint32_t pixel)
{
    if (bytes_per_pixel == 2) {
        uint16_t value = (uint16_t)pixel;
        memcpy(destination, &value, sizeof(value));
    } else if (bytes_per_pixel == 3) {
        destination[0] = (unsigned char)(pixel & 0xffU);
        destination[1] = (unsigned char)((pixel >> 8U) & 0xffU);
        destination[2] = (unsigned char)((pixel >> 16U) & 0xffU);
    } else {
        memcpy(destination, &pixel, sizeof(pixel));
    }
}

/*
    @brief 创建显示对象 
    @param config 显示配置结构体指针
    @return 显示对象指针，或 NULL 如果失败
*/
display_t *display_create(const display_config_t *config)
{
    display_t *display;
    const char *device;
    unsigned int bits_per_pixel;     // 每像素位数
    unsigned int bytes_per_pixel;    // 每像素字节数
    uint64_t visible_row_bytes;       // 可见行字节数
    uint64_t required_size;           // 所需内存大小

    if (config == NULL) {
        LOG_ERROR("显示配置不能为空");
        return NULL;
    }

    device = config->device;
    if (device == NULL || device[0] == '\0') {
        LOG_ERROR("framebuffer 设备路径不能为空");
        return NULL;
    }

    display = calloc(1, sizeof(*display));
    if (display == NULL) {
        LOG_ERROR("分配显示对象失败");
        return NULL;
    }

    display->fd = -1;
    display->keep_aspect_ratio = config->keep_aspect_ratio != 0;
    // 打开 framebuffer 设备
    display->fd = open(device, O_RDWR | O_CLOEXEC);
    if (display->fd < 0) {
        LOG_ERROR("打开 %s 失败: %s", device, strerror(errno));
        goto fail;
    }

    if (ioctl(display->fd, FBIOGET_VSCREENINFO, &display->variable) < 0 ||
        ioctl(display->fd, FBIOGET_FSCREENINFO, &display->fixed) < 0) {
        LOG_ERROR("读取 framebuffer 信息失败: %s", strerror(errno));
        goto fail;
    }

    bits_per_pixel = display->variable.bits_per_pixel;

    if (display->variable.xres == 0 || display->variable.yres == 0 ||
        (bits_per_pixel != 16 && bits_per_pixel != 24 &&
         bits_per_pixel != 32)) {
        LOG_ERROR("不支持的 framebuffer: %ux%u, bpp=%u",
                  display->variable.xres, display->variable.yres,
                  bits_per_pixel);
        goto fail;
    }
    if (!bitfield_is_valid(&display->variable.red, bits_per_pixel) ||
        !bitfield_is_valid(&display->variable.green, bits_per_pixel) ||
        !bitfield_is_valid(&display->variable.blue, bits_per_pixel) ||
        !bitfield_is_valid(&display->variable.transp, bits_per_pixel)) {
        LOG_ERROR("framebuffer 颜色位域无效");
        goto fail;
    }

    bytes_per_pixel = bits_per_pixel / 8U;  // 每像素字节数
    visible_row_bytes = ((uint64_t)display->variable.xoffset + display->variable.xres) * bytes_per_pixel;
    if (visible_row_bytes > display->fixed.line_length) {
        LOG_ERROR("framebuffer stride 无效: %u",
                  display->fixed.line_length);
        goto fail;
    }
    required_size = ((uint64_t)display->variable.yoffset + display->variable.yres - 1U) * display->fixed.line_length + visible_row_bytes;
    if (display->fixed.smem_len == 0 ||
        required_size > display->fixed.smem_len) {
        LOG_ERROR("framebuffer 映射长度无效: required=%llu, smem_len=%u",
                  (unsigned long long)required_size,
                  display->fixed.smem_len);
        goto fail;
    }

    display->memory_size = display->fixed.smem_len;
    display->memory = mmap(NULL, display->memory_size,
                           PROT_READ | PROT_WRITE, MAP_SHARED,
                           display->fd, 0);
    if (display->memory == MAP_FAILED) {
        display->memory = NULL;
        LOG_ERROR("映射 framebuffer 失败: %s", strerror(errno));
        goto fail;
    }

    for (unsigned int value = 0; value < 256; ++value) {
        display->red_values[value] =
            pack_channel((unsigned char)value, &display->variable.red);
        display->green_values[value] =
            pack_channel((unsigned char)value, &display->variable.green);
        display->blue_values[value] =
            pack_channel((unsigned char)value, &display->variable.blue);
    }
    display->opaque = pack_channel(255, &display->variable.transp);

    LOG_INFO("LCD: %ux%u, stride=%u, bpp=%u, "
             "R=%u:%u G=%u:%u B=%u:%u",
             display->variable.xres, display->variable.yres,
             display->fixed.line_length, bits_per_pixel,
             display->variable.red.offset, display->variable.red.length,
             display->variable.green.offset, display->variable.green.length,
             display->variable.blue.offset, display->variable.blue.length);
    return display;

fail:
    display_destroy(display);
    return NULL;
}

int display_present(display_t *display, const display_frame_t *frame)
{
    unsigned int screen_width;
    unsigned int screen_height;
    unsigned int display_width;
    unsigned int display_height;
    unsigned int left;
    unsigned int top;
    unsigned int bytes_per_pixel;
    unsigned int red_index;
    unsigned int blue_index;

    if (display == NULL || frame == NULL || frame->data == NULL ||
        frame->width == 0 || frame->height == 0 ||
        frame->width > UINT_MAX / 3U ||
        frame->stride < frame->width * 3U) {
        LOG_ERROR("显示帧参数无效");
        return -1;
    }
    if (frame->pixel_format == DISPLAY_PIXEL_FORMAT_RGB888) {
        red_index = 0;
        blue_index = 2;
    } else if (frame->pixel_format == DISPLAY_PIXEL_FORMAT_BGR888) {
        red_index = 2;
        blue_index = 0;
    } else {
        LOG_ERROR("不支持的显示输入像素格式: %d",
                  frame->pixel_format);
        return -1;
    }

    screen_width = display->variable.xres;
    screen_height = display->variable.yres;
    if (display->keep_aspect_ratio) {
        if ((uint64_t)screen_width * frame->height <=
            (uint64_t)screen_height * frame->width) {
            display_width = screen_width;
            display_height = (unsigned int)((uint64_t)frame->height *
                                            screen_width / frame->width);
        } else {
            display_height = screen_height;
            display_width = (unsigned int)((uint64_t)frame->width *
                                           screen_height / frame->height);
        }
    } else {
        display_width = screen_width;
        display_height = screen_height;
    }
    if (display_width == 0 || display_height == 0) {
        LOG_ERROR("计算出的显示区域无效");
        return -1;
    }
    left = (screen_width - display_width) / 2U;
    top = (screen_height - display_height) / 2U;
    bytes_per_pixel = display->variable.bits_per_pixel / 8U;

    for (unsigned int y = 0; y < screen_height; ++y) {
        unsigned char *output = display->memory +
            (size_t)(display->variable.yoffset + y) *
                display->fixed.line_length +
            (size_t)display->variable.xoffset * bytes_per_pixel;

        for (unsigned int x = 0; x < screen_width; ++x) {
            uint32_t pixel = display->opaque;
            if (x >= left && x < left + display_width &&
                y >= top && y < top + display_height) {
                unsigned int source_x =
                    (unsigned int)((uint64_t)(x - left) * frame->width /
                                   display_width);
                unsigned int source_y =
                    (unsigned int)((uint64_t)(y - top) * frame->height /
                                   display_height);
                const unsigned char *source =
                    frame->data + (size_t)source_y * frame->stride +
                    (size_t)source_x * 3U;
                pixel |= display->red_values[source[red_index]] |
                         display->green_values[source[1]] |
                         display->blue_values[source[blue_index]];
            }
            write_pixel(output, bytes_per_pixel, pixel);
            output += bytes_per_pixel;
        }
    }
    return 0;
}

unsigned int display_width(const display_t *display)
{
    return display != NULL ? display->variable.xres : 0;
}

unsigned int display_height(const display_t *display)
{
    return display != NULL ? display->variable.yres : 0;
}

void display_destroy(display_t *display)
{
    if (display == NULL)
        return;
    if (display->memory != NULL)
        munmap(display->memory, display->memory_size);
    if (display->fd >= 0)
        close(display->fd);
    free(display);
}

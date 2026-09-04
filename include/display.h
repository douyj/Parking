#ifndef DISPLAY_H
#define DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct display display_t;

typedef enum {
    DISPLAY_PIXEL_FORMAT_RGB888 = 0,
    DISPLAY_PIXEL_FORMAT_BGR888
} display_pixel_format_t;

typedef struct {
    const char *device;       /* framebuffer 设备，例如 /dev/fb0 */
    int keep_aspect_ratio;    /* 非 0 时保持输入图像宽高比 */
} display_config_t;

typedef struct {
    const unsigned char *data;
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    display_pixel_format_t pixel_format;
} display_frame_t;

/* 打开并映射 framebuffer；失败返回 NULL。 */
display_t *display_create(const display_config_t *config);

/* 缩放并显示一帧 RGB888/BGR888 图像。 */
int display_present(display_t *display, const display_frame_t *frame);

unsigned int display_width(const display_t *display);
unsigned int display_height(const display_t *display);

/* 解除 framebuffer 映射、关闭设备并释放显示对象。 */
void display_destroy(display_t *display);

#ifdef __cplusplus
}
#endif

#endif

#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* 摄像头缓冲区结构体 */
typedef struct {
    void *start;
    size_t length;
    int in_use;
} CameraBuffer;

/* 摄像头配置结构体 */
typedef struct {
    const char *device;     // 摄像头设备路径，例如 /dev/video0
    unsigned int width;     // 图像宽度
    unsigned int height;     // 图像高度
    unsigned int fps;     // 帧率
    unsigned int pixel_format;     // 像素格式，例如 V4L2_PIX_FMT_MJPEG 或 V4L2_PIX_FMT_YUYV
} CameraConfig;

/* data 指向 mmap 缓冲区；使用后必须调用 camera_release_frame()。 */
typedef struct {
    void *data;
    size_t size;
    unsigned int width;
    unsigned int height;
    unsigned int pixel_format;
    unsigned int bytes_per_line;
    uint64_t timestamp_ms;
    unsigned int sequence;
    unsigned int buffer_index;
} CameraFrame;

/* 摄像头结构体 */
typedef struct {
    int fd;     // 文件描述符   
    unsigned int width;     // 图像宽度
    unsigned int height;     // 图像高度
    unsigned int fps;     // 帧率
    unsigned int pixel_format;     // 像素格式
    unsigned int bytes_per_line;     // 每行字节数
    size_t image_size;     // 图像大小
    CameraBuffer *buffers;     // 缓冲区数组
    unsigned int buffer_count;   // 缓冲区数量
    int streaming;     // 是否正在流式传输
} Camera;

int camera_open(Camera *camera, const CameraConfig *config);
int camera_start(Camera *camera);
int camera_get_frame(Camera *camera, CameraFrame *frame, int timeout_ms);
int camera_release_frame(Camera *camera, CameraFrame *frame);
int camera_stop(Camera *camera);
void camera_close(Camera *camera);

#ifdef __cplusplus
}
#endif

#endif

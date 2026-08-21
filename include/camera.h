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
    const char *device;
    unsigned int width;
    unsigned int height;
    unsigned int fps;
    unsigned int pixel_format;
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
    int fd;
    unsigned int width;
    unsigned int height;
    unsigned int fps;
    unsigned int pixel_format;
    unsigned int bytes_per_line;
    size_t image_size;
    CameraBuffer *buffers;
    unsigned int buffer_count;
    int streaming;
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

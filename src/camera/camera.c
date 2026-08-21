#include "camera.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAMERA_DEFAULT_BUFFER_COUNT 4U

/*
    @brief 执行 ioctl 操作，处理 EINTR 错误
    @param fd: 文件描述符
    @param request: ioctl 请求类型
    @param arg: ioctl 请求参数
    @return ioctl 返回值
    @note 文件描述符必须是打开的
*/
static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

/*
    @brief 清空摄像头帧
    @param frame: 摄像头帧指针
    @note 指针必须是有效且非 NULL 的 CameraFrame 结构体指针
    @note 该函数会将 CameraFrame 结构体的所有字段设置为 0 
*/
static void clear_frame(CameraFrame *frame)
{
    if (frame != NULL)
        memset(frame, 0, sizeof(*frame));
}

/*
    @brief 打开摄像头
    @param camera: 摄像头指针
    @param config: 摄像头配置指针
    @return 0 成功，-1 失败
*/
int camera_open(Camera *camera, const CameraConfig *config)
{
    struct v4l2_capability capability;  // 查询摄像头能力
    struct v4l2_format format;          // 设置摄像头格式
    struct v4l2_streamparm parameters;  // 设置摄像头流参数参数 
    struct v4l2_requestbuffers request;  // 请求摄像头缓冲区
    unsigned int mapped_count = 0;      // 已映射缓冲区数量
    __u32 capabilities;                  // 摄像头能力

    if (camera == NULL || config == NULL || config->device == NULL ||
        config->device[0] == '\0' || config->width == 0 ||
        config->height == 0 || config->fps == 0 || config->pixel_format == 0) {
        LOG_ERROR("%s", "摄像头参数无效");
        return -1;
    }

    // 初始化摄像头结构体
    memset(camera, 0, sizeof(*camera));
    camera->fd = -1;
    memset(&capability, 0, sizeof(capability));
    memset(&format, 0, sizeof(format));
    memset(&parameters, 0, sizeof(parameters));
    memset(&request, 0, sizeof(request));

    // 打开摄像头
    camera->fd = open(config->device, O_RDWR | O_NONBLOCK);
    if (camera->fd < 0) {
        LOG_ERROR("打开摄像头 %s 失败: %s", config->device, strerror(errno));
        return -1;
    }

    // 查询摄像头能力
    if (xioctl(camera->fd, VIDIOC_QUERYCAP, &capability) < 0) {
        LOG_ERROR("查询摄像头能力失败: %s", strerror(errno));
        goto fail;
    }
    // 检查摄像头是否支持单平面视频采集
    capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                       ? capability.device_caps : capability.capabilities;
    if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_ERROR("设备 %s 不支持单平面视频采集", config->device);
        goto fail;
    }
    // 检查摄像头是否支持流式 I/O
    if (!(capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERROR("设备 %s 不支持流式 I/O", config->device);
        goto fail;
    }

    // 设置摄像头格式
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = config->width;
    format.fmt.pix.height = config->height;
    format.fmt.pix.pixelformat = config->pixel_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;  // 选择任意场
    if (xioctl(camera->fd, VIDIOC_S_FMT, &format) < 0) {
        LOG_ERROR("设置摄像头格式失败: %s", strerror(errno));
        goto fail;
    }

    // 设置摄像头流参数
    camera->width = format.fmt.pix.width;
    camera->height = format.fmt.pix.height;
    camera->pixel_format = format.fmt.pix.pixelformat;
    camera->bytes_per_line = format.fmt.pix.bytesperline;
    camera->image_size = format.fmt.pix.sizeimage;
    if (camera->width == 0 || camera->height == 0 ||
        camera->pixel_format == 0) {
        LOG_ERROR("驱动返回无效格式: %ux%u, fourcc=0x%08x",
                  camera->width, camera->height, camera->pixel_format);
        goto fail;
    }
    if (camera->bytes_per_line == 0 &&
        (camera->pixel_format == V4L2_PIX_FMT_RGB565 ||
         camera->pixel_format == V4L2_PIX_FMT_YUYV)) {
        camera->bytes_per_line = camera->width * 2U;
    }
    if (camera->image_size == 0 && camera->bytes_per_line != 0) {
        camera->image_size =
            (size_t)camera->bytes_per_line * camera->height;
    }

    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = config->fps;
    if (xioctl(camera->fd, VIDIOC_S_PARM, &parameters) < 0) {
        LOG_WARN("摄像头不接受帧率 %u: %s", config->fps, strerror(errno));
        camera->fps = config->fps;
    } else if (parameters.parm.capture.timeperframe.numerator != 0) {   
        camera->fps = parameters.parm.capture.timeperframe.denominator /
                      parameters.parm.capture.timeperframe.numerator;
    } else {
        camera->fps = config->fps;
    }

    // 请求摄像头缓冲区
    request.count = CAMERA_DEFAULT_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(camera->fd, VIDIOC_REQBUFS, &request) < 0) {
        LOG_ERROR("申请摄像头缓冲区失败: %s", strerror(errno));
        goto fail;
    }
    if (request.count < 2) {
        LOG_ERROR("驱动提供的缓冲区不足: %u", request.count);
        goto fail;
    }

    // 分配摄像头缓冲区信息
    camera->buffers = calloc(request.count, sizeof(*camera->buffers));
    if (camera->buffers == NULL) {
        LOG_ERROR("%s", "分配摄像头缓冲区信息失败");
        goto fail;
    }
    camera->buffer_count = request.count;

    for (unsigned int i = 0; i < request.count; ++i) {
        struct v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(camera->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            LOG_ERROR("查询缓冲区[%u]失败: %s", i, strerror(errno));
            goto fail;
        }
        camera->buffers[i].length = buffer.length;
        camera->buffers[i].start = mmap(NULL, buffer.length,
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        camera->fd, buffer.m.offset);
        if (camera->buffers[i].start == MAP_FAILED) {
            camera->buffers[i].start = NULL;
            camera->buffers[i].length = 0;
            LOG_ERROR("映射缓冲区[%u]失败: %s", i, strerror(errno));
            goto fail;
        }
        mapped_count++;
    }

    LOG_INFO("摄像头已打开: %s, %ux%u, fps=%u, buffers=%u",
             config->device, camera->width, camera->height,
             camera->fps, camera->buffer_count);
    return 0;

fail:
    if (camera->buffers != NULL) {
        for (unsigned int i = 0; i < mapped_count; ++i)
            munmap(camera->buffers[i].start, camera->buffers[i].length);
        free(camera->buffers);
    }
    if (camera->fd >= 0)
        close(camera->fd);
    memset(camera, 0, sizeof(*camera));
    camera->fd = -1;
    return -1;
}

/*
    @brief 启动摄像头采集
    @param camera 摄像头结构体指针
    @return 0 成功，-1 失败
*/
int camera_start(Camera *camera)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (camera == NULL || camera->fd < 0 || camera->buffers == NULL ||
        camera->buffer_count == 0) {
        LOG_ERROR("%s", "摄像头未打开");
        return -1;
    }
    if (camera->streaming)
        return 0;

    for (unsigned int i = 0; i < camera->buffer_count; ++i) {
        struct v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(camera->fd, VIDIOC_QBUF, &buffer) < 0) {
            LOG_ERROR("缓冲区[%u]入队失败: %s", i, strerror(errno));
            return -1;
        }
        camera->buffers[i].in_use = 0;
    }

    if (xioctl(camera->fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("启动摄像头采集失败: %s", strerror(errno));
        return -1;
    }
    camera->streaming = 1;
    return 0;
}

/*
    @brief 获取摄像头帧
    @param camera 摄像头结构体指针
    @param frame 摄像头帧结构体指针
    @param timeout_ms 超时时间（毫秒）
    @return 0 成功，-1 失败
*/
int camera_get_frame(Camera *camera, CameraFrame *frame, int timeout_ms)
{
    struct pollfd descriptor;
    struct v4l2_buffer buffer;
    int poll_result;

    if (camera == NULL || frame == NULL || camera->fd < 0 ||
        !camera->streaming) {
        LOG_ERROR("%s", "获取帧时摄像头状态无效");
        return -1;
    }
    clear_frame(frame);
    memset(&descriptor, 0, sizeof(descriptor));
    memset(&buffer, 0, sizeof(buffer));

    descriptor.fd = camera->fd;
    descriptor.events = POLLIN;
    do {
        poll_result = poll(&descriptor, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        LOG_WARN("%s", "等待摄像头帧超时");
        return -1;
    }
    if (poll_result < 0) {
        LOG_ERROR("等待摄像头帧失败: %s", strerror(errno));
        return -1;
    }
    if (!(descriptor.revents & POLLIN)) {
        LOG_ERROR("摄像头 poll 状态异常: 0x%x", descriptor.revents);
        return -1;
    }

    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (xioctl(camera->fd, VIDIOC_DQBUF, &buffer) < 0) {
        LOG_ERROR("摄像头帧出队失败: %s", strerror(errno));
        return -1;
    }
    if (buffer.index >= camera->buffer_count ||
        buffer.bytesused > camera->buffers[buffer.index].length) {
        LOG_ERROR("驱动返回无效帧: index=%u, bytes=%u",
                  buffer.index, buffer.bytesused);
        if (buffer.index < camera->buffer_count)
            xioctl(camera->fd, VIDIOC_QBUF, &buffer);
        return -1;
    }

    camera->buffers[buffer.index].in_use = 1;
    frame->data = camera->buffers[buffer.index].start;
    frame->size = buffer.bytesused;
    frame->width = camera->width;
    frame->height = camera->height;
    frame->pixel_format = camera->pixel_format;
    frame->bytes_per_line = camera->bytes_per_line;
    frame->timestamp_ms = (uint64_t)buffer.timestamp.tv_sec * 1000ULL +
                          (uint64_t)buffer.timestamp.tv_usec / 1000ULL;
    frame->sequence = buffer.sequence;
    frame->buffer_index = buffer.index;
    return 0;
}

/*
    @brief 归还摄像头帧
    @param camera 摄像头结构体指针
    @param frame 摄像头帧结构体指针
    @return 0 成功，-1 失败
*/
int camera_release_frame(Camera *camera, CameraFrame *frame)
{
    struct v4l2_buffer buffer;
    unsigned int index;

    if (camera == NULL || frame == NULL || camera->fd < 0 ||
        !camera->streaming || frame->data == NULL ||
        frame->buffer_index >= camera->buffer_count) {
        LOG_ERROR("%s", "归还帧参数无效");
        return -1;
    }
    index = frame->buffer_index;
    if (!camera->buffers[index].in_use ||
        frame->data != camera->buffers[index].start) {
        LOG_ERROR("%s", "帧不属于当前摄像头或已经归还");
        return -1;
    }

    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (xioctl(camera->fd, VIDIOC_QBUF, &buffer) < 0) {
        LOG_ERROR("归还缓冲区[%u]失败: %s", index, strerror(errno));
        return -1;
    }
    camera->buffers[index].in_use = 0;
    clear_frame(frame);
    return 0;
}

/*
    @brief 停止摄像头采集
    @param camera 摄像头结构体指针
    @return 0 成功，-1 失败
*/
int camera_stop(Camera *camera)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (camera == NULL || camera->fd < 0)
        return -1;
    if (!camera->streaming)
        return 0;
    if (xioctl(camera->fd, VIDIOC_STREAMOFF, &type) < 0) {
        LOG_ERROR("停止摄像头采集失败: %s", strerror(errno));
        return -1;
    }

    camera->streaming = 0;
    for (unsigned int i = 0; i < camera->buffer_count; ++i)
        camera->buffers[i].in_use = 0;
    return 0;
}

/*
    @brief 关闭摄像头
    @param camera 摄像头结构体指针
*/
void camera_close(Camera *camera)
{
    if (camera == NULL)
        return;
    if (camera->fd >= 0 && camera->streaming)
        (void)camera_stop(camera);

    if (camera->buffers != NULL) {
        for (unsigned int i = 0; i < camera->buffer_count; ++i) {
            if (camera->buffers[i].start != NULL &&
                camera->buffers[i].length != 0 &&
                munmap(camera->buffers[i].start,
                       camera->buffers[i].length) < 0) {
                LOG_ERROR("解除缓冲区[%u]映射失败: %s", i, strerror(errno));
            }
        }
        free(camera->buffers);
    }
    if (camera->fd >= 0 && close(camera->fd) < 0)
        LOG_ERROR("关闭摄像头失败: %s", strerror(errno));

    memset(camera, 0, sizeof(*camera));
    camera->fd = -1;
}

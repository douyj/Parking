#define _POSIX_C_SOURCE 200809L

#include "pwm_sysfs.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PWM_EXPORT_WAIT_ATTEMPTS 50
#define PWM_EXPORT_WAIT_INTERVAL_NS 10000000L

/*
    @brief 设置 PWM 错误信息
    @param pwm PWM 设备指针
    @param format 错误信息格式
    @param ... 格式化参数
*/
static void set_error(pwm_sysfs_t *pwm, const char *format, ...)
{
    va_list arguments;

    if (pwm == NULL)
        return;

    va_start(arguments, format);
    vsnprintf(pwm->error, sizeof(pwm->error), format, arguments);
    va_end(arguments);
}

/*
    @brief 清除 PWM 错误信息
    @param pwm PWM 设备指针
*/
static void clear_error(pwm_sysfs_t *pwm)
{
    if (pwm != NULL)
        pwm->error[0] = '\0';
}

/*
    @brief 构建 PWM sysfs 路径
    @param pwm PWM 设备指针
    @param destination 目标缓冲区
    @param destination_size 目标缓冲区大小
    @param format 路径格式
    @param ... 格式化参数
    @return 0 成功，-1 失败
*/
static int make_path(pwm_sysfs_t *pwm, char *destination,
                     size_t destination_size, const char *format, ...)
{
    va_list arguments;
    int length;

    va_start(arguments, format);
    length = vsnprintf(destination, destination_size, format, arguments);
    va_end(arguments);

    if (length < 0 || (size_t)length >= destination_size) {
        set_error(pwm, "PWM sysfs 路径过长");
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

/*
    @brief 写入 PWM sysfs 文件
    @param pwm PWM 设备指针
    @param path 文件路径
    @param text 要写入的文本
    @return 0 成功，-1 失败
*/
static int write_text(pwm_sysfs_t *pwm, const char *path, const char *text)
{
    int descriptor;
    int saved_errno;
    size_t length;
    ssize_t written;

    do {
        descriptor = open(path, O_WRONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);

    if (descriptor < 0) {
        saved_errno = errno;
        set_error(pwm, "打开 %s 失败: %s", path, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    length = strlen(text);
    do {
        written = write(descriptor, text, length);
    } while (written < 0 && errno == EINTR);

    if (written < 0 || (size_t)written != length) {
        saved_errno = written < 0 ? errno : EIO;
        set_error(pwm, "写入 %s 失败: %s", path, strerror(saved_errno));
        close(descriptor);
        errno = saved_errno;
        return -1;
    }

    if (close(descriptor) != 0) {
        saved_errno = errno;
        set_error(pwm, "关闭 %s 失败: %s", path, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    return 0;
}

/*
    @brief 写入 PWM sysfs 文件
    @param pwm PWM 设备指针
    @param path 文件路径
    @param value 要写入的 64 位无符号整数
    @return 0 成功，-1 失败
*/
static int write_u64(pwm_sysfs_t *pwm, const char *path, uint64_t value)
{
    char text[32];
    int length;

    length = snprintf(text, sizeof(text), "%" PRIu64, value);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        set_error(pwm, "格式化 PWM 数值失败");
        errno = EOVERFLOW;
        return -1;
    }

    return write_text(pwm, path, text);
}

/*
    @brief 写入 PWM sysfs 文件
    @param pwm PWM 设备指针
    @param path 文件路径
    @param channel 要写入的通道
    @return 0 成功，-1 失败
*/
static int write_channel(pwm_sysfs_t *pwm, const char *path,
                         unsigned int channel)
{
    char text[32];
    int length;

    length = snprintf(text, sizeof(text), "%u", channel);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        set_error(pwm, "格式化 PWM 通道失败");
        errno = EOVERFLOW;
        return -1;
    }

    return write_text(pwm, path, text);
}

/*
    @brief 读取 PWM sysfs 文件
    @param pwm PWM 设备指针
    @param count 通道数量指针
    @return 0 成功，-1 失败
*/
static int read_channel_count(pwm_sysfs_t *pwm, unsigned int *count)
{
    char path[PWM_SYSFS_PATH_CAPACITY];
    char text[64];
    char *end;
    unsigned long value;
    int descriptor;
    int saved_errno;
    ssize_t length;

    if (make_path(pwm, path, sizeof(path), "%s/npwm",
                  pwm->pwmchip_path) != 0)
        return -1;

    do {
        descriptor = open(path, O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);

    if (descriptor < 0) {
        saved_errno = errno;
        set_error(pwm, "打开 %s 失败: %s", path, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    do {
        length = read(descriptor, text, sizeof(text) - 1U);
    } while (length < 0 && errno == EINTR);

    if (length <= 0) {
        saved_errno = length < 0 ? errno : EIO;
        set_error(pwm, "读取 %s 失败: %s", path, strerror(saved_errno));
        close(descriptor);
        errno = saved_errno;
        return -1;
    }

    text[length] = '\0';
    if (close(descriptor) != 0) {
        saved_errno = errno;
        set_error(pwm, "关闭 %s 失败: %s", path, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || value > UINT_MAX) {
        set_error(pwm, "%s 中的通道数量无效", path);
        errno = EINVAL;
        return -1;
    }

    *count = (unsigned int)value;
    return 0;
}

/*
    @brief 检查路径是否为目录
    @param path 路径
    @return 0 成功，-1 失败
*/
static int path_is_directory(const char *path)
{
    struct stat status;

    if (stat(path, &status) != 0)
        return 0;

    return S_ISDIR(status.st_mode);
}

/*
    @brief 等待 PWM 通道目录存在
    @param pwm PWM 设备指针
    @return 0 成功，-1 失败
*/
static int wait_for_pwm_path(pwm_sysfs_t *pwm)
{
    const struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = PWM_EXPORT_WAIT_INTERVAL_NS,
    };
    int attempt;

    for (attempt = 0; attempt < PWM_EXPORT_WAIT_ATTEMPTS; ++attempt) {
        if (path_is_directory(pwm->pwm_path))
            return 0;

        nanosleep(&interval, NULL);
    }

    set_error(pwm, "等待 PWM 通道目录 %s 超时", pwm->pwm_path);
    errno = ETIMEDOUT;
    return -1;
}


/*
===================================================================================
*/


/*
    @brief 打开 PWM 通道
    @param pwm PWM 设备指针
    @param pwmchip_path pwmchip 路径
    @param channel 要打开的通道
    @return 0 成功，-1 失败
*/
int pwm_sysfs_open(pwm_sysfs_t *pwm, const char *pwmchip_path,
                   unsigned int channel)
{
    char export_path[PWM_SYSFS_PATH_CAPACITY];      // export 文件路径
    char unexport_path[PWM_SYSFS_PATH_CAPACITY];    // unexport 文件路径
    char saved_error[PWM_SYSFS_ERROR_CAPACITY];      // 保存的错误信息
    unsigned int channel_count;                     // 通道数量
    int export_result;                              // export 操作结果
    int export_errno;                               // export 操作错误码
    int length;                                     // 字符串长度

    if (pwm == NULL || pwmchip_path == NULL || pwmchip_path[0] == '\0') {
        if (pwm != NULL)
            set_error(pwm, "PWM 参数无效");
        errno = EINVAL;
        return -1;
    }

    memset(pwm, 0, sizeof(*pwm));   // 初始化 PWM 设备结构体

    length = snprintf(pwm->pwmchip_path, sizeof(pwm->pwmchip_path), "%s", pwmchip_path);  
    if (length < 0 || (size_t)length >= sizeof(pwm->pwmchip_path)) {
        set_error(pwm, "pwmchip 路径过长");
        errno = ENAMETOOLONG;
        return -1;
    }

    // 检查 pwmchip 目录是否存在
    if (!path_is_directory(pwm->pwmchip_path)) {
        set_error(pwm, "pwmchip 目录不存在: %s", pwm->pwmchip_path);
        errno = ENOENT;
        return -1;
    }

    // 检查通道是否超出范围
    pwm->channel = channel;
    if (read_channel_count(pwm, &channel_count) != 0)
        return -1;

    if (channel >= channel_count) {
        set_error(pwm, "PWM 通道 %u 超出范围，%s 仅有 %u 个通道", channel,
                  pwm->pwmchip_path, channel_count);
        errno = EINVAL;
        return -1;
    }

    // 构建 PWM 通道路径
    if (make_path(pwm, pwm->pwm_path, sizeof(pwm->pwm_path), "%s/pwm%u", pwm->pwmchip_path, channel) != 0 ||
        make_path(pwm, export_path, sizeof(export_path), "%s/export", pwm->pwmchip_path) != 0 ||
        make_path(pwm, unexport_path, sizeof(unexport_path), "%s/unexport",pwm->pwmchip_path) != 0)
        return -1;

    if (!path_is_directory(pwm->pwm_path)) {
        export_result = write_channel(pwm, export_path, channel);
        export_errno = errno;
        if (export_result != 0 && export_errno != EBUSY)
            return -1;

        pwm->exported_by_us = export_result == 0;
        if (wait_for_pwm_path(pwm) != 0) {
            if (pwm->exported_by_us) {
                snprintf(saved_error, sizeof(saved_error), "%s", pwm->error);
                write_channel(pwm, unexport_path, channel);
                snprintf(pwm->error, sizeof(pwm->error), "%s", saved_error);
            }
            pwm->exported_by_us = 0;
            return -1;
        }
    }

    pwm->initialized = 1;
    clear_error(pwm);
    return 0;
}

/*
    @brief 配置 PWM 通道
    @param pwm PWM 设备指针
    @param period_ns 周期时间（纳秒）
    @param initial_duty_ns 初始占空时间（纳秒）
    @return 0 成功，-1 失败
*/
int pwm_sysfs_configure(pwm_sysfs_t *pwm, uint64_t period_ns,
                        uint64_t initial_duty_ns)
{
    char polarity_path[PWM_SYSFS_PATH_CAPACITY];
    char period_path[PWM_SYSFS_PATH_CAPACITY];
    char duty_path[PWM_SYSFS_PATH_CAPACITY];

    if (pwm == NULL || !pwm->initialized || period_ns == 0 ||
        initial_duty_ns > period_ns) {
        if (pwm != NULL)
            set_error(pwm, "PWM 配置参数无效");
        errno = EINVAL;
        return -1;
    }

    clear_error(pwm);
    if (make_path(pwm, polarity_path, sizeof(polarity_path),
                "%s/polarity", pwm->pwm_path) != 0 ||
        make_path(pwm, period_path, sizeof(period_path),
                "%s/period", pwm->pwm_path) != 0 ||
        make_path(pwm, duty_path, sizeof(duty_path),
                "%s/duty_cycle", pwm->pwm_path) != 0)
        return -1;

        /*
        * RK3576 刚 export PWM 时 period 可能为 0。
        * 此时不能先写 enable=0，否则驱动可能返回 EINVAL。
        *
        * 所以先建立合法 period，再设置 duty 和 polarity。
        */
        if (write_u64(pwm, period_path, period_ns) != 0 ||
            write_u64(pwm, duty_path, initial_duty_ns) != 0 ||
            write_text(pwm, polarity_path, "normal") != 0) {

            pwm->enabled = 0;
            pwm->configured = 0;
            return -1;
        }

    pwm->period_ns = period_ns;
    pwm->duty_cycle_ns = initial_duty_ns;
    pwm->enabled = 0;
    pwm->configured = 1;
    return 0;
}

/*
    @brief 设置 PWM 通道占空时间
    @param pwm PWM 设备指针
    @param duty_ns 占空时间（纳秒）
    @return 0 成功，-1 失败
*/
int pwm_sysfs_set_duty_cycle(pwm_sysfs_t *pwm, uint64_t duty_ns)
{
    char duty_path[PWM_SYSFS_PATH_CAPACITY];

    if (pwm == NULL || !pwm->initialized || !pwm->configured ||
        duty_ns > pwm->period_ns) {
        if (pwm != NULL)
            set_error(pwm, "PWM 占空时间无效");
        errno = EINVAL;
        return -1;
    }

    clear_error(pwm);
    if (make_path(pwm, duty_path, sizeof(duty_path), "%s/duty_cycle",
                  pwm->pwm_path) != 0 ||
        write_u64(pwm, duty_path, duty_ns) != 0)
        return -1;

    pwm->duty_cycle_ns = duty_ns;
    return 0;
}

/*
    @brief 使能 PWM 通道
    @param pwm PWM 设备指针
    @return 0 成功，-1 失败
*/
int pwm_sysfs_enable(pwm_sysfs_t *pwm)
{
    char enable_path[PWM_SYSFS_PATH_CAPACITY];

    if (pwm == NULL || !pwm->initialized || !pwm->configured) {
        if (pwm != NULL)
            set_error(pwm, "PWM 尚未完成配置");
        errno = EINVAL;
        return -1;
    }

    if (pwm->enabled)
        return 0;

    clear_error(pwm);
    if (make_path(pwm, enable_path, sizeof(enable_path), "%s/enable", pwm->pwm_path) != 0 || write_text(pwm, enable_path, "1") != 0)
        return -1;

    pwm->enabled = 1;
    return 0;
}

/*
    @brief 禁用 PWM 通道
    @param pwm PWM 设备指针
    @return 0 成功，-1 失败
*/
int pwm_sysfs_disable(pwm_sysfs_t *pwm)
{
    char enable_path[PWM_SYSFS_PATH_CAPACITY];

    if (pwm == NULL || !pwm->initialized) {
        if (pwm != NULL)
            set_error(pwm, "PWM 尚未打开");
        errno = EINVAL;
        return -1;
    }

    if (!pwm->enabled)
        return 0;

    clear_error(pwm);
    if (make_path(pwm, enable_path, sizeof(enable_path), "%s/enable",
                  pwm->pwm_path) != 0 ||
        write_text(pwm, enable_path, "0") != 0)
        return -1;

    pwm->enabled = 0;
    return 0;
}

/*
    @brief 获取 PWM 通道最后错误信息
    @param pwm PWM 设备指针
    @return 错误信息字符串指针
    @note 调用者不得释放返回的字符串，由 pwm_sysfs_t 持有。
*/
const char *pwm_sysfs_last_error(const pwm_sysfs_t *pwm)
{
    return pwm != NULL ? pwm->error : "pwm_sysfs is null";
}

/*
    @brief 关闭 PWM 通道
    @param pwm PWM 设备指针
*/
void pwm_sysfs_close(pwm_sysfs_t *pwm)
{
    char unexport_path[PWM_SYSFS_PATH_CAPACITY];

    if (pwm == NULL)
        return;

    if (pwm->initialized && pwm->enabled)
        pwm_sysfs_disable(pwm);

    if (pwm->initialized && pwm->exported_by_us &&
        make_path(pwm, unexport_path, sizeof(unexport_path), "%s/unexport",
                  pwm->pwmchip_path) == 0)
        write_channel(pwm, unexport_path, pwm->channel);

    memset(pwm, 0, sizeof(*pwm));
}

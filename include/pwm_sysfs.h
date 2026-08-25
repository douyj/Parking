#ifndef PWM_SYSFS_H
#define PWM_SYSFS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWM_SYSFS_PATH_CAPACITY 4096
#define PWM_SYSFS_ERROR_CAPACITY 256

/*
 * Linux PWM sysfs 设备。
 * 该结构只供 gate 模块持有，业务代码不应直接修改其中字段。
 */
typedef struct pwm_sysfs {
    char pwmchip_path[PWM_SYSFS_PATH_CAPACITY];
    char pwm_path[PWM_SYSFS_PATH_CAPACITY];
    unsigned int channel;
    uint64_t period_ns;
    uint64_t duty_cycle_ns;
    int exported_by_us;
    int initialized;
    int configured;
    int enabled;
    char error[PWM_SYSFS_ERROR_CAPACITY];
} pwm_sysfs_t;

/* 打开 pwmchip，并在需要时导出指定通道。 */
int pwm_sysfs_open(pwm_sysfs_t *pwm, const char *pwmchip_path,
                   unsigned int channel);

/* 设置 normal 极性、周期和初始占空时间；配置完成后 PWM 保持禁用。 */
int pwm_sysfs_configure(pwm_sysfs_t *pwm, uint64_t period_ns,
                        uint64_t initial_duty_ns);

/* 修改有效电平持续时间，必须满足 duty_ns <= period_ns。 */
int pwm_sysfs_set_duty_cycle(pwm_sysfs_t *pwm, uint64_t duty_ns);

int pwm_sysfs_enable(pwm_sysfs_t *pwm);
int pwm_sysfs_disable(pwm_sysfs_t *pwm);

/* 返回的字符串由 pwm_sysfs_t 持有，调用者不得释放。 */
const char *pwm_sysfs_last_error(const pwm_sysfs_t *pwm);

/* 禁用输出；仅当通道由本实例导出时才执行 unexport。 */
void pwm_sysfs_close(pwm_sysfs_t *pwm);

#ifdef __cplusplus
}
#endif

#endif

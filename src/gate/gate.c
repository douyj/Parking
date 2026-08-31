#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gate.h"
#include "log.h"
#include "pwm_sysfs.h"

#define GATE_ERROR_CAPACITY 256

struct gate{
    pwm_sysfs_t pwm;

    uint64_t open_pulse_ns;     //开闸时的 PWM 高电平持续时间，单位纳秒
    uint64_t close_pulse_ns;    //关闸时的 PWM 高电平持续时间，单位纳秒
    unsigned int movement_time_ms;      //软件认为完成一次开关动作所需的时间
    int hold_after_move;          //是否在移动完成后保持状态，0：不保持，1：保持
 
    gate_state_t state;     //当前状态
    uint64_t movement_start_ms;     //本次动作开始时间

    char error[GATE_ERROR_CAPACITY];     //错误信息
};

/*
    @brief 获取当前时间（毫秒）
    @param value 时间指针
    @return 0 成功 -1 失败
*/
static int monotonic_ms(uint64_t *value)
{
    struct timespec time;
    if(value == NULL){
        return -1;
    }

    if(clock_gettime(CLOCK_MONOTONIC, &time) != 0){
        return -1;
    }

    *value = (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U; 
    return 0;
}

/* 错误记录 */
static void set_error(gate_t *gate, const char *format, ...)
{
    va_list arguments;

    if(gate == NULL) return;

    va_start(arguments, format);
    vsnprintf(gate->error, sizeof(gate->error), format, arguments);
    va_end(arguments);
}

/* 参数检查 */
static int validate_config(const gate_config_t *config)
{
    if(config == NULL || config->pwmchip_path == NULL) return GATE_ERROR_INVALID_ARGUMENT;

    if (config->period_ns == 0 ||
        config->open_pulse_ns > config->period_ns ||
        config->close_pulse_ns > config->period_ns ||
        config->open_pulse_ns == config->close_pulse_ns ||
        config->movement_time_ms == 0)
        return GATE_ERROR_INVALID_ARGUMENT;

    if (config->hold_after_move != 0 &&
        config->hold_after_move != 1)
        return GATE_ERROR_INVALID_ARGUMENT;

    return GATE_OK;
}

/* 统一的动作函数 */
//开闸和关闸的区别只有脉宽和目标状态，因此可以共用一个内部函数
static int start_motion(gate_t *gate, uint64_t pulse_ns, gate_state_t moving_state)
{
    uint64_t now;

    if(gate == NULL) return GATE_ERROR_INVALID_ARGUMENT;
    
    if(gate->state == GATE_STATE_ERROR) return GATE_ERROR_INVALID_STATE;

    if(pwm_sysfs_set_duty_cycle(&gate->pwm, pulse_ns) != 0){
        set_error(gate, "设置 PWM 脉宽失败：%s", pwm_sysfs_last_error(&gate->pwm));
        gate->state = GATE_STATE_ERROR;
        return GATE_ERROR_IO;
    }

    if(pwm_sysfs_enable(&gate->pwm) != 0){
        set_error(gate, "使能 PWM 失败：%s", pwm_sysfs_last_error(&gate->pwm));
        gate->state = GATE_STATE_ERROR;
        return GATE_ERROR_IO;
    }

    if(monotonic_ms(&now) != 0){
        set_error(gate, "获取当前时间失败");
        pwm_sysfs_disable(&gate->pwm);
        gate->state = GATE_STATE_ERROR;
        return GATE_ERROR_IO;
    }

    gate->movement_start_ms = now;
    gate->state = moving_state;
    return GATE_OK;
}

/*
    @brief 创建闸门
    @param config 闸门配置
    @return 闸门指针
*/
gate_t *gate_create(const gate_config_t *config)
{
    gate_t *gate = NULL;

    if(validate_config(config) != GATE_OK) return NULL;

    gate = calloc(1, sizeof(*gate));
    if(gate == NULL) return NULL;

    gate->open_pulse_ns = config->open_pulse_ns;
    gate->close_pulse_ns = config->close_pulse_ns;
    gate->movement_time_ms = config->movement_time_ms;
    gate->hold_after_move = config->hold_after_move;

    if(pwm_sysfs_open(&gate->pwm, config->pwmchip_path, config->channel) != 0){
        free(gate);
        return NULL;
    }

    if(pwm_sysfs_configure(&gate->pwm, config->period_ns, config->close_pulse_ns) != 0){
        pwm_sysfs_close(&gate->pwm);
        free(gate);
        return NULL;
    }

    //创建成功后主动将闸门置于关闭位置
    gate->state = GATE_STATE_OPEN;
    if(gate_close(gate) != GATE_OK){
        pwm_sysfs_close(&gate->pwm);
        free(gate);
        return NULL;
    }

    return gate;
}

/*
    @brief 开闸
    @param gate 闸门指针
    @return 0 成功 -1 失败
*/
int gate_open(gate_t *gate)
{
    if(gate == NULL) return GATE_ERROR_INVALID_ARGUMENT;

    if(gate->state == GATE_STATE_OPEN || 
        gate->state == GATE_STATE_OPENING){
            return GATE_ERROR_INVALID_STATE;
    }

    return start_motion(gate, gate->open_pulse_ns, GATE_STATE_OPENING);

}

/*
    @brief 关闸
    @param gate 闸门指针
    @return 0 成功 -1 失败
*/
int gate_close(gate_t *gate)
{
    if (gate == NULL)
        return GATE_ERROR_INVALID_ARGUMENT;

    /* 重复命令直接视为成功 */
    if (gate->state == GATE_STATE_CLOSED ||
        gate->state == GATE_STATE_CLOSING)
        return GATE_OK;

    return start_motion(gate, gate->close_pulse_ns, GATE_STATE_CLOSING);    
}


/*
    @brief 判断动作是否完成
    @param gate 闸门指针
    @return 0 成功 -1 失败
*/
int gate_update(gate_t *gate)
{
    uint64_t now;
    gate_state_t completed_state;

    if(gate == NULL ) return GATE_ERROR_INVALID_ARGUMENT;

    if(gate->state != GATE_STATE_OPENING && gate->state != GATE_STATE_CLOSING) return GATE_OK;

    if(monotonic_ms(&now) != 0){
        set_error(gate, "获取当前时间失败");
        gate->state = GATE_STATE_ERROR;
        return GATE_ERROR_IO;
    }

    if(now - gate->movement_start_ms < gate->movement_time_ms) return GATE_OK;
    
    completed_state = gate->state == GATE_STATE_OPENING ? GATE_STATE_OPEN : GATE_STATE_CLOSED;

    if(!gate->hold_after_move){
        if(pwm_sysfs_disable(&gate->pwm) != 0){
            set_error(gate, "停止 PWM 失败：%s", pwm_sysfs_last_error(&gate->pwm));
            gate->state = GATE_STATE_ERROR;
            return GATE_ERROR_IO;
        }
    }

    gate->state = completed_state;
    return GATE_OK;
}

/*
    @brief 获取闸门状态
    @param gate 闸门指针
    @return 闸门状态
*/
gate_state_t gate_get_state(const gate_t *gate)
{
    if (gate == NULL)
        return GATE_STATE_ERROR;

    return gate->state;
}

/*
    @brief 获取闸门最后错误信息
    @param gate 闸门指针
    @return 错后错误信息
*/
const char *gate_last_error(const gate_t *gate)
{
    return gate != NULL ? gate->error : "gate is null";
}

/*
    @brief 销毁闸门
    @param gate 闸门指针
*/
void gate_destroy(gate_t *gate)
{
    if (gate == NULL)
        return;

    pwm_sysfs_disable(&gate->pwm);
    pwm_sysfs_close(&gate->pwm);
    free(gate);
}

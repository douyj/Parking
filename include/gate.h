#ifndef GATE_H
#define GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gate gate_t;

typedef enum {
    GATE_STATE_CLOSED = 0,
    GATE_STATE_OPENING,
    GATE_STATE_OPEN,
    GATE_STATE_CLOSING,
    GATE_STATE_ERROR
} gate_state_t;

typedef enum {
    GATE_OK = 0,
    GATE_ERROR_INVALID_ARGUMENT = -1,
    GATE_ERROR_IO = -2,
    GATE_ERROR_INVALID_STATE = -3,
    GATE_ERROR_TIMEOUT = -4
} gate_error_t;

typedef struct {
    const char *pwmchip_path;   // PWM 设备路径
    unsigned int channel;       // PWM 通道

    uint64_t period_ns;         // PWM 周期，单位纳秒
    uint64_t open_pulse_ns;     // 开闸脉冲宽度，单位纳秒
    uint64_t close_pulse_ns;    // 关闸脉冲宽度，单位纳秒

    unsigned int movement_time_ms; // 闸门移动时间，单位毫秒
    int hold_after_move;          // 是否在移动完成后保持状态
} gate_config_t;

/* 创建成功后主动将闸门置于关闭位置 */
gate_t *gate_create(const gate_config_t *config);

/* 非阻塞地开始开闸 */
int gate_open(gate_t *gate);

/* 非阻塞地开始关闸 */
int gate_close(gate_t *gate);

/* 更新 OPENING/CLOSING 状态 */
int gate_update(gate_t *gate);

/* 查询当前状态 */
gate_state_t gate_get_state(const gate_t *gate);

/* 返回的字符串只读，不得释放 */
const char *gate_last_error(const gate_t *gate);

/* 停止 PWM 并释放资源 */
void gate_destroy(gate_t *gate);

#ifdef __cplusplus
}
#endif

#endif
#ifndef PARKING_PLATE_POLICY_H
#define PARKING_PLATE_POLICY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARKING_PLATE_MIN_CHARACTERS 7U
#define PARKING_PLATE_MAX_CHARACTERS 8U
#define PARKING_PLATE_AMBIGUOUS_DISTANCE 1U

typedef enum {
    PARKING_PLATE_ACCEPT = 0,
    PARKING_PLATE_IGNORE_DUPLICATE,
    PARKING_PLATE_REJECT_INVALID,
    PARKING_PLATE_REJECT_AMBIGUOUS,
    PARKING_PLATE_REJECT_BUSY
} parking_plate_decision_t;

typedef struct {
    parking_plate_decision_t decision;
    size_t character_count;
    size_t edit_distance;
} parking_plate_policy_result_t;

/*
 * 对稳定识别事件执行停车业务入口策略：
 *
 * - 只接受模型字符集能够表示的 7/8 字符中国车牌；
 * - IDLE 时允许合法车牌进入状态机；
 * - 忙碌时与当前车牌完全相同的事件作为重复事件忽略；
 * - 忙碌时 UTF-8 字符编辑距离为 1 的事件标记为歧义，不自动合并；
 * - 其他合法车牌按车道忙碌处理。
 *
 * confidence 是检测框置信度，只检查它是否为 [0, 1] 内的有限值；
 * 低置信度阈值仍由 plate_pipeline 统一控制。
 */
int parking_plate_policy_evaluate(
    const char *plate_number,
    float confidence,
    const char *active_plate,
    int lane_idle,
    parking_plate_policy_result_t *result);

const char *parking_plate_decision_name(parking_plate_decision_t decision);

#ifdef __cplusplus
}
#endif

#endif

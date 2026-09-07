#ifndef PLATE_CONFIRMATION_H
#define PLATE_CONFIRMATION_H

#include "plate_recognizer.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 车牌确认配置结构体
typedef struct {
    unsigned int required_frames;       // 确认车牌需要的连续帧数
    unsigned int max_missed_frames;     // 允许的最大连续未识别帧数
    uint64_t same_plate_cooldown_us;    // 相同车牌的冷却时间（微秒）
} plate_confirmation_config_t;


// 车牌确认器结构体
typedef struct {
    plate_confirmation_config_t config;

    char candidate_plate[PLATE_TEXT_CAPACITY];  // 候选车牌
    unsigned int consecutive_frames;            // 当前候选车牌已经被识别多少次
    unsigned int missed_frames;                 // 当前候选车牌已经有多少帧没有看到了
    int candidate_confirmed;                    // 当前候选车牌是否已经被确认

    char last_confirmed_plate[PLATE_TEXT_CAPACITY];  // 上一个确认的车牌
    uint64_t last_confirmed_us;                      // 上一个确认的时间戳（微秒）
} plate_confirmation_t;


int plate_confirmation_init(plate_confirmation_t *confirmation, const plate_confirmation_config_t *config);

int plate_confirmation_update(plate_confirmation_t *confirmation, const plate_result_t *result, uint64_t now_us);

void plate_confirmation_reset(plate_confirmation_t *confirmation);

#ifdef __cplusplus
}
#endif

#endif

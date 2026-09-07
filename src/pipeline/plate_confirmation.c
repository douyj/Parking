#include "plate_confirmation.h"

#include <string.h>

// 复制车牌号，确保字符串以空字符结尾，避免缓冲区溢出
static void copy_plate_number(char destination[PLATE_TEXT_CAPACITY], const char *source)
{
    strncpy(destination, source, PLATE_TEXT_CAPACITY - 1);
    destination[PLATE_TEXT_CAPACITY - 1] = '\0';   // 确保字符串以空字符结尾，避免缓冲区溢出
}

// 清除候选车牌信息
static void clear_candidate(plate_confirmation_t *confirmation)
{
    confirmation->candidate_plate[0] = '\0';
    confirmation->consecutive_frames = 0;
    confirmation->missed_frames = 0;
    confirmation->candidate_confirmed = 0;
}

// 初始化车牌确认器
int plate_confirmation_init(plate_confirmation_t *confirmation, const plate_confirmation_config_t *config)
{
    if(confirmation == NULL || config == NULL)
        return -1;

    if(config->required_frames == 0 || config->max_missed_frames ==0)
        return -1;

    memset(confirmation, 0, sizeof(plate_confirmation_t));
    confirmation->config = *config;

    return 0;
}


// 更新车牌确认器
int plate_confirmation_update(plate_confirmation_t *confirmation, const plate_result_t *result, uint64_t now_us)
{
    if (confirmation == NULL)
        return -1;

    if (confirmation->config.required_frames == 0 ||
        confirmation->config.max_missed_frames == 0)
        return -1;

    // 当前帧没识别到车牌，增加漏检帧数
    if (result == NULL || result->plate_number[0] == '\0') {
        if (confirmation->missed_frames < confirmation->config.max_missed_frames) {
            ++confirmation->missed_frames;
        }

        // 达到最大漏检数，清除候选
        if (confirmation->missed_frames >= confirmation->config.max_missed_frames) {
            clear_candidate(confirmation);
        }

        return 0;
    }

    // 当前帧成功识别到有效车牌，连续漏检帧数清零
    confirmation->missed_frames = 0;

    // 当前帧有有效车牌
    if (strcmp(confirmation->candidate_plate, result->plate_number) == 0) {

        // 如果还是同一个车牌，增加连续帧数
        if (confirmation->consecutive_frames < confirmation->config.required_frames) {
            ++confirmation->consecutive_frames;
        }
    } else {
        // 如果当前识别的是另一块车牌，重置候选车牌信息
        copy_plate_number(confirmation->candidate_plate, result->plate_number);

        confirmation->consecutive_frames = 1;

        // 新的候选车牌还没有被确认
        confirmation->candidate_confirmed = 0;
    }

    // 还没达到稳定帧数
    if (confirmation->consecutive_frames < confirmation->config.required_frames) {
        return 0;
    }

    // 已经确认过这个候选了，直接返回
    if (confirmation->candidate_confirmed)
        return 0;

    // 冷却时间判断，如果上一个确认的是同一个车牌，且冷却时间未到，直接返回
    if (strcmp(confirmation->last_confirmed_plate, result->plate_number) == 0 &&
        now_us - confirmation->last_confirmed_us < confirmation->config.same_plate_cooldown_us) {
        return 0;
    }

    // 真正确认成功，更新上一个确认的车牌和时间戳，设置确认状态为已确认
    copy_plate_number(confirmation->last_confirmed_plate, result->plate_number);

    confirmation->last_confirmed_us = now_us;
    confirmation->candidate_confirmed = 1;

    return 1;
}

// 把确认器运行状态全部清空，但保留原来的配置
void plate_confirmation_reset(plate_confirmation_t *confirmation)
{
    plate_confirmation_config_t config;

    if(confirmation == NULL)
        return;

    config = confirmation->config;
    memset(confirmation, 0, sizeof(*confirmation));
    confirmation->config = config;
}
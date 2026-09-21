#ifndef PARKING_FEE_H
#define PARKING_FEE_H

#include <stdint.h>

typedef struct parking_fee parking_fee_t;

typedef struct {
    uint32_t free_min;       /* 免费分钟数 */
    uint32_t unit_min;       /* 每个计费周期的分钟数 */
    uint32_t unit_cents;     /* 每周期收费，单位：分 */
    uint32_t max_cents;      /* 单次费用上限，单位：分 */
} parking_fee_policy_t;

typedef enum {
    PARKING_FEE_OK = 0,
    PARKING_FEE_BAD_ARG = -1,
    PARKING_FEE_BAD_POLICY = -2,
    PARKING_FEE_BAD_TIME = -3,
    PARKING_FEE_OVERFLOW = -4
} parking_fee_status_t;

/* 创建时自动使用默认规则。 */
parking_fee_t *parking_fee_create(void);

void parking_fee_destroy(parking_fee_t *fee);

/*
 * 设置收费规则。
 * 成功返回 0，参数或规则无效返回 -1。
 * 模块会复制 policy 内容，不保存调用者指针。
 */
int parking_fee_set_policy(
    parking_fee_t *fee,
    const parking_fee_policy_t *policy);

/*
 * 获取当前收费规则。
 * 返回的是规则副本，调用者可以自行修改。
 */
int parking_fee_get_policy(const parking_fee_t *fee, parking_fee_policy_t *policy);

/* 使用当前规则计算费用。 */
parking_fee_status_t parking_fee_calc(
    const parking_fee_t *fee,
    uint64_t entry_us,
    uint64_t exit_us,
    uint64_t *fee_cents);

const char *parking_fee_status_name(parking_fee_status_t status);

#endif
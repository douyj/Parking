#include "parking_fee.h"

#include <limits.h>
#include <stdlib.h>

#define US_PER_MINUTE 60000000ULL

#define DEFAULT_FREE_MIN 30U
#define DEFAULT_UNIT_MIN 10U
#define DEFAULT_UNIT_CENTS 50U
#define DEFAULT_MAX_CENTS 2000U

struct parking_fee {
    parking_fee_policy_t policy;
};

// 转换分钟数为微秒数
static int minutes_to_us(uint32_t minutes, uint64_t *value)
{
    /* uint32_t 的最大分钟数乘 60000000 仍在 uint64_t 范围内。 */
    if (value == NULL)
        return -1;

    *value = (uint64_t)minutes * US_PER_MINUTE;
    return 0;
}

// 验证收费规则是否有效
static int policy_valid(const parking_fee_policy_t *policy)
{
    uint64_t unused;

    if (policy == NULL || policy->unit_min == 0U ||
        policy->unit_cents == 0U || policy->max_cents == 0U ||
        policy->max_cents < policy->unit_cents)
        return 0;

    if (minutes_to_us(policy->free_min, &unused) != 0 ||
        minutes_to_us(policy->unit_min, &unused) != 0)
        return 0;

    return 1;
}

// 创建时自动使用默认规则
parking_fee_t *parking_fee_create(void)
{
    parking_fee_t *fee;

    fee = calloc(1, sizeof(*fee));
    if (fee == NULL)
        return NULL;

    fee->policy.free_min = DEFAULT_FREE_MIN;
    fee->policy.unit_min = DEFAULT_UNIT_MIN;
    fee->policy.unit_cents = DEFAULT_UNIT_CENTS;
    fee->policy.max_cents = DEFAULT_MAX_CENTS;
    return fee;
}

// 销毁收费模块
void parking_fee_destroy(parking_fee_t *fee)
{
    free(fee);
}

// 设置收费规则
int parking_fee_set_policy(parking_fee_t *fee, const parking_fee_policy_t *policy)
{
    if (fee == NULL || !policy_valid(policy))
        return -1;

    fee->policy = *policy;
    return 0;
}

// 获取当前收费规则
int parking_fee_get_policy(const parking_fee_t *fee, parking_fee_policy_t *policy)
{
    if (fee == NULL || policy == NULL)
        return -1;

    *policy = fee->policy;
    return 0;
}

// 使用当前规则计算费用
parking_fee_status_t parking_fee_calc(const parking_fee_t *fee, uint64_t entry_us, uint64_t exit_us, uint64_t *fee_cents)
{
    uint64_t free_us;       // 免费时间，单位：微秒
    uint64_t unit_us;       // 每个计费周期的微秒数
    uint64_t duration_us;   // 停车时间，单位：微秒
    uint64_t billable_us;   // 计费时间，单位：微秒
    uint64_t units;         // 计费周期数
    uint64_t unit_cents;    // 每周期收费，单位：分
    uint64_t max_cents;     // 单次费用上限，单位：分

    if (fee_cents == NULL)
        return PARKING_FEE_BAD_ARG;

    *fee_cents = 0U;

    if (fee == NULL)
        return PARKING_FEE_BAD_ARG;

    if (!policy_valid(&fee->policy))
        return PARKING_FEE_BAD_POLICY;
        
    if (exit_us < entry_us)
        return PARKING_FEE_BAD_TIME;

    if (minutes_to_us(fee->policy.free_min, &free_us) != 0 ||
        minutes_to_us(fee->policy.unit_min, &unit_us) != 0)
        return PARKING_FEE_OVERFLOW;

    duration_us = exit_us - entry_us;
    if (duration_us <= free_us)
        return PARKING_FEE_OK;

    billable_us = duration_us - free_us;
    units = billable_us / unit_us;
    if (billable_us % unit_us != 0U) {
        if (units == UINT64_MAX)
            return PARKING_FEE_OVERFLOW;
        ++units;
    }

    unit_cents = fee->policy.unit_cents;
    max_cents = fee->policy.max_cents;

    /* 先判断封顶，避免 units * unit_cents 发生整数溢出。 */
    if (units > max_cents / unit_cents) {
        *fee_cents = max_cents;
        return PARKING_FEE_OK;
    }

    if (units > UINT64_MAX / unit_cents)
        return PARKING_FEE_OVERFLOW;

    *fee_cents = units * unit_cents;
    if (*fee_cents > max_cents)
        *fee_cents = max_cents;

    return PARKING_FEE_OK;
}

const char *parking_fee_status_name(parking_fee_status_t status)
{
    switch (status) {
        case PARKING_FEE_OK:
            return "OK";
        case PARKING_FEE_BAD_ARG:
            return "BAD_ARG";
        case PARKING_FEE_BAD_POLICY:
            return "BAD_POLICY";
        case PARKING_FEE_BAD_TIME:
            return "BAD_TIME";
        case PARKING_FEE_OVERFLOW:
            return "OVERFLOW";
        default:
            return "UNKNOWN";
    }
}

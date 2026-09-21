#include "parking_fee.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define US_PER_MINUTE 60000000ULL

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "[FAIL] %s:%d: %s\n",                       \
                    __func__, __LINE__, #condition);                       \
            return -1;                                                     \
        }                                                                  \
    } while (0)

#define RUN_TEST(test_function)                                            \
    do {                                                                   \
        if ((test_function)() != 0) {                                      \
            fprintf(stderr, "%s failed\n", #test_function);             \
            return 1;                                                      \
        }                                                                  \
        ++passed;                                                          \
        printf("[PASS] %s\n", #test_function);                           \
    } while (0)

static int policy_equal(const parking_fee_policy_t *left,
                        const parking_fee_policy_t *right)
{
    return left->free_min == right->free_min &&
           left->unit_min == right->unit_min &&
           left->unit_cents == right->unit_cents &&
           left->max_cents == right->max_cents;
}

static int expect_fee(parking_fee_t *fee,
                      uint64_t duration_us,
                      uint64_t expected_cents)
{
    const uint64_t entry_us = 1000000ULL;
    parking_fee_status_t status;
    uint64_t actual_cents = UINT64_MAX;

    CHECK(duration_us <= UINT64_MAX - entry_us);
    status = parking_fee_calc(fee, entry_us, entry_us + duration_us,
                              &actual_cents);
    if (status != PARKING_FEE_OK || actual_cents != expected_cents) {
        fprintf(stderr,
                "[FAIL] %s: duration=%" PRIu64
                " expected=%" PRIu64 " actual=%" PRIu64
                " status=%s\n",
                __func__, duration_us, expected_cents, actual_cents,
                parking_fee_status_name(status));
        return -1;
    }

    return 0;
}

static int test_default_policy_and_lifecycle(void)
{
    parking_fee_policy_t policy;
    parking_fee_t *fee;

    fee = parking_fee_create();
    CHECK(fee != NULL);
    CHECK(parking_fee_get_policy(fee, &policy) == 0);
    CHECK(policy.free_min == 30U);
    CHECK(policy.unit_min == 10U);
    CHECK(policy.unit_cents == 50U);
    CHECK(policy.max_cents == 2000U);

    parking_fee_destroy(fee);
    parking_fee_destroy(NULL);
    return 0;
}

static int test_normal_charging(void)
{
    parking_fee_t *fee;

    fee = parking_fee_create();
    CHECK(fee != NULL);

    CHECK(expect_fee(fee, 20ULL * US_PER_MINUTE, 0U) == 0);
    CHECK(expect_fee(fee, 35ULL * US_PER_MINUTE, 50U) == 0);
    CHECK(expect_fee(fee, 40ULL * US_PER_MINUTE, 50U) == 0);
    CHECK(expect_fee(fee, 41ULL * US_PER_MINUTE, 100U) == 0);
    CHECK(expect_fee(fee, 60ULL * US_PER_MINUTE, 150U) == 0);
    CHECK(expect_fee(fee, 430ULL * US_PER_MINUTE, 2000U) == 0);
    CHECK(expect_fee(fee, 1000ULL * US_PER_MINUTE, 2000U) == 0);

    parking_fee_destroy(fee);
    return 0;
}

static int test_time_boundaries(void)
{
    parking_fee_t *fee;

    fee = parking_fee_create();
    CHECK(fee != NULL);

    CHECK(expect_fee(fee, 0U, 0U) == 0);
    CHECK(expect_fee(fee, 30ULL * US_PER_MINUTE - 1U, 0U) == 0);
    CHECK(expect_fee(fee, 30ULL * US_PER_MINUTE, 0U) == 0);
    CHECK(expect_fee(fee, 30ULL * US_PER_MINUTE + 1U, 50U) == 0);
    CHECK(expect_fee(fee, 40ULL * US_PER_MINUTE, 50U) == 0);
    CHECK(expect_fee(fee, 40ULL * US_PER_MINUTE + 1U, 100U) == 0);
    CHECK(expect_fee(fee, 420ULL * US_PER_MINUTE, 1950U) == 0);
    CHECK(expect_fee(fee, 420ULL * US_PER_MINUTE + 1U, 2000U) == 0);

    parking_fee_destroy(fee);
    return 0;
}

static int test_custom_policy_and_copy(void)
{
    parking_fee_policy_t policy;
    parking_fee_policy_t actual;
    parking_fee_t *fee;

    fee = parking_fee_create();
    CHECK(fee != NULL);

    policy.free_min = 20U;
    policy.unit_min = 5U;
    policy.unit_cents = 25U;
    policy.max_cents = 100U;
    CHECK(parking_fee_set_policy(fee, &policy) == 0);

    /* set_policy 必须复制规则，不能保存调用者传入的指针。 */
    policy.unit_cents = 999U;
    CHECK(parking_fee_get_policy(fee, &actual) == 0);
    CHECK(actual.free_min == 20U);
    CHECK(actual.unit_min == 5U);
    CHECK(actual.unit_cents == 25U);
    CHECK(actual.max_cents == 100U);

    /* get_policy 返回副本，修改副本不能改变模块内部规则。 */
    actual.free_min = 0U;
    CHECK(parking_fee_get_policy(fee, &policy) == 0);
    CHECK(policy.free_min == 20U);

    CHECK(expect_fee(fee, 20ULL * US_PER_MINUTE, 0U) == 0);
    CHECK(expect_fee(fee, 20ULL * US_PER_MINUTE + 1U, 25U) == 0);
    CHECK(expect_fee(fee, 25ULL * US_PER_MINUTE, 25U) == 0);
    CHECK(expect_fee(fee, 25ULL * US_PER_MINUTE + 1U, 50U) == 0);
    CHECK(expect_fee(fee, 35ULL * US_PER_MINUTE, 75U) == 0);
    CHECK(expect_fee(fee, 35ULL * US_PER_MINUTE + 1U, 100U) == 0);

    parking_fee_destroy(fee);
    return 0;
}

static int test_invalid_arguments(void)
{
    parking_fee_policy_t policy;
    parking_fee_t *fee;
    uint64_t fee_cents;

    fee = parking_fee_create();
    CHECK(fee != NULL);
    CHECK(parking_fee_get_policy(fee, &policy) == 0);

    CHECK(parking_fee_get_policy(NULL, &policy) == -1);
    CHECK(parking_fee_get_policy(fee, NULL) == -1);
    CHECK(parking_fee_set_policy(NULL, &policy) == -1);
    CHECK(parking_fee_set_policy(fee, NULL) == -1);

    fee_cents = 1234U;
    CHECK(parking_fee_calc(NULL, 100U, 200U, &fee_cents) ==
          PARKING_FEE_BAD_ARG);
    CHECK(fee_cents == 0U);
    CHECK(parking_fee_calc(fee, 100U, 200U, NULL) ==
          PARKING_FEE_BAD_ARG);

    fee_cents = 1234U;
    CHECK(parking_fee_calc(fee, 200U, 100U, &fee_cents) ==
          PARKING_FEE_BAD_TIME);
    CHECK(fee_cents == 0U);

    parking_fee_destroy(fee);
    return 0;
}

static int test_invalid_policies_preserve_current_policy(void)
{
    parking_fee_policy_t original;
    parking_fee_policy_t invalid;
    parking_fee_policy_t actual;
    parking_fee_t *fee;

    fee = parking_fee_create();
    CHECK(fee != NULL);
    CHECK(parking_fee_get_policy(fee, &original) == 0);

    invalid = original;
    invalid.unit_min = 0U;
    CHECK(parking_fee_set_policy(fee, &invalid) == -1);

    invalid = original;
    invalid.unit_cents = 0U;
    CHECK(parking_fee_set_policy(fee, &invalid) == -1);

    invalid = original;
    invalid.max_cents = 0U;
    CHECK(parking_fee_set_policy(fee, &invalid) == -1);

    invalid = original;
    invalid.unit_cents = 100U;
    invalid.max_cents = 99U;
    CHECK(parking_fee_set_policy(fee, &invalid) == -1);

    CHECK(parking_fee_get_policy(fee, &actual) == 0);
    CHECK(policy_equal(&actual, &original));

    parking_fee_destroy(fee);
    return 0;
}

static int test_policy_boundaries(void)
{
    parking_fee_policy_t policy;
    parking_fee_t *fee;

    fee = parking_fee_create();
    CHECK(fee != NULL);

    /* 免费时间可以为 0；只要停车超过 0 微秒就进入计费。 */
    policy.free_min = 0U;
    policy.unit_min = 10U;
    policy.unit_cents = 50U;
    policy.max_cents = 2000U;
    CHECK(parking_fee_set_policy(fee, &policy) == 0);
    CHECK(expect_fee(fee, 0U, 0U) == 0);
    CHECK(expect_fee(fee, 1U, 50U) == 0);
    CHECK(expect_fee(fee, 10ULL * US_PER_MINUTE, 50U) == 0);

    /* 封顶金额不必是单周期费用的整数倍。 */
    policy.free_min = 0U;
    policy.unit_min = 1U;
    policy.unit_cents = 60U;
    policy.max_cents = 100U;
    CHECK(parking_fee_set_policy(fee, &policy) == 0);
    CHECK(expect_fee(fee, 1ULL * US_PER_MINUTE, 60U) == 0);
    CHECK(expect_fee(fee, 1ULL * US_PER_MINUTE + 1U, 100U) == 0);

    parking_fee_destroy(fee);
    return 0;
}

static int test_extreme_values(void)
{
    parking_fee_policy_t policy;
    parking_fee_t *fee;
    uint64_t free_us;
    uint64_t fee_cents;

    fee = parking_fee_create();
    CHECK(fee != NULL);

    policy.free_min = UINT32_MAX;
    policy.unit_min = UINT32_MAX;
    policy.unit_cents = UINT32_MAX;
    policy.max_cents = UINT32_MAX;
    CHECK(parking_fee_set_policy(fee, &policy) == 0);

    free_us = (uint64_t)UINT32_MAX * US_PER_MINUTE;
    CHECK(parking_fee_calc(fee, 0U, free_us, &fee_cents) ==
          PARKING_FEE_OK);
    CHECK(fee_cents == 0U);
    CHECK(parking_fee_calc(fee, 0U, free_us + 1U, &fee_cents) ==
          PARKING_FEE_OK);
    CHECK(fee_cents == UINT32_MAX);
    CHECK(parking_fee_calc(fee, 0U, UINT64_MAX, &fee_cents) ==
          PARKING_FEE_OK);
    CHECK(fee_cents == UINT32_MAX);
    CHECK(parking_fee_calc(fee, UINT64_MAX, UINT64_MAX, &fee_cents) ==
          PARKING_FEE_OK);
    CHECK(fee_cents == 0U);

    parking_fee_destroy(fee);
    return 0;
}

static int test_status_names(void)
{
    CHECK(strcmp(parking_fee_status_name(PARKING_FEE_OK), "OK") == 0);
    CHECK(strcmp(parking_fee_status_name(PARKING_FEE_BAD_ARG),
                 "BAD_ARG") == 0);
    CHECK(strcmp(parking_fee_status_name(PARKING_FEE_BAD_POLICY),
                 "BAD_POLICY") == 0);
    CHECK(strcmp(parking_fee_status_name(PARKING_FEE_BAD_TIME),
                 "BAD_TIME") == 0);
    CHECK(strcmp(parking_fee_status_name(PARKING_FEE_OVERFLOW),
                 "OVERFLOW") == 0);
    CHECK(strcmp(parking_fee_status_name((parking_fee_status_t)12345),
                 "UNKNOWN") == 0);
    return 0;
}

int main(void)
{
    unsigned int passed = 0U;

    RUN_TEST(test_default_policy_and_lifecycle);
    RUN_TEST(test_normal_charging);
    RUN_TEST(test_time_boundaries);
    RUN_TEST(test_custom_policy_and_copy);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_invalid_policies_preserve_current_policy);
    RUN_TEST(test_policy_boundaries);
    RUN_TEST(test_extreme_values);
    RUN_TEST(test_status_names);

    printf("parking_fee: all %u tests passed\n", passed);
    return 0;
}

#define _XOPEN_SOURCE 700

#include "parking_fee.h"
#include "parking_store.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MINUTE_US 60000000ULL
#define BASE_TIME_US 1700000000000000ULL

typedef struct {
    parking_fee_t *fee;
    unsigned int call_count;
} fee_context_t;

static void remove_database_files(const char *database_path)
{
    char sidecar[256];

    unlink(database_path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", database_path);
    unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", database_path);
    unlink(sidecar);
}

static parking_store_t *open_store(const char *database_path)
{
    parking_store_config_t config;

    memset(&config, 0, sizeof(config));
    config.database_path = database_path;
    config.initial_total_spaces = 2U;
    config.busy_timeout_ms = 1000;
    return parking_store_open(&config);
}

static int calculate_fee(void *context,
                         uint64_t entry_time_unix_us,
                         uint64_t exit_time_unix_us,
                         uint64_t *fee_cents)
{
    fee_context_t *fee_context = context;

    if (fee_context == NULL)
        return -1;

    ++fee_context->call_count;
    return parking_fee_calc(fee_context->fee,
                            entry_time_unix_us,
                            exit_time_unix_us,
                            fee_cents) == PARKING_FEE_OK
        ? 0
        : -1;
}

static uint64_t enter_vehicle(parking_store_t *store,
                              const char *plate_number,
                              uint64_t entry_time_unix_us)
{
    parking_store_check_result_t check;

    assert(parking_store_check_entry(store, plate_number,
                                     entry_time_unix_us, &check) == 0);
    assert(check.allowed == 1);
    assert(check.operation_id != 0U);
    assert(parking_store_complete_entry(store, check.operation_id,
                                        plate_number,
                                        entry_time_unix_us + 1U) == 0);
    return check.operation_id;
}

static void complete_exit(parking_store_t *store,
                          const char *plate_number,
                          const parking_store_check_result_t *check,
                          uint64_t completed_at_unix_us)
{
    assert(check->allowed == 1);
    assert(check->operation_id != 0U);
    assert(parking_store_complete_exit(store, check->operation_id,
                                       plate_number,
                                       completed_at_unix_us) == 0);
}

int main(void)
{
    char database_path[] = "/tmp/parking-store-test-XXXXXX";
    parking_fee_policy_t changed_policy;
    parking_store_check_result_t first_entry;
    parking_store_check_result_t repeated_entry;
    parking_store_check_result_t second_entry;
    parking_store_check_result_t first_exit;
    parking_store_check_result_t check;
    parking_store_status_t status;
    parking_fee_t *fee;
    parking_store_t *store;
    fee_context_t fee_context;
    uint64_t second_entry_time;
    unsigned int calls_before;
    int file_descriptor;

    file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0);
    close(file_descriptor);
    assert(unlink(database_path) == 0);

    fee = parking_fee_create();
    assert(fee != NULL);
    memset(&fee_context, 0, sizeof(fee_context));
    fee_context.fee = fee;

    store = open_store(database_path);
    assert(store != NULL);

    assert(parking_store_get_status(store, &status) == 0);
    assert(status.schema_version == 1U);
    assert(status.total_spaces == 2U);
    assert(status.occupied_spaces == 0U);
    assert(status.pending_entry_operations == 0U);
    assert(status.available_spaces == 2U);

    /* 入场检查可重试，且待完成入场也会占用可用容量。 */
    assert(parking_store_check_entry(store, "粤A3333G", BASE_TIME_US,
                                     &first_entry) == 0);
    assert(first_entry.allowed == 1);
    assert(first_entry.operation_id != 0U);
    assert(first_entry.pending_entry_operations == 1U);
    assert(first_entry.available_spaces == 1U);

    assert(parking_store_check_entry(store, "粤A3333G",
                                     BASE_TIME_US + 100U,
                                     &repeated_entry) == 0);
    assert(repeated_entry.allowed == 1);
    assert(repeated_entry.operation_id == first_entry.operation_id);
    assert(repeated_entry.pending_entry_operations == 1U);

    assert(parking_store_complete_entry(store, first_entry.operation_id,
                                        "粤A3333G",
                                        BASE_TIME_US + 1000U) == 0);
    assert(parking_store_complete_entry(store, first_entry.operation_id,
                                        "粤A3333G",
                                        BASE_TIME_US + 2000U) == 0);

    assert(parking_store_check_entry(store, "粤A3333G",
                                     BASE_TIME_US + MINUTE_US,
                                     &check) == 0);
    assert(check.allowed == 0);
    assert(check.reason == PARKING_STORE_REASON_ALREADY_INSIDE);

    second_entry_time = BASE_TIME_US + 2ULL * MINUTE_US;
    assert(parking_store_check_entry(store, "粤B88888",
                                     second_entry_time,
                                     &second_entry) == 0);
    assert(second_entry.allowed == 1);
    assert(second_entry.operation_id != first_entry.operation_id);
    assert(second_entry.available_spaces == 0U);

    assert(parking_store_check_entry(store, "粤C66666",
                                     BASE_TIME_US + 3ULL * MINUTE_US,
                                     &check) == 0);
    assert(check.allowed == 0);
    assert(check.reason == PARKING_STORE_REASON_LOT_FULL);

    /* 重启后仍能找到并完成原来的待入场操作。 */
    parking_store_close(store);
    store = open_store(database_path);
    assert(store != NULL);
    assert(parking_store_check_entry(store, "粤B88888",
                                     BASE_TIME_US + 4ULL * MINUTE_US,
                                     &check) == 0);
    assert(check.allowed == 1);
    assert(check.operation_id == second_entry.operation_id);
    assert(parking_store_complete_entry(store, check.operation_id,
                                        "粤B88888",
                                        BASE_TIME_US + 5ULL * MINUTE_US) == 0);

    assert(parking_store_get_status(store, &status) == 0);
    assert(status.occupied_spaces == 2U);
    assert(status.active_sessions == 2U);
    assert(status.available_spaces == 0U);

    /* 无在场记录时拒绝出场，并且不调用计费器。 */
    calls_before = fee_context.call_count;
    assert(parking_store_check_exit(store, "粤C66666",
                                    BASE_TIME_US + 6ULL * MINUTE_US,
                                    calculate_fee, &fee_context,
                                    &check) == 0);
    assert(check.allowed == 0);
    assert(check.reason == PARKING_STORE_REASON_NO_ACTIVE_SESSION);
    assert(fee_context.call_count == calls_before);

    /* 非法参数：出场接口必须提供计费函数。 */
    assert(parking_store_check_exit(store, "粤A3333G",
                                    BASE_TIME_US + 30ULL * MINUTE_US,
                                    NULL, &fee_context, &check) == -1);

    /* 出场早于入场时计费失败，事务回滚且不会留下待出场操作。 */
    calls_before = fee_context.call_count;
    assert(parking_store_check_exit(store, "粤A3333G",
                                    BASE_TIME_US - 1U,
                                    calculate_fee, &fee_context,
                                    &check) == -1);
    assert(fee_context.call_count == calls_before + 1U);

    /* 恰好 30 分钟免费；重复检查复用同一操作和冻结金额。 */
    assert(parking_store_check_exit(store, "粤A3333G",
                                    BASE_TIME_US + 30ULL * MINUTE_US,
                                    calculate_fee, &fee_context,
                                    &first_exit) == 0);
    assert(first_exit.allowed == 1);
    assert(first_exit.amount_cents == 0U);
    assert(first_exit.payment_required == 0);
    calls_before = fee_context.call_count;

    assert(parking_store_check_exit(store, "粤A3333G",
                                    BASE_TIME_US + 100ULL * MINUTE_US,
                                    calculate_fee, &fee_context,
                                    &check) == 0);
    assert(check.operation_id == first_exit.operation_id);
    assert(check.amount_cents == 0U);
    assert(fee_context.call_count == calls_before);
    complete_exit(store, "粤A3333G", &check,
                  BASE_TIME_US + 101ULL * MINUTE_US);

    /* 超过免费时长 1 微秒，按一个 10 分钟周期收 50 分。 */
    enter_vehicle(store, "粤C66666",
                  BASE_TIME_US + 110ULL * MINUTE_US);
    assert(parking_store_check_exit(
               store, "粤C66666",
               BASE_TIME_US + 140ULL * MINUTE_US + 1U,
               calculate_fee, &fee_context, &check) == 0);
    assert(check.amount_cents == 50U);
    assert(check.payment_required == 1);
    complete_exit(store, "粤C66666", &check,
                  BASE_TIME_US + 141ULL * MINUTE_US);

    /* 超长停车按默认规则封顶 2000 分。 */
    enter_vehicle(store, "粤D99999",
                  BASE_TIME_US + 150ULL * MINUTE_US);
    assert(parking_store_check_exit(
               store, "粤D99999",
               BASE_TIME_US + 580ULL * MINUTE_US,
               calculate_fee, &fee_context, &check) == 0);
    assert(check.amount_cents == 2000U);
    assert(check.payment_required == 1);
    complete_exit(store, "粤D99999", &check,
                  BASE_TIME_US + 581ULL * MINUTE_US);

    /* 首次检查后修改规则，待出场金额仍必须保持不变。 */
    assert(parking_store_check_exit(
               store, "粤B88888",
               second_entry_time + 40ULL * MINUTE_US,
               calculate_fee, &fee_context, &first_exit) == 0);
    assert(first_exit.amount_cents == 50U);
    calls_before = fee_context.call_count;

    changed_policy.free_min = 0U;
    changed_policy.unit_min = 1U;
    changed_policy.unit_cents = 777U;
    changed_policy.max_cents = 5000U;
    assert(parking_fee_set_policy(fee, &changed_policy) == 0);

    assert(parking_store_check_exit(
               store, "粤B88888",
               second_entry_time + 100ULL * MINUTE_US,
               calculate_fee, &fee_context, &check) == 0);
    assert(check.operation_id == first_exit.operation_id);
    assert(check.amount_cents == 50U);
    assert(fee_context.call_count == calls_before);

    /* 重启数据库后，仍返回之前持久化的 operation_id 和金额。 */
    parking_store_close(store);
    store = open_store(database_path);
    assert(store != NULL);
    assert(parking_store_check_exit(
               store, "粤B88888",
               second_entry_time + 200ULL * MINUTE_US,
               calculate_fee, &fee_context, &check) == 0);
    assert(check.operation_id == first_exit.operation_id);
    assert(check.amount_cents == 50U);
    assert(fee_context.call_count == calls_before);

    complete_exit(store, "粤B88888", &check,
                  second_entry_time + 201ULL * MINUTE_US);
    assert(parking_store_complete_exit(store, check.operation_id,
                                       "粤B88888",
                                       second_entry_time +
                                           202ULL * MINUTE_US) == 0);

    assert(parking_store_get_status(store, &status) == 0);
    assert(status.occupied_spaces == 0U);
    assert(status.active_sessions == 0U);
    assert(status.available_spaces == 2U);

    parking_store_close(store);
    parking_fee_destroy(fee);
    remove_database_files(database_path);
    puts("parking_store: all tests passed");
    return 0;
}

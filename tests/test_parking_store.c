#define _XOPEN_SOURCE 700

#include "parking_store.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    config.default_exit_fee_cents = 200U;
    config.busy_timeout_ms = 1000;
    return parking_store_open(&config);
}

int main(void)
{
    char database_path[] = "/tmp/parking-store-test-XXXXXX";
    parking_store_check_result_t first_entry;
    parking_store_check_result_t repeated_entry;
    parking_store_check_result_t second_entry;
    parking_store_check_result_t check;
    parking_store_status_t status;
    parking_store_t *store;
    int file_descriptor;

    file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0);
    close(file_descriptor);
    assert(unlink(database_path) == 0);

    store = open_store(database_path);
    assert(store != NULL);

    assert(parking_store_get_status(store, &status) == 0);
    assert(status.schema_version == 1U);
    assert(status.total_spaces == 2U);
    assert(status.occupied_spaces == 0U);
    assert(status.pending_entry_operations == 0U);
    assert(status.available_spaces == 2U);

    assert(parking_store_check_entry(
               store, "粤A3333G", 1700000000000000ULL,
               &first_entry) == 0);
    assert(first_entry.allowed == 1);
    assert(first_entry.operation_id != 0U);
    assert(first_entry.pending_entry_operations == 1U);
    assert(first_entry.available_spaces == 1U);

    assert(parking_store_check_entry(
               store, "粤A3333G", 1700000000000100ULL,
               &repeated_entry) == 0);
    assert(repeated_entry.allowed == 1);
    assert(repeated_entry.operation_id == first_entry.operation_id);
    assert(repeated_entry.pending_entry_operations == 1U);

    assert(parking_store_complete_entry(
               store, first_entry.operation_id, "粤A3333G",
               1700000001000000ULL) == 0);
    assert(parking_store_complete_entry(
               store, first_entry.operation_id, "粤A3333G",
               1700000001000100ULL) == 0);

    assert(parking_store_check_entry(
               store, "粤A3333G", 1700000002000000ULL, &check) == 0);
    assert(check.allowed == 0);
    assert(check.reason == PARKING_STORE_REASON_ALREADY_INSIDE);

    assert(parking_store_check_entry(
               store, "粤B88888", 1700000003000000ULL,
               &second_entry) == 0);
    assert(second_entry.allowed == 1);
    assert(second_entry.operation_id != first_entry.operation_id);
    assert(second_entry.available_spaces == 0U);

    assert(parking_store_check_entry(
               store, "粤C66666", 1700000004000000ULL, &check) == 0);
    assert(check.allowed == 0);
    assert(check.reason == PARKING_STORE_REASON_LOT_FULL);

    /* 模拟进程在第二辆车完成入场前退出。 */
    parking_store_close(store);
    store = open_store(database_path);
    assert(store != NULL);

    assert(parking_store_check_entry(
               store, "粤B88888", 1700000005000000ULL, &check) == 0);
    assert(check.allowed == 1);
    assert(check.operation_id == second_entry.operation_id);
    assert(parking_store_complete_entry(
               store, check.operation_id, "粤B88888",
               1700000006000000ULL) == 0);

    assert(parking_store_get_status(store, &status) == 0);
    assert(status.occupied_spaces == 2U);
    assert(status.active_sessions == 2U);
    assert(status.pending_entry_operations == 0U);
    assert(status.available_spaces == 0U);

    assert(parking_store_check_exit(
               store, "粤C66666", 1700000007000000ULL, &check) == 0);
    assert(check.allowed == 0);
    assert(check.reason == PARKING_STORE_REASON_NO_ACTIVE_SESSION);

    assert(parking_store_check_exit(
               store, "粤A3333G", 1700000008000000ULL, &check) == 0);
    assert(check.allowed == 1);
    assert(check.operation_id != 0U);
    assert(check.session_id != 0U);
    assert(check.amount_cents == 200U);
    assert(check.payment_required == 1);
    repeated_entry = check;

    assert(parking_store_check_exit(
               store, "粤A3333G", 1700000008000100ULL, &check) == 0);
    assert(check.operation_id == repeated_entry.operation_id);

    assert(parking_store_complete_exit(
               store, check.operation_id, "粤A3333G",
               1700000010000000ULL) == 0);
    assert(parking_store_complete_exit(
               store, check.operation_id, "粤A3333G",
               1700000010000100ULL) == 0);

    assert(parking_store_get_status(store, &status) == 0);
    assert(status.occupied_spaces == 1U);
    assert(status.active_sessions == 1U);
    assert(status.available_spaces == 1U);

    assert(parking_store_check_exit(
               store, "粤A3333G", 1700000011000000ULL, &check) == 0);
    assert(check.allowed == 0);
    assert(check.reason == PARKING_STORE_REASON_NO_ACTIVE_SESSION);

    parking_store_close(store);
    remove_database_files(database_path);
    puts("parking_store: all tests passed");
    return 0;
}

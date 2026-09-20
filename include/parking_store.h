#ifndef PARKING_STORE_H
#define PARKING_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARKING_STORE_PLATE_CAPACITY 32U
#define PARKING_STORE_DEFAULT_TOTAL_SPACES 100U
#define PARKING_STORE_DEFAULT_EXIT_FEE_CENTS 200ULL
#define PARKING_STORE_DEFAULT_BUSY_TIMEOUT_MS 3000

typedef struct parking_store parking_store_t;

typedef enum {
    PARKING_STORE_REASON_NONE = 0,
    PARKING_STORE_REASON_ALREADY_INSIDE,
    PARKING_STORE_REASON_LOT_FULL,
    PARKING_STORE_REASON_NO_ACTIVE_SESSION
} parking_store_reason_t;

typedef struct {
    const char *database_path;

    /* 仅在首次创建数据库时使用，已有数据库保留其原有容量。 */
    uint32_t initial_total_spaces;
    uint64_t default_exit_fee_cents;
    int busy_timeout_ms;
} parking_store_config_t;

typedef struct {
    int allowed;
    parking_store_reason_t reason;
    uint64_t operation_id;
    uint64_t session_id;
    uint64_t amount_cents;
    int payment_required;

    uint32_t total_spaces;
    uint32_t occupied_spaces;
    uint32_t pending_entry_operations;
    uint32_t available_spaces;
} parking_store_check_result_t;

typedef struct {
    uint32_t schema_version;
    uint32_t total_spaces;
    uint32_t occupied_spaces;
    uint32_t pending_entry_operations;
    uint32_t available_spaces;
    uint64_t active_sessions;
} parking_store_status_t;

/*
 * 存储层在内部动态加载目标系统的 libsqlite3.so.0/libsqlite3.so。
 * 这样交叉编译不依赖当前工具链 sysroot 中缺失的 SQLite 开发包；
 * 板端运行时仍必须安装与系统 ABI 匹配的 SQLite3 共享库。
 */
parking_store_t *parking_store_open(const parking_store_config_t *config);
void parking_store_close(parking_store_t *store);

/*
 * 检查成功且允许通行时返回持久化 operation_id。
 * 同一车牌的未完成检查会返回原 operation_id，支持进程重启后重试。
 * 业务拒绝仍返回 0，并通过 result->allowed/reason 表示；-1 表示存储错误。
 * 时间均为 Unix epoch 微秒，不能传入单调时钟时间。
 */
int parking_store_check_entry(parking_store_t *store,
                              const char *plate_number,
                              uint64_t requested_at_unix_us,
                              parking_store_check_result_t *result);

int parking_store_check_exit(parking_store_t *store,
                             const char *plate_number,
                             uint64_t requested_at_unix_us,
                             parking_store_check_result_t *result);

/* 完成接口可重复调用；同一 operation_id 已提交时仍返回成功。 */
int parking_store_complete_entry(parking_store_t *store,
                                 uint64_t operation_id,
                                 const char *plate_number,
                                 uint64_t completed_at_unix_us);

int parking_store_complete_exit(parking_store_t *store,
                                uint64_t operation_id,
                                const char *plate_number,
                                uint64_t completed_at_unix_us);

int parking_store_get_status(parking_store_t *store,
                             parking_store_status_t *status);

int parking_store_copy_last_error(parking_store_t *store,
                                  char *buffer,
                                  size_t capacity);

const char *parking_store_reason_name(parking_store_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif

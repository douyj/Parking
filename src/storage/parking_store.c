#include "parking_store.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARKING_STORE_ERROR_CAPACITY 256U
#define PARKING_STORE_SCHEMA_VERSION 1U

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101

#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_FULLMUTEX 0x00010000

#define STORE_EVENT_ENTRY 1
#define STORE_EVENT_EXIT 2

#define STORE_EVENT_PENDING 0
#define STORE_EVENT_COMMITTED 1
#define STORE_EVENT_REJECTED 2

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

typedef struct {
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*close_v2)(sqlite3 *);
    const char *(*errmsg)(sqlite3 *);
    int (*exec)(sqlite3 *, const char *,
                int (*)(void *, int, char **, char **), void *, char **);
    void (*free_memory)(void *);
    int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **,
                      const char **);
    int (*bind_text)(sqlite3_stmt *, int, const char *, int,
                     void (*)(void *));
    int (*bind_int)(sqlite3_stmt *, int, int);
    int (*bind_int64)(sqlite3_stmt *, int, int64_t);
    int (*step)(sqlite3_stmt *);
    int (*finalize)(sqlite3_stmt *);
    int (*column_int)(sqlite3_stmt *, int);
    int64_t (*column_int64)(sqlite3_stmt *, int);
    int64_t (*last_insert_rowid)(sqlite3 *);
    int (*changes)(sqlite3 *);
    int (*busy_timeout)(sqlite3 *, int);
} sqlite_api_t;

struct parking_store {
    void *sqlite_library;
    sqlite_api_t api;
    sqlite3 *database;
    char last_error[PARKING_STORE_ERROR_CAPACITY];
};

static void set_error(parking_store_t *store, const char *format, ...)
{
    va_list arguments;

    if (store == NULL || format == NULL)
        return;

    va_start(arguments, format);
    vsnprintf(store->last_error, sizeof(store->last_error),
              format, arguments);
    va_end(arguments);
}

static void set_database_error(parking_store_t *store,
                               const char *operation)
{
    const char *message = "未知 SQLite 错误";

    if (store != NULL && store->database != NULL &&
        store->api.errmsg != NULL) {
        const char *database_message = store->api.errmsg(store->database);

        if (database_message != NULL)
            message = database_message;
    }

    set_error(store, "%s: %s",
              operation != NULL ? operation : "SQLite 操作失败",
              message);
}

static int load_symbol(void *library, const char *name,
                       void *destination, size_t destination_size)
{
    void *symbol;

    if (library == NULL || name == NULL || destination == NULL ||
        destination_size != sizeof(symbol))
        return -1;

    dlerror();
    symbol = dlsym(library, name);
    if (symbol == NULL || dlerror() != NULL)
        return -1;

    memcpy(destination, &symbol, sizeof(symbol));
    return 0;
}

#define LOAD_SQLITE_SYMBOL(store, member, symbol_name)                    \
    do {                                                                  \
        if (load_symbol((store)->sqlite_library, (symbol_name),           \
                        &(store)->api.member,                             \
                        sizeof((store)->api.member)) != 0) {              \
            set_error((store), "SQLite 缺少符号 %s", (symbol_name));     \
            return -1;                                                    \
        }                                                                 \
    } while (0)

static int load_sqlite(parking_store_t *store)
{
    static const char *library_names[] = {
        "libsqlite3.so.0",
        "libsqlite3.so"
    };
    size_t i;

    for (i = 0U; i < sizeof(library_names) / sizeof(library_names[0]);
         ++i) {
        store->sqlite_library = dlopen(library_names[i], RTLD_NOW | RTLD_LOCAL);
        if (store->sqlite_library != NULL)
            break;
    }

    if (store->sqlite_library == NULL) {
        const char *message = dlerror();

        set_error(store, "无法加载 SQLite3 共享库: %s",
                  message != NULL ? message : "未知错误");
        return -1;
    }

    LOAD_SQLITE_SYMBOL(store, open_v2, "sqlite3_open_v2");
    LOAD_SQLITE_SYMBOL(store, close_v2, "sqlite3_close_v2");
    LOAD_SQLITE_SYMBOL(store, errmsg, "sqlite3_errmsg");
    LOAD_SQLITE_SYMBOL(store, exec, "sqlite3_exec");
    LOAD_SQLITE_SYMBOL(store, free_memory, "sqlite3_free");
    LOAD_SQLITE_SYMBOL(store, prepare_v2, "sqlite3_prepare_v2");
    LOAD_SQLITE_SYMBOL(store, bind_text, "sqlite3_bind_text");
    LOAD_SQLITE_SYMBOL(store, bind_int, "sqlite3_bind_int");
    LOAD_SQLITE_SYMBOL(store, bind_int64, "sqlite3_bind_int64");
    LOAD_SQLITE_SYMBOL(store, step, "sqlite3_step");
    LOAD_SQLITE_SYMBOL(store, finalize, "sqlite3_finalize");
    LOAD_SQLITE_SYMBOL(store, column_int, "sqlite3_column_int");
    LOAD_SQLITE_SYMBOL(store, column_int64, "sqlite3_column_int64");
    LOAD_SQLITE_SYMBOL(store, last_insert_rowid, "sqlite3_last_insert_rowid");
    LOAD_SQLITE_SYMBOL(store, changes, "sqlite3_changes");
    LOAD_SQLITE_SYMBOL(store, busy_timeout, "sqlite3_busy_timeout");
    return 0;
}

static int execute_sql(parking_store_t *store, const char *sql,
                       const char *operation)
{
    char *message = NULL;
    int result;

    result = store->api.exec(store->database, sql, NULL, NULL, &message);
    if (result != SQLITE_OK) {
        if (message != NULL) {
            set_error(store, "%s: %s", operation, message);
            store->api.free_memory(message);
        } else {
            set_database_error(store, operation);
        }
        return -1;
    }

    return 0;
}

static void rollback_silently(parking_store_t *store)
{
    char *message = NULL;

    if (store == NULL || store->database == NULL)
        return;

    store->api.exec(store->database, "ROLLBACK", NULL, NULL, &message);
    if (message != NULL)
        store->api.free_memory(message);
}

static int begin_transaction(parking_store_t *store)
{
    return execute_sql(store, "BEGIN IMMEDIATE", "开始 SQLite 事务失败");
}

static int commit_transaction(parking_store_t *store)
{
    return execute_sql(store, "COMMIT", "提交 SQLite 事务失败");
}

static int prepare_statement(parking_store_t *store, const char *sql,
                             sqlite3_stmt **statement,
                             const char *operation)
{
    if (store->api.prepare_v2(store->database, sql, -1, statement, NULL) !=
        SQLITE_OK) {
        set_database_error(store, operation);
        return -1;
    }

    return 0;
}

static int bind_text(parking_store_t *store, sqlite3_stmt *statement,
                     int index, const char *value,
                     const char *operation)
{
    /* NULL destructor 表示 SQLITE_STATIC；statement 在 value 失效前完成。 */
    if (store->api.bind_text(statement, index, value, -1, NULL) != SQLITE_OK) {
        set_database_error(store, operation);
        return -1;
    }

    return 0;
}

static int bind_uint64(parking_store_t *store, sqlite3_stmt *statement,
                       int index, uint64_t value,
                       const char *operation)
{
    if (value > (uint64_t)INT64_MAX ||
        store->api.bind_int64(statement, index, (int64_t)value) !=
            SQLITE_OK) {
        set_database_error(store, operation);
        return -1;
    }

    return 0;
}

static int validate_plate(const char *plate_number)
{
    size_t length;

    if (plate_number == NULL || plate_number[0] == '\0')
        return -1;

    length = strlen(plate_number);
    return length < PARKING_STORE_PLATE_CAPACITY ? 0 : -1;
}

static int validate_timestamp(uint64_t timestamp)
{
    return timestamp > 0U && timestamp <= (uint64_t)INT64_MAX ? 0 : -1;
}

static int initialize_schema(parking_store_t *store,
                             uint32_t initial_total_spaces)
{
    static const char schema_sql[] =
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "id INTEGER PRIMARY KEY CHECK(id = 1),"
        "version INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS parking_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "event_type INTEGER NOT NULL CHECK(event_type IN (1, 2)),"
        "plate_number TEXT NOT NULL,"
        "session_id INTEGER,"
        "requested_at_unix_us INTEGER NOT NULL,"
        "completed_at_unix_us INTEGER,"
        "amount_cents INTEGER NOT NULL DEFAULT 0 CHECK(amount_cents >= 0),"
        "state INTEGER NOT NULL CHECK(state IN (0, 1, 2)),"
        "rejection_reason INTEGER NOT NULL DEFAULT 0,"
        "FOREIGN KEY(session_id) REFERENCES parking_sessions(id),"
        "CHECK((state = 0 AND completed_at_unix_us IS NULL "
        "AND rejection_reason = 0) OR "
        "(state = 1 AND completed_at_unix_us IS NOT NULL "
        "AND rejection_reason = 0) OR "
        "(state = 2 AND completed_at_unix_us IS NOT NULL "
        "AND rejection_reason != 0)));"
        "CREATE TABLE IF NOT EXISTS parking_sessions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "plate_number TEXT NOT NULL,"
        "entry_time_unix_us INTEGER NOT NULL,"
        "exit_time_unix_us INTEGER,"
        "fee_cents INTEGER NOT NULL DEFAULT 0 CHECK(fee_cents >= 0),"
        "status INTEGER NOT NULL CHECK(status IN (0, 1)),"
        "entry_operation_id INTEGER NOT NULL UNIQUE,"
        "exit_operation_id INTEGER UNIQUE,"
        "FOREIGN KEY(entry_operation_id) REFERENCES parking_events(id),"
        "FOREIGN KEY(exit_operation_id) REFERENCES parking_events(id),"
        "CHECK((status = 0 AND exit_time_unix_us IS NULL "
        "AND exit_operation_id IS NULL) OR "
        "(status = 1 AND exit_time_unix_us IS NOT NULL "
        "AND exit_operation_id IS NOT NULL)));"
        "CREATE TABLE IF NOT EXISTS parking_spaces ("
        "id INTEGER PRIMARY KEY CHECK(id = 1),"
        "total_spaces INTEGER NOT NULL CHECK(total_spaces > 0),"
        "occupied_spaces INTEGER NOT NULL CHECK(occupied_spaces >= 0 "
        "AND occupied_spaces <= total_spaces),"
        "updated_at_unix_us INTEGER NOT NULL DEFAULT 0);"
        "CREATE UNIQUE INDEX IF NOT EXISTS "
        "idx_parking_sessions_active_plate "
        "ON parking_sessions(plate_number) WHERE status = 0;"
        "CREATE UNIQUE INDEX IF NOT EXISTS "
        "idx_parking_events_pending_plate_type "
        "ON parking_events(event_type, plate_number) WHERE state = 0;"
        "CREATE INDEX IF NOT EXISTS idx_parking_events_session "
        "ON parking_events(session_id);";
    sqlite3_stmt *statement = NULL;
    int step_result;
    int version;

    if (begin_transaction(store) != 0)
        return -1;

    if (execute_sql(store, schema_sql, "创建停车数据库 schema 失败") != 0)
        goto fail;

    if (execute_sql(
            store,
            "INSERT OR IGNORE INTO schema_version(id, version) VALUES(1, 1)",
            "初始化 schema 版本失败") != 0)
        goto fail;

    if (prepare_statement(
            store,
            "INSERT OR IGNORE INTO parking_spaces("
            "id, total_spaces, occupied_spaces, updated_at_unix_us) "
            "VALUES(1, ?, 0, 0)",
            &statement, "准备初始化车位容量失败") != 0)
        goto fail;

    if (store->api.bind_int(statement, 1,
                            (int)initial_total_spaces) != SQLITE_OK) {
        set_database_error(store, "绑定初始车位容量失败");
        goto fail;
    }

    step_result = store->api.step(statement);
    if (step_result != SQLITE_DONE) {
        set_database_error(store, "初始化车位容量失败");
        goto fail;
    }
    store->api.finalize(statement);
    statement = NULL;

    if (prepare_statement(store,
                          "SELECT version FROM schema_version WHERE id = 1",
                          &statement, "读取 schema 版本失败") != 0)
        goto fail;

    step_result = store->api.step(statement);
    if (step_result != SQLITE_ROW) {
        set_database_error(store, "schema 版本记录不存在");
        goto fail;
    }
    version = store->api.column_int(statement, 0);
    store->api.finalize(statement);
    statement = NULL;

    if (version != (int)PARKING_STORE_SCHEMA_VERSION) {
        set_error(store, "不支持的停车数据库 schema 版本: %d", version);
        goto fail;
    }

    if (commit_transaction(store) != 0) {
        rollback_silently(store);
        return -1;
    }
    return 0;

fail:
    if (statement != NULL)
        store->api.finalize(statement);
    rollback_silently(store);
    return -1;
}

static int read_capacity(parking_store_t *store,
                         uint32_t *total_spaces,
                         uint32_t *occupied_spaces,
                         uint32_t *pending_entries)
{
    static const char sql[] =
        "SELECT total_spaces, occupied_spaces, "
        "(SELECT COUNT(*) FROM parking_events "
        " WHERE event_type = 1 AND state = 0) "
        "FROM parking_spaces WHERE id = 1";
    sqlite3_stmt *statement = NULL;
    int result;
    int64_t total;
    int64_t occupied;
    int64_t pending;

    if (prepare_statement(store, sql, &statement,
                          "准备读取车位状态失败") != 0)
        return -1;

    result = store->api.step(statement);
    if (result != SQLITE_ROW) {
        set_database_error(store, "读取车位状态失败");
        store->api.finalize(statement);
        return -1;
    }

    total = store->api.column_int64(statement, 0);
    occupied = store->api.column_int64(statement, 1);
    pending = store->api.column_int64(statement, 2);
    store->api.finalize(statement);

    if (total <= 0 || total > (int64_t)UINT32_MAX || occupied < 0 ||
        occupied > total || pending < 0 || pending > (int64_t)UINT32_MAX) {
        set_error(store, "停车数据库中的车位统计无效");
        return -1;
    }

    *total_spaces = (uint32_t)total;
    *occupied_spaces = (uint32_t)occupied;
    *pending_entries = (uint32_t)pending;
    return 0;
}

static void fill_capacity_result(parking_store_check_result_t *result,
                                 uint32_t total_spaces,
                                 uint32_t occupied_spaces,
                                 uint32_t pending_entries)
{
    uint64_t unavailable = (uint64_t)occupied_spaces + pending_entries;

    result->total_spaces = total_spaces;
    result->occupied_spaces = occupied_spaces;
    result->pending_entry_operations = pending_entries;
    result->available_spaces = unavailable < total_spaces
        ? total_spaces - (uint32_t)unavailable
        : 0U;
}

static int find_pending_operation(parking_store_t *store,
                                  int event_type,
                                  const char *plate_number,
                                  uint64_t *operation_id,
                                  uint64_t *session_id,
                                  uint64_t *amount_cents)
{
    static const char sql[] =
        "SELECT id, COALESCE(session_id, 0), amount_cents "
        "FROM parking_events "
        "WHERE event_type = ? AND plate_number = ? AND state = 0 "
        "ORDER BY id LIMIT 1";
    sqlite3_stmt *statement = NULL;
    int step_result;
    int64_t id;
    int64_t stored_session_id;
    int64_t amount;

    if (prepare_statement(store, sql, &statement,
                          "准备查询未完成停车操作失败") != 0)
        return -1;

    if (store->api.bind_int(statement, 1, event_type) != SQLITE_OK ||
        bind_text(store, statement, 2, plate_number,
                  "绑定未完成停车操作车牌失败") != 0) {
        store->api.finalize(statement);
        return -1;
    }

    step_result = store->api.step(statement);
    if (step_result == SQLITE_DONE) {
        store->api.finalize(statement);
        return 0;
    }
    if (step_result != SQLITE_ROW) {
        set_database_error(store, "查询未完成停车操作失败");
        store->api.finalize(statement);
        return -1;
    }

    id = store->api.column_int64(statement, 0);
    stored_session_id = store->api.column_int64(statement, 1);
    amount = store->api.column_int64(statement, 2);
    store->api.finalize(statement);

    if (id <= 0 || stored_session_id < 0 || amount < 0) {
        set_error(store, "未完成停车操作记录无效");
        return -1;
    }

    *operation_id = (uint64_t)id;
    *session_id = (uint64_t)stored_session_id;
    *amount_cents = (uint64_t)amount;
    return 1;
}

static int find_active_session(parking_store_t *store,
                               const char *plate_number,
                               uint64_t *session_id,
                               uint64_t *entry_time_unix_us)
{
    static const char sql[] =
        "SELECT id, entry_time_unix_us FROM parking_sessions "
        "WHERE plate_number = ? AND status = 0 ORDER BY id LIMIT 1";
    sqlite3_stmt *statement = NULL;
    int step_result;
    int64_t id;
    int64_t entry_time;

    if (prepare_statement(store, sql, &statement,
                          "准备查询在场车辆失败") != 0)
        return -1;
    if (bind_text(store, statement, 1, plate_number,
                  "绑定在场车辆车牌失败") != 0) {
        store->api.finalize(statement);
        return -1;
    }

    step_result = store->api.step(statement);
    if (step_result == SQLITE_DONE) {
        store->api.finalize(statement);
        return 0;
    }
    if (step_result != SQLITE_ROW) {
        set_database_error(store, "查询在场车辆失败");
        store->api.finalize(statement);
        return -1;
    }

    id = store->api.column_int64(statement, 0);
    entry_time = store->api.column_int64(statement, 1);
    store->api.finalize(statement);
    if (id <= 0 || entry_time <= 0) {
        set_error(store, "在场车辆记录无效");
        return -1;
    }

    *session_id = (uint64_t)id;
    if (entry_time_unix_us != NULL)
        *entry_time_unix_us = (uint64_t)entry_time;
    return 1;
}

static int insert_event(parking_store_t *store,
                        int event_type,
                        const char *plate_number,
                        uint64_t session_id,
                        uint64_t requested_at_unix_us,
                        uint64_t amount_cents,
                        int state,
                        parking_store_reason_t rejection_reason,
                        uint64_t *operation_id)
{
    static const char sql[] =
        "INSERT INTO parking_events("
        "event_type, plate_number, session_id, requested_at_unix_us, "
        "completed_at_unix_us, amount_cents, state, rejection_reason) "
        "VALUES(?, ?, NULLIF(?, 0), ?, CASE WHEN ? = 2 THEN ? ELSE NULL END, "
        "?, ?, ?)";
    sqlite3_stmt *statement = NULL;
    int step_result;

    if (prepare_statement(store, sql, &statement,
                          "准备写入停车事件失败") != 0)
        return -1;

    if (store->api.bind_int(statement, 1, event_type) != SQLITE_OK ||
        bind_text(store, statement, 2, plate_number,
                  "绑定停车事件车牌失败") != 0 ||
        bind_uint64(store, statement, 3, session_id,
                    "绑定停车事件会话失败") != 0 ||
        bind_uint64(store, statement, 4, requested_at_unix_us,
                    "绑定停车事件时间失败") != 0 ||
        store->api.bind_int(statement, 5, state) != SQLITE_OK ||
        bind_uint64(store, statement, 6, requested_at_unix_us,
                    "绑定停车事件完成时间失败") != 0 ||
        bind_uint64(store, statement, 7, amount_cents,
                    "绑定停车事件金额失败") != 0 ||
        store->api.bind_int(statement, 8, state) != SQLITE_OK ||
        store->api.bind_int(statement, 9,
                            (int)rejection_reason) != SQLITE_OK) {
        set_database_error(store, "绑定停车事件参数失败");
        store->api.finalize(statement);
        return -1;
    }

    step_result = store->api.step(statement);
    if (step_result != SQLITE_DONE) {
        set_database_error(store, "写入停车事件失败");
        store->api.finalize(statement);
        return -1;
    }
    store->api.finalize(statement);

    if (operation_id != NULL) {
        int64_t id = store->api.last_insert_rowid(store->database);

        if (id <= 0) {
            set_error(store, "SQLite 未返回有效 operation_id");
            return -1;
        }
        *operation_id = (uint64_t)id;
    }

    return 0;
}

parking_store_t *parking_store_open(const parking_store_config_t *config)
{
    parking_store_t *store;
    uint32_t initial_total_spaces;
    int busy_timeout_ms;
    int flags;

    if (config == NULL || config->database_path == NULL ||
        config->database_path[0] == '\0')
        return NULL;

    initial_total_spaces = config->initial_total_spaces != 0U
        ? config->initial_total_spaces
        : PARKING_STORE_DEFAULT_TOTAL_SPACES;
    busy_timeout_ms = config->busy_timeout_ms > 0
        ? config->busy_timeout_ms
        : PARKING_STORE_DEFAULT_BUSY_TIMEOUT_MS;

    if (initial_total_spaces > (uint32_t)INT_MAX)
        return NULL;

    store = calloc(1, sizeof(*store));
    if (store == NULL)
        return NULL;

    if (load_sqlite(store) != 0)
        goto fail;

    flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_FULLMUTEX;
    if (store->api.open_v2(config->database_path, &store->database,
                           flags, NULL) != SQLITE_OK) {
        set_database_error(store, "打开停车数据库失败");
        goto fail;
    }

    if (store->api.busy_timeout(store->database, busy_timeout_ms) !=
        SQLITE_OK) {
        set_database_error(store, "设置 SQLite busy timeout 失败");
        goto fail;
    }

    if (execute_sql(store, "PRAGMA foreign_keys = ON",
                    "启用 SQLite 外键失败") != 0 ||
        execute_sql(store, "PRAGMA journal_mode = WAL",
                    "启用 SQLite WAL 失败") != 0 ||
        execute_sql(store, "PRAGMA synchronous = FULL",
                    "设置 SQLite 同步模式失败") != 0 ||
        initialize_schema(store, initial_total_spaces) != 0)
        goto fail;

    store->last_error[0] = '\0';
    return store;

fail:
    fprintf(stderr, "parking_store: %s\n",
            store->last_error[0] != '\0'
                ? store->last_error
                : "初始化失败");
    parking_store_close(store);
    return NULL;
}

void parking_store_close(parking_store_t *store)
{
    if (store == NULL)
        return;

    if (store->database != NULL && store->api.close_v2 != NULL)
        store->api.close_v2(store->database);
    if (store->sqlite_library != NULL)
        dlclose(store->sqlite_library);
    free(store);
}

int parking_store_check_entry(parking_store_t *store,
                              const char *plate_number,
                              uint64_t requested_at_unix_us,
                              parking_store_check_result_t *result)
{
    uint64_t operation_id = 0;
    uint64_t session_id = 0;
    uint64_t amount_cents = 0;
    uint32_t total_spaces;
    uint32_t occupied_spaces;
    uint32_t pending_entries;
    int query_result;

    if (store == NULL || result == NULL ||
        validate_plate(plate_number) != 0 ||
        validate_timestamp(requested_at_unix_us) != 0)
        return -1;

    memset(result, 0, sizeof(*result));
    store->last_error[0] = '\0';
    if (begin_transaction(store) != 0)
        return -1;

    query_result = find_pending_operation(
        store, STORE_EVENT_ENTRY, plate_number,
        &operation_id, &session_id, &amount_cents);
    if (query_result < 0)
        goto fail;

    if (read_capacity(store, &total_spaces, &occupied_spaces,
                      &pending_entries) != 0)
        goto fail;

    if (query_result == 1) {
        result->allowed = 1;
        result->operation_id = operation_id;
        fill_capacity_result(result, total_spaces, occupied_spaces,
                             pending_entries);
        if (commit_transaction(store) != 0)
            goto fail_without_rollback_error;
        return 0;
    }

    query_result = find_active_session(store, plate_number, &session_id,
                                       NULL);
    if (query_result < 0)
        goto fail;
    if (query_result == 1) {
        result->reason = PARKING_STORE_REASON_ALREADY_INSIDE;
        if (insert_event(store, STORE_EVENT_ENTRY, plate_number,
                         session_id, requested_at_unix_us, 0,
                         STORE_EVENT_REJECTED,
                         result->reason, NULL) != 0)
            goto fail;
        fill_capacity_result(result, total_spaces, occupied_spaces,
                             pending_entries);
        if (commit_transaction(store) != 0)
            goto fail_without_rollback_error;
        return 0;
    }

    if ((uint64_t)occupied_spaces + pending_entries >= total_spaces) {
        result->reason = PARKING_STORE_REASON_LOT_FULL;
        if (insert_event(store, STORE_EVENT_ENTRY, plate_number, 0,
                         requested_at_unix_us, 0,
                         STORE_EVENT_REJECTED,
                         result->reason, NULL) != 0)
            goto fail;
        fill_capacity_result(result, total_spaces, occupied_spaces,
                             pending_entries);
        if (commit_transaction(store) != 0)
            goto fail_without_rollback_error;
        return 0;
    }

    if (insert_event(store, STORE_EVENT_ENTRY, plate_number, 0,
                     requested_at_unix_us, 0, STORE_EVENT_PENDING,
                     PARKING_STORE_REASON_NONE, &operation_id) != 0)
        goto fail;

    result->allowed = 1;
    result->operation_id = operation_id;
    fill_capacity_result(result, total_spaces, occupied_spaces,
                         pending_entries + 1U);
    if (commit_transaction(store) != 0)
        goto fail_without_rollback_error;
    return 0;

fail:
    rollback_silently(store);
    return -1;

fail_without_rollback_error:
    rollback_silently(store);
    return -1;
}

int parking_store_check_exit(parking_store_t *store,
                             const char *plate_number,
                             uint64_t requested_at_unix_us,
                             parking_store_fee_fn calculate_fee,
                             void *fee_context,
                             parking_store_check_result_t *result)
{
    uint64_t operation_id = 0;
    uint64_t session_id = 0;
    uint64_t entry_time_unix_us = 0;
    uint64_t amount_cents = 0;
    uint32_t total_spaces;
    uint32_t occupied_spaces;
    uint32_t pending_entries;
    int query_result;

    if (store == NULL || calculate_fee == NULL || result == NULL ||
        validate_plate(plate_number) != 0 ||
        validate_timestamp(requested_at_unix_us) != 0)
        return -1;

    memset(result, 0, sizeof(*result));
    store->last_error[0] = '\0';
    if (begin_transaction(store) != 0)
        return -1;

    query_result = find_pending_operation(
        store, STORE_EVENT_EXIT, plate_number,
        &operation_id, &session_id, &amount_cents);
    if (query_result < 0)
        goto fail;

    if (read_capacity(store, &total_spaces, &occupied_spaces,
                      &pending_entries) != 0)
        goto fail;

    if (query_result == 1) {
        result->allowed = 1;
        result->operation_id = operation_id;
        result->session_id = session_id;
        result->amount_cents = amount_cents;
        result->payment_required = amount_cents > 0U;
        fill_capacity_result(result, total_spaces, occupied_spaces,
                             pending_entries);
        if (commit_transaction(store) != 0)
            goto fail_after_commit;
        return 0;
    }

    query_result = find_active_session(store, plate_number, &session_id,
                                       &entry_time_unix_us);
    if (query_result < 0)
        goto fail;
    if (query_result == 0) {
        result->reason = PARKING_STORE_REASON_NO_ACTIVE_SESSION;
        if (insert_event(store, STORE_EVENT_EXIT, plate_number, 0,
                         requested_at_unix_us, 0,
                         STORE_EVENT_REJECTED,
                         result->reason, NULL) != 0)
            goto fail;
        fill_capacity_result(result, total_spaces, occupied_spaces,
                             pending_entries);
        if (commit_transaction(store) != 0)
            goto fail_after_commit;
        return 0;
    }

    if (calculate_fee(fee_context, entry_time_unix_us,
                      requested_at_unix_us, &amount_cents) != 0) {
        set_error(store, "计算停车费失败");
        goto fail;
    }
    if (amount_cents > (uint64_t)INT64_MAX) {
        set_error(store, "停车费超出 SQLite 可存储范围");
        goto fail;
    }
    if (insert_event(store, STORE_EVENT_EXIT, plate_number, session_id,
                     requested_at_unix_us, amount_cents,
                     STORE_EVENT_PENDING, PARKING_STORE_REASON_NONE,
                     &operation_id) != 0)
        goto fail;

    result->allowed = 1;
    result->operation_id = operation_id;
    result->session_id = session_id;
    result->amount_cents = amount_cents;
    result->payment_required = amount_cents > 0U;
    fill_capacity_result(result, total_spaces, occupied_spaces,
                         pending_entries);
    if (commit_transaction(store) != 0)
        goto fail_after_commit;
    return 0;

fail:
    rollback_silently(store);
    return -1;

fail_after_commit:
    rollback_silently(store);
    return -1;
}

static int read_operation(parking_store_t *store,
                          uint64_t operation_id,
                          int event_type,
                          const char *plate_number,
                          int *state,
                          uint64_t *session_id,
                          uint64_t *requested_at_unix_us,
                          uint64_t *amount_cents)
{
    static const char sql[] =
        "SELECT state, COALESCE(session_id, 0), requested_at_unix_us, "
        "amount_cents FROM parking_events "
        "WHERE id = ? AND event_type = ? AND plate_number = ?";
    sqlite3_stmt *statement = NULL;
    int step_result;
    int64_t stored_session_id;
    int64_t requested_at;
    int64_t amount;

    if (prepare_statement(store, sql, &statement,
                          "准备读取停车操作失败") != 0)
        return -1;

    if (bind_uint64(store, statement, 1, operation_id,
                    "绑定 operation_id 失败") != 0 ||
        store->api.bind_int(statement, 2, event_type) != SQLITE_OK ||
        bind_text(store, statement, 3, plate_number,
                  "绑定停车操作车牌失败") != 0) {
        set_database_error(store, "绑定停车操作参数失败");
        store->api.finalize(statement);
        return -1;
    }

    step_result = store->api.step(statement);
    if (step_result == SQLITE_DONE) {
        set_error(store, "operation_id=%" PRIu64 " 不存在或与车牌不匹配",
                  operation_id);
        store->api.finalize(statement);
        return -1;
    }
    if (step_result != SQLITE_ROW) {
        set_database_error(store, "读取停车操作失败");
        store->api.finalize(statement);
        return -1;
    }

    *state = store->api.column_int(statement, 0);
    stored_session_id = store->api.column_int64(statement, 1);
    requested_at = store->api.column_int64(statement, 2);
    amount = store->api.column_int64(statement, 3);
    store->api.finalize(statement);

    if (stored_session_id < 0 || requested_at <= 0 || amount < 0) {
        set_error(store, "operation_id=%" PRIu64 " 的记录无效",
                  operation_id);
        return -1;
    }

    *session_id = (uint64_t)stored_session_id;
    *requested_at_unix_us = (uint64_t)requested_at;
    *amount_cents = (uint64_t)amount;
    return 0;
}

int parking_store_complete_entry(parking_store_t *store,
                                 uint64_t operation_id,
                                 const char *plate_number,
                                 uint64_t completed_at_unix_us)
{
    static const char insert_session_sql[] =
        "INSERT INTO parking_sessions("
        "plate_number, entry_time_unix_us, fee_cents, status, "
        "entry_operation_id) VALUES(?, ?, 0, 0, ?)";
    static const char update_space_sql[] =
        "UPDATE parking_spaces SET occupied_spaces = occupied_spaces + 1, "
        "updated_at_unix_us = ? "
        "WHERE id = 1 AND occupied_spaces < total_spaces";
    static const char update_event_sql[] =
        "UPDATE parking_events SET state = 1, session_id = ?, "
        "completed_at_unix_us = ? WHERE id = ? AND state = 0";
    sqlite3_stmt *statement = NULL;
    uint64_t session_id;
    uint64_t stored_session_id;
    uint64_t requested_at;
    uint64_t amount_cents;
    int state;

    if (store == NULL || operation_id == 0U ||
        validate_plate(plate_number) != 0 ||
        validate_timestamp(completed_at_unix_us) != 0)
        return -1;

    store->last_error[0] = '\0';
    if (begin_transaction(store) != 0)
        return -1;
    if (read_operation(store, operation_id, STORE_EVENT_ENTRY,
                       plate_number, &state, &stored_session_id,
                       &requested_at, &amount_cents) != 0)
        goto fail;

    if (state == STORE_EVENT_COMMITTED) {
        if (commit_transaction(store) != 0)
            goto fail_after_commit;
        return 0;
    }
    if (state != STORE_EVENT_PENDING) {
        set_error(store, "入场 operation_id=%" PRIu64 " 不是待完成状态",
                  operation_id);
        goto fail;
    }

    if (prepare_statement(store, insert_session_sql, &statement,
                          "准备创建停车会话失败") != 0)
        goto fail;
    if (bind_text(store, statement, 1, plate_number,
                  "绑定入场车牌失败") != 0 ||
        bind_uint64(store, statement, 2, requested_at,
                    "绑定入场时间失败") != 0 ||
        bind_uint64(store, statement, 3, operation_id,
                    "绑定入场 operation_id 失败") != 0 ||
        store->api.step(statement) != SQLITE_DONE) {
        set_database_error(store, "创建停车会话失败");
        goto fail;
    }
    store->api.finalize(statement);
    statement = NULL;

    session_id = (uint64_t)store->api.last_insert_rowid(store->database);
    if (session_id == 0U) {
        set_error(store, "创建停车会话后未取得 session_id");
        goto fail;
    }

    if (prepare_statement(store, update_space_sql, &statement,
                          "准备更新已占用车位失败") != 0)
        goto fail;
    if (bind_uint64(store, statement, 1, completed_at_unix_us,
                    "绑定车位更新时间失败") != 0 ||
        store->api.step(statement) != SQLITE_DONE ||
        store->api.changes(store->database) != 1) {
        set_database_error(store, "更新已占用车位失败");
        goto fail;
    }
    store->api.finalize(statement);
    statement = NULL;

    if (prepare_statement(store, update_event_sql, &statement,
                          "准备完成入场事件失败") != 0)
        goto fail;
    if (bind_uint64(store, statement, 1, session_id,
                    "绑定入场 session_id 失败") != 0 ||
        bind_uint64(store, statement, 2, completed_at_unix_us,
                    "绑定入场完成时间失败") != 0 ||
        bind_uint64(store, statement, 3, operation_id,
                    "绑定入场事件 ID 失败") != 0 ||
        store->api.step(statement) != SQLITE_DONE ||
        store->api.changes(store->database) != 1) {
        set_database_error(store, "完成入场事件失败");
        goto fail;
    }
    store->api.finalize(statement);
    statement = NULL;

    if (commit_transaction(store) != 0)
        goto fail_after_commit;
    return 0;

fail:
    if (statement != NULL)
        store->api.finalize(statement);
    rollback_silently(store);
    return -1;

fail_after_commit:
    if (statement != NULL)
        store->api.finalize(statement);
    rollback_silently(store);
    return -1;
}

int parking_store_complete_exit(parking_store_t *store,
                                uint64_t operation_id,
                                const char *plate_number,
                                uint64_t completed_at_unix_us)
{
    static const char update_session_sql[] =
        "UPDATE parking_sessions SET exit_time_unix_us = ?, "
        "fee_cents = ?, status = 1, exit_operation_id = ? "
        "WHERE id = ? AND plate_number = ? AND status = 0";
    static const char update_space_sql[] =
        "UPDATE parking_spaces SET occupied_spaces = occupied_spaces - 1, "
        "updated_at_unix_us = ? "
        "WHERE id = 1 AND occupied_spaces > 0";
    static const char update_event_sql[] =
        "UPDATE parking_events SET state = 1, completed_at_unix_us = ? "
        "WHERE id = ? AND state = 0";
    sqlite3_stmt *statement = NULL;
    uint64_t session_id;
    uint64_t requested_at;
    uint64_t amount_cents;
    int state;

    if (store == NULL || operation_id == 0U ||
        validate_plate(plate_number) != 0 ||
        validate_timestamp(completed_at_unix_us) != 0)
        return -1;

    store->last_error[0] = '\0';
    if (begin_transaction(store) != 0)
        return -1;
    if (read_operation(store, operation_id, STORE_EVENT_EXIT,
                       plate_number, &state, &session_id,
                       &requested_at, &amount_cents) != 0)
        goto fail;

    if (state == STORE_EVENT_COMMITTED) {
        if (commit_transaction(store) != 0)
            goto fail_after_commit;
        return 0;
    }
    if (state != STORE_EVENT_PENDING || session_id == 0U) {
        set_error(store, "出场 operation_id=%" PRIu64 " 不是有效待完成状态",
                  operation_id);
        goto fail;
    }

    if (prepare_statement(store, update_session_sql, &statement,
                          "准备完成停车会话失败") != 0)
        goto fail;
    if (bind_uint64(store, statement, 1, completed_at_unix_us,
                    "绑定出场时间失败") != 0 ||
        bind_uint64(store, statement, 2, amount_cents,
                    "绑定停车费用失败") != 0 ||
        bind_uint64(store, statement, 3, operation_id,
                    "绑定出场 operation_id 失败") != 0 ||
        bind_uint64(store, statement, 4, session_id,
                    "绑定出场 session_id 失败") != 0 ||
        bind_text(store, statement, 5, plate_number,
                  "绑定出场车牌失败") != 0 ||
        store->api.step(statement) != SQLITE_DONE ||
        store->api.changes(store->database) != 1) {
        set_database_error(store, "完成停车会话失败");
        goto fail;
    }
    store->api.finalize(statement);
    statement = NULL;

    if (prepare_statement(store, update_space_sql, &statement,
                          "准备释放车位失败") != 0)
        goto fail;
    if (bind_uint64(store, statement, 1, completed_at_unix_us,
                    "绑定车位释放时间失败") != 0 ||
        store->api.step(statement) != SQLITE_DONE ||
        store->api.changes(store->database) != 1) {
        set_database_error(store, "释放车位失败");
        goto fail;
    }
    store->api.finalize(statement);
    statement = NULL;

    if (prepare_statement(store, update_event_sql, &statement,
                          "准备完成出场事件失败") != 0)
        goto fail;
    if (bind_uint64(store, statement, 1, completed_at_unix_us,
                    "绑定出场完成时间失败") != 0 ||
        bind_uint64(store, statement, 2, operation_id,
                    "绑定出场事件 ID 失败") != 0 ||
        store->api.step(statement) != SQLITE_DONE ||
        store->api.changes(store->database) != 1) {
        set_database_error(store, "完成出场事件失败");
        goto fail;
    }
    store->api.finalize(statement);
    statement = NULL;

    if (commit_transaction(store) != 0)
        goto fail_after_commit;
    return 0;

fail:
    if (statement != NULL)
        store->api.finalize(statement);
    rollback_silently(store);
    return -1;

fail_after_commit:
    if (statement != NULL)
        store->api.finalize(statement);
    rollback_silently(store);
    return -1;
}

int parking_store_get_status(parking_store_t *store,
                             parking_store_status_t *status)
{
    sqlite3_stmt *statement = NULL;
    int step_result;
    int64_t active_sessions;

    if (store == NULL || status == NULL)
        return -1;

    memset(status, 0, sizeof(*status));
    store->last_error[0] = '\0';
    if (read_capacity(store, &status->total_spaces,
                      &status->occupied_spaces,
                      &status->pending_entry_operations) != 0)
        return -1;

    if (prepare_statement(store,
                          "SELECT COUNT(*) FROM parking_sessions "
                          "WHERE status = 0",
                          &statement,
                          "准备统计在场车辆失败") != 0)
        return -1;
    step_result = store->api.step(statement);
    if (step_result != SQLITE_ROW) {
        set_database_error(store, "统计在场车辆失败");
        store->api.finalize(statement);
        return -1;
    }
    active_sessions = store->api.column_int64(statement, 0);
    store->api.finalize(statement);

    if (active_sessions < 0) {
        set_error(store, "在场车辆统计无效");
        return -1;
    }

    status->schema_version = PARKING_STORE_SCHEMA_VERSION;
    status->active_sessions = (uint64_t)active_sessions;
    status->available_spaces =
        (uint64_t)status->occupied_spaces +
                status->pending_entry_operations < status->total_spaces
        ? status->total_spaces - status->occupied_spaces -
              status->pending_entry_operations
        : 0U;
    return 0;
}

int parking_store_copy_last_error(parking_store_t *store,
                                  char *buffer,
                                  size_t capacity)
{
    if (store == NULL || buffer == NULL || capacity == 0U)
        return -1;

    snprintf(buffer, capacity, "%s", store->last_error);
    return 0;
}

const char *parking_store_reason_name(parking_store_reason_t reason)
{
    switch (reason) {
        case PARKING_STORE_REASON_NONE:
            return "NONE";
        case PARKING_STORE_REASON_ALREADY_INSIDE:
            return "ALREADY_INSIDE";
        case PARKING_STORE_REASON_LOT_FULL:
            return "LOT_FULL";
        case PARKING_STORE_REASON_NO_ACTIVE_SESSION:
            return "NO_ACTIVE_SESSION";
        default:
            return "UNKNOWN";
    }
}

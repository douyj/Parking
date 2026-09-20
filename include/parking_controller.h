#ifndef PARKING_CONTROLLER_H
#define PARKING_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARKING_PLATE_CAPACITY 32U
#define PARKING_CONTROLLER_MAX_COMMANDS 2U

#define PARKING_DEFAULT_CHECK_TIMEOUT_US       5000000ULL
#define PARKING_DEFAULT_PAYMENT_TIMEOUT_US    30000000ULL
#define PARKING_DEFAULT_GATE_TIMEOUT_US         3000000ULL
#define PARKING_DEFAULT_PASSAGE_TIMEOUT_US     10000000ULL
#define PARKING_DEFAULT_COMMIT_TIMEOUT_US        5000000ULL
#define PARKING_DEFAULT_REJECT_COOLDOWN_US       3000000ULL

typedef struct parking_controller parking_controller_t;

typedef enum {
    PARKING_LANE_ENTRY = 0,
    PARKING_LANE_EXIT = 1
} parking_lane_mode_t;

typedef enum {
    PARKING_STATE_INITIALIZING = 0,
    PARKING_STATE_IDLE,
    PARKING_STATE_CHECKING,
    PARKING_STATE_WAIT_PAYMENT,
    PARKING_STATE_OPENING,
    PARKING_STATE_WAIT_PASSAGE,
    PARKING_STATE_CLOSING,
    PARKING_STATE_COMPLETING,
    PARKING_STATE_REJECT_COOLDOWN,
    PARKING_STATE_ERROR
} parking_state_t;

typedef enum {
    PARKING_PASSAGE_NONE = 0,
    PARKING_PASSAGE_SENSOR,
    PARKING_PASSAGE_MANUAL,
    PARKING_PASSAGE_VISION_TIMEOUT
} parking_passage_source_t;

typedef enum {
    PARKING_REASON_NONE = 0,
    PARKING_REASON_INVALID_PLATE,
    PARKING_REASON_AMBIGUOUS_PLATE,
    PARKING_REASON_LANE_BUSY,
    PARKING_REASON_LANE_UNAVAILABLE,
    PARKING_REASON_ALREADY_INSIDE,
    PARKING_REASON_LOT_FULL,
    PARKING_REASON_NO_ACTIVE_SESSION,
    PARKING_REASON_CHECK_REJECTED,
    PARKING_REASON_CHECK_TIMEOUT,
    PARKING_REASON_PAYMENT_FAILED,
    PARKING_REASON_PAYMENT_TIMEOUT,
    PARKING_REASON_GATE_FAILURE,
    PARKING_REASON_GATE_TIMEOUT,
    PARKING_REASON_COMMIT_FAILURE,
    PARKING_REASON_COMMIT_TIMEOUT,
    PARKING_REASON_UNEXPECTED_EVENT,
    PARKING_REASON_INTERNAL_ERROR
} parking_reason_t;

typedef enum {
    PARKING_EVENT_PLATE_CONFIRMED = 0,
    PARKING_EVENT_CHECK_APPROVED,
    PARKING_EVENT_CHECK_REJECTED,
    PARKING_EVENT_PAYMENT_SUCCEEDED,
    PARKING_EVENT_PAYMENT_FAILED,
    PARKING_EVENT_GATE_OPENED,
    PARKING_EVENT_GATE_CLOSED,
    PARKING_EVENT_PASSAGE_CONFIRMED,
    PARKING_EVENT_GATE_FAILED,
    PARKING_EVENT_COMMIT_SUCCEEDED,
    PARKING_EVENT_COMMIT_FAILED,
    PARKING_EVENT_RESET
} parking_event_type_t;

typedef enum {
    PARKING_COMMAND_CHECK_ENTRY = 0,
    PARKING_COMMAND_CHECK_EXIT,
    PARKING_COMMAND_REQUEST_PAYMENT,
    PARKING_COMMAND_OPEN_GATE,
    PARKING_COMMAND_CLOSE_GATE,
    PARKING_COMMAND_COMPLETE_ENTRY,
    PARKING_COMMAND_COMPLETE_EXIT,
    PARKING_COMMAND_REJECT,
    PARKING_COMMAND_REPORT_ERROR
} parking_command_type_t;

typedef struct {
    parking_lane_mode_t mode;
    int gate_initially_closed;

    /* 配置为 0 时使用上面的默认值。 */
    uint64_t check_timeout_us;
    uint64_t payment_timeout_us;
    uint64_t gate_timeout_us;
    uint64_t passage_timeout_us;
    uint64_t commit_timeout_us;
    uint64_t reject_cooldown_us;
} parking_controller_config_t;

typedef struct {
    parking_event_type_t type;
    uint64_t occurred_at_us;

    union {
        struct {
            char plate_number[PARKING_PLATE_CAPACITY];
            float confidence;
        } plate;

        struct {
            uint64_t operation_id;
            uint64_t amount_cents;
            int payment_required;
        } check;

        struct {
            parking_reason_t reason;
        } rejection;

        struct {
            parking_passage_source_t source;
        } passage;

        struct {
            parking_reason_t reason;
        } failure;
    } data;
} parking_event_t;

typedef struct {
    parking_command_type_t type;
    parking_lane_mode_t mode;
    parking_reason_t reason;
    parking_passage_source_t passage_source;

    char plate_number[PARKING_PLATE_CAPACITY];
    float confidence;
    uint64_t confirmed_at_us;
    uint64_t operation_id;
    uint64_t amount_cents;
} parking_command_t;

typedef struct {
    parking_command_t items[PARKING_CONTROLLER_MAX_COMMANDS];
    size_t count;
} parking_command_list_t;

/* 控制器由主线程驱动，本身不创建线程，也不直接访问硬件或数据库。 */
parking_controller_t *parking_controller_create(
    const parking_controller_config_t *config);

void parking_controller_destroy(parking_controller_t *controller);

/* 只有 IDLE 且已经确认 Gate CLOSED 时才允许切换模式。 */
int parking_controller_set_mode(parking_controller_t *controller,
                                parking_lane_mode_t mode);

int parking_controller_handle_event(parking_controller_t *controller,
                                    const parking_event_t *event,
                                    parking_command_list_t *commands);

/*
 * 周期调用，用于产生检查、支付、Gate、通行和提交超时事件。
 * last_plate_seen_us 来自 plate_pipeline_last_plate_seen_us()；仅在
 * WAIT_PASSAGE 状态用于判断“连续多久没有看到车牌”。
 */
int parking_controller_tick(parking_controller_t *controller,
                            uint64_t now_us,
                            uint64_t last_plate_seen_us,
                            parking_command_list_t *commands);

parking_state_t parking_controller_state(
    const parking_controller_t *controller);

parking_lane_mode_t parking_controller_mode(
    const parking_controller_t *controller);

int parking_controller_copy_active_plate(
    const parking_controller_t *controller,
    char *buffer,
    size_t capacity);

int parking_controller_copy_last_error(
    const parking_controller_t *controller,
    char *buffer,
    size_t capacity);

const char *parking_controller_state_name(parking_state_t state);
const char *parking_controller_command_name(parking_command_type_t command);
const char *parking_controller_reason_name(parking_reason_t reason);
const char *parking_controller_passage_source_name(
    parking_passage_source_t source);

#ifdef __cplusplus
}
#endif

#endif

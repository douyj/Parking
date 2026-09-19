#include "parking_controller.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARKING_ERROR_CAPACITY 256U

typedef enum {
    CONTROLLER_GATE_UNKNOWN = 0,
    CONTROLLER_GATE_CLOSED,
    CONTROLLER_GATE_OPENING,
    CONTROLLER_GATE_OPEN,
    CONTROLLER_GATE_CLOSING,
    CONTROLLER_GATE_ERROR
} controller_gate_state_t;

struct parking_controller {
    parking_controller_config_t config;
    parking_state_t state;
    controller_gate_state_t gate_state;

    uint64_t state_started_us;
    uint64_t operation_id;
    uint64_t amount_cents;
    uint64_t plate_confirmed_at_us;

    float confidence;
    parking_passage_source_t passage_source;
    char plate_number[PARKING_PLATE_CAPACITY];
    char last_error[PARKING_ERROR_CAPACITY];
};

static int valid_mode(parking_lane_mode_t mode)
{
    return mode == PARKING_LANE_ENTRY || mode == PARKING_LANE_EXIT;
}

static uint64_t configured_timeout(uint64_t value, uint64_t default_value)
{
    return value != 0 ? value : default_value;
}

static void set_error(parking_controller_t *controller,
                      const char *format, ...)
{
    va_list arguments;

    if (controller == NULL || format == NULL)
        return;

    va_start(arguments, format);
    vsnprintf(controller->last_error, sizeof(controller->last_error),
              format, arguments);
    va_end(arguments);
}

static void clear_active_operation(parking_controller_t *controller)
{
    controller->operation_id = 0;
    controller->amount_cents = 0;
    controller->plate_confirmed_at_us = 0;
    controller->confidence = 0.0F;
    controller->passage_source = PARKING_PASSAGE_NONE;
    memset(controller->plate_number, 0, sizeof(controller->plate_number));
}

static void enter_state(parking_controller_t *controller,
                        parking_state_t state,
                        uint64_t now_us)
{
    controller->state = state;
    controller->state_started_us = now_us;
}

static int plate_string_valid(const char plate[PARKING_PLATE_CAPACITY])
{
    return plate != NULL && plate[0] != '\0' &&
           memchr(plate, '\0', PARKING_PLATE_CAPACITY) != NULL;
}

static void copy_plate(char destination[PARKING_PLATE_CAPACITY],
                       const char *source)
{
    size_t length;

    length = strlen(source);
    if (length >= PARKING_PLATE_CAPACITY)
        length = PARKING_PLATE_CAPACITY - 1U;

    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void clear_commands(parking_command_list_t *commands)
{
    memset(commands, 0, sizeof(*commands));
}

static parking_command_t *append_command(
    parking_controller_t *controller,
    parking_command_list_t *commands,
    parking_command_type_t type,
    parking_reason_t reason)
{
    parking_command_t *command;

    if (commands->count >= PARKING_CONTROLLER_MAX_COMMANDS) {
        set_error(controller, "业务命令列表容量不足");
        return NULL;
    }

    command = &commands->items[commands->count++];
    memset(command, 0, sizeof(*command));
    command->type = type;
    command->mode = controller->config.mode;
    command->reason = reason;
    command->passage_source = controller->passage_source;
    command->confidence = controller->confidence;
    command->confirmed_at_us = controller->plate_confirmed_at_us;
    command->operation_id = controller->operation_id;
    command->amount_cents = controller->amount_cents;
    copy_plate(command->plate_number, controller->plate_number);

    return command;
}

static int emit_rejection(parking_controller_t *controller,
                          parking_command_list_t *commands,
                          const char *plate_number,
                          parking_reason_t reason)
{
    parking_command_t *command;

    command = append_command(controller, commands,
                             PARKING_COMMAND_REJECT, reason);
    if (command == NULL)
        return -1;

    if (plate_number != NULL)
        copy_plate(command->plate_number, plate_number);

    return 0;
}

static int enter_error_state(parking_controller_t *controller,
                             parking_command_list_t *commands,
                             parking_reason_t reason,
                             uint64_t now_us,
                             const char *message)
{
    if (message != NULL)
        set_error(controller, "%s", message);

    enter_state(controller, PARKING_STATE_ERROR, now_us);
    if (append_command(controller, commands,
                       PARKING_COMMAND_REPORT_ERROR, reason) == NULL)
        return -1;

    return 0;
}

static int begin_gate_opening(parking_controller_t *controller,
                              parking_command_list_t *commands,
                              uint64_t now_us)
{
    controller->gate_state = CONTROLLER_GATE_OPENING;
    enter_state(controller, PARKING_STATE_OPENING, now_us);

    return append_command(controller, commands,
                          PARKING_COMMAND_OPEN_GATE,
                          PARKING_REASON_NONE) != NULL ? 0 : -1;
}

static int begin_gate_closing(parking_controller_t *controller,
                              parking_command_list_t *commands,
                              uint64_t now_us,
                              parking_passage_source_t source)
{
    controller->passage_source = source;
    controller->gate_state = CONTROLLER_GATE_CLOSING;
    enter_state(controller, PARKING_STATE_CLOSING, now_us);

    return append_command(controller, commands,
                          PARKING_COMMAND_CLOSE_GATE,
                          PARKING_REASON_NONE) != NULL ? 0 : -1;
}

static int begin_reject_cooldown(parking_controller_t *controller,
                                 parking_command_list_t *commands,
                                 uint64_t now_us,
                                 parking_reason_t reason)
{
    enter_state(controller, PARKING_STATE_REJECT_COOLDOWN, now_us);
    return emit_rejection(controller, commands,
                          controller->plate_number, reason);
}

static int handle_plate_event(parking_controller_t *controller,
                              const parking_event_t *event,
                              parking_command_list_t *commands)
{
    parking_command_type_t command_type;

    if (!plate_string_valid(event->data.plate.plate_number)) {
        if (controller->state == PARKING_STATE_IDLE) {
            enter_state(controller, PARKING_STATE_REJECT_COOLDOWN,
                        event->occurred_at_us);
        }
        return emit_rejection(controller, commands, "",
                              PARKING_REASON_INVALID_PLATE);
    }

    if (controller->state != PARKING_STATE_IDLE) {
        if (controller->plate_number[0] != '\0' &&
            strcmp(controller->plate_number,
                   event->data.plate.plate_number) == 0)
            return 0;

        return emit_rejection(controller, commands,
                              event->data.plate.plate_number,
                              controller->state == PARKING_STATE_ERROR ||
                              controller->state == PARKING_STATE_INITIALIZING
                                  ? PARKING_REASON_LANE_UNAVAILABLE
                                  : PARKING_REASON_LANE_BUSY);
    }

    if (controller->gate_state != CONTROLLER_GATE_CLOSED) {
        return emit_rejection(controller, commands,
                              event->data.plate.plate_number,
                              PARKING_REASON_LANE_UNAVAILABLE);
    }

    copy_plate(controller->plate_number,
               event->data.plate.plate_number);
    controller->confidence = event->data.plate.confidence;
    controller->plate_confirmed_at_us = event->occurred_at_us;
    controller->operation_id = 0;
    controller->amount_cents = 0;
    controller->passage_source = PARKING_PASSAGE_NONE;
    controller->last_error[0] = '\0';
    enter_state(controller, PARKING_STATE_CHECKING,
                event->occurred_at_us);

    command_type = controller->config.mode == PARKING_LANE_ENTRY
        ? PARKING_COMMAND_CHECK_ENTRY
        : PARKING_COMMAND_CHECK_EXIT;

    return append_command(controller, commands, command_type,
                          PARKING_REASON_NONE) != NULL ? 0 : -1;
}

static int handle_check_approved(parking_controller_t *controller,
                                 const parking_event_t *event,
                                 parking_command_list_t *commands)
{
    if (controller->state != PARKING_STATE_CHECKING)
        return enter_error_state(controller, commands,
                                 PARKING_REASON_UNEXPECTED_EVENT,
                                 event->occurred_at_us,
                                 "非 CHECKING 状态收到检查通过事件");

    if (event->data.check.payment_required != 0 &&
        event->data.check.payment_required != 1)
        return -1;

    controller->operation_id = event->data.check.operation_id;
    controller->amount_cents = event->data.check.amount_cents;

    if (controller->config.mode == PARKING_LANE_EXIT &&
        event->data.check.payment_required) {
        enter_state(controller, PARKING_STATE_WAIT_PAYMENT,
                    event->occurred_at_us);
        return append_command(controller, commands,
                              PARKING_COMMAND_REQUEST_PAYMENT,
                              PARKING_REASON_NONE) != NULL ? 0 : -1;
    }

    return begin_gate_opening(controller, commands,
                              event->occurred_at_us);
}

static int handle_gate_closed(parking_controller_t *controller,
                              const parking_event_t *event,
                              parking_command_list_t *commands)
{
    parking_command_type_t command_type;

    controller->gate_state = CONTROLLER_GATE_CLOSED;

    if (controller->state == PARKING_STATE_INITIALIZING) {
        enter_state(controller, PARKING_STATE_IDLE,
                    event->occurred_at_us);
        return 0;
    }

    if (controller->state == PARKING_STATE_CLOSING) {
        enter_state(controller, PARKING_STATE_COMPLETING,
                    event->occurred_at_us);
        command_type = controller->config.mode == PARKING_LANE_ENTRY
            ? PARKING_COMMAND_COMPLETE_ENTRY
            : PARKING_COMMAND_COMPLETE_EXIT;
        return append_command(controller, commands, command_type,
                              PARKING_REASON_NONE) != NULL ? 0 : -1;
    }

    if (controller->state == PARKING_STATE_IDLE ||
        controller->state == PARKING_STATE_CHECKING ||
        controller->state == PARKING_STATE_WAIT_PAYMENT ||
        controller->state == PARKING_STATE_COMPLETING ||
        controller->state == PARKING_STATE_REJECT_COOLDOWN ||
        controller->state == PARKING_STATE_ERROR)
        return 0;

    return enter_error_state(controller, commands,
                             PARKING_REASON_UNEXPECTED_EVENT,
                             event->occurred_at_us,
                             "业务流程中收到意外的 Gate CLOSED 事件");
}

parking_controller_t *parking_controller_create(
    const parking_controller_config_t *config)
{
    parking_controller_t *controller;

    if (config == NULL || !valid_mode(config->mode) ||
        (config->gate_initially_closed != 0 &&
         config->gate_initially_closed != 1))
        return NULL;

    controller = calloc(1, sizeof(*controller));
    if (controller == NULL)
        return NULL;

    controller->config = *config;
    controller->config.check_timeout_us = configured_timeout(
        config->check_timeout_us, PARKING_DEFAULT_CHECK_TIMEOUT_US);
    controller->config.payment_timeout_us = configured_timeout(
        config->payment_timeout_us, PARKING_DEFAULT_PAYMENT_TIMEOUT_US);
    controller->config.gate_timeout_us = configured_timeout(
        config->gate_timeout_us, PARKING_DEFAULT_GATE_TIMEOUT_US);
    controller->config.passage_timeout_us = configured_timeout(
        config->passage_timeout_us, PARKING_DEFAULT_PASSAGE_TIMEOUT_US);
    controller->config.commit_timeout_us = configured_timeout(
        config->commit_timeout_us, PARKING_DEFAULT_COMMIT_TIMEOUT_US);
    controller->config.reject_cooldown_us = configured_timeout(
        config->reject_cooldown_us, PARKING_DEFAULT_REJECT_COOLDOWN_US);

    if (config->gate_initially_closed) {
        controller->gate_state = CONTROLLER_GATE_CLOSED;
        controller->state = PARKING_STATE_IDLE;
    } else {
        controller->gate_state = CONTROLLER_GATE_UNKNOWN;
        controller->state = PARKING_STATE_INITIALIZING;
    }

    return controller;
}

void parking_controller_destroy(parking_controller_t *controller)
{
    free(controller);
}

int parking_controller_set_mode(parking_controller_t *controller,
                                parking_lane_mode_t mode)
{
    if (controller == NULL || !valid_mode(mode))
        return -1;

    if (controller->state != PARKING_STATE_IDLE ||
        controller->gate_state != CONTROLLER_GATE_CLOSED) {
        set_error(controller,
                  "只有 IDLE 且 Gate CLOSED 时才能切换车道模式");
        return -1;
    }

    controller->config.mode = mode;
    controller->last_error[0] = '\0';
    return 0;
}

int parking_controller_handle_event(parking_controller_t *controller,
                                    const parking_event_t *event,
                                    parking_command_list_t *commands)
{
    parking_reason_t reason;

    if (controller == NULL || event == NULL || commands == NULL)
        return -1;

    clear_commands(commands);

    switch (event->type) {
        case PARKING_EVENT_PLATE_CONFIRMED:
            return handle_plate_event(controller, event, commands);

        case PARKING_EVENT_CHECK_APPROVED:
            return handle_check_approved(controller, event, commands);

        case PARKING_EVENT_CHECK_REJECTED:
            if (controller->state != PARKING_STATE_CHECKING)
                return enter_error_state(
                    controller, commands, PARKING_REASON_UNEXPECTED_EVENT,
                    event->occurred_at_us,
                    "非 CHECKING 状态收到检查拒绝事件");
            reason = event->data.rejection.reason;
            if (reason == PARKING_REASON_NONE)
                reason = PARKING_REASON_CHECK_REJECTED;
            return begin_reject_cooldown(controller, commands,
                                         event->occurred_at_us, reason);

        case PARKING_EVENT_PAYMENT_SUCCEEDED:
            if (controller->state != PARKING_STATE_WAIT_PAYMENT)
                return enter_error_state(
                    controller, commands, PARKING_REASON_UNEXPECTED_EVENT,
                    event->occurred_at_us,
                    "非 WAIT_PAYMENT 状态收到支付成功事件");
            return begin_gate_opening(controller, commands,
                                      event->occurred_at_us);

        case PARKING_EVENT_PAYMENT_FAILED:
            if (controller->state != PARKING_STATE_WAIT_PAYMENT)
                return enter_error_state(
                    controller, commands, PARKING_REASON_UNEXPECTED_EVENT,
                    event->occurred_at_us,
                    "非 WAIT_PAYMENT 状态收到支付失败事件");
            return begin_reject_cooldown(
                controller, commands, event->occurred_at_us,
                PARKING_REASON_PAYMENT_FAILED);

        case PARKING_EVENT_GATE_OPENED:
            if (controller->state == PARKING_STATE_WAIT_PASSAGE)
                return 0;
            if (controller->state != PARKING_STATE_OPENING)
                return enter_error_state(
                    controller, commands, PARKING_REASON_UNEXPECTED_EVENT,
                    event->occurred_at_us,
                    "非 OPENING 状态收到 Gate OPENED 事件");
            controller->gate_state = CONTROLLER_GATE_OPEN;
            enter_state(controller, PARKING_STATE_WAIT_PASSAGE,
                        event->occurred_at_us);
            return 0;

        case PARKING_EVENT_GATE_CLOSED:
            return handle_gate_closed(controller, event, commands);

        case PARKING_EVENT_PASSAGE_CONFIRMED:
            if (controller->state != PARKING_STATE_WAIT_PASSAGE)
                return enter_error_state(
                    controller, commands, PARKING_REASON_UNEXPECTED_EVENT,
                    event->occurred_at_us,
                    "非 WAIT_PASSAGE 状态收到车辆通过事件");
            if (event->data.passage.source != PARKING_PASSAGE_SENSOR &&
                event->data.passage.source != PARKING_PASSAGE_MANUAL)
                return -1;
            return begin_gate_closing(
                controller, commands, event->occurred_at_us,
                event->data.passage.source);

        case PARKING_EVENT_GATE_FAILED:
            controller->gate_state = CONTROLLER_GATE_ERROR;
            reason = event->data.failure.reason;
            if (reason == PARKING_REASON_NONE)
                reason = PARKING_REASON_GATE_FAILURE;
            return enter_error_state(controller, commands, reason,
                                     event->occurred_at_us,
                                     "Gate 执行失败");

        case PARKING_EVENT_COMMIT_SUCCEEDED:
            if (controller->state != PARKING_STATE_COMPLETING)
                return enter_error_state(
                    controller, commands, PARKING_REASON_UNEXPECTED_EVENT,
                    event->occurred_at_us,
                    "非 COMPLETING 状态收到提交成功事件");
            clear_active_operation(controller);
            enter_state(controller, PARKING_STATE_IDLE,
                        event->occurred_at_us);
            return 0;

        case PARKING_EVENT_COMMIT_FAILED:
            if (controller->state != PARKING_STATE_COMPLETING)
                return enter_error_state(
                    controller, commands, PARKING_REASON_UNEXPECTED_EVENT,
                    event->occurred_at_us,
                    "非 COMPLETING 状态收到提交失败事件");
            return enter_error_state(controller, commands,
                                     PARKING_REASON_COMMIT_FAILURE,
                                     event->occurred_at_us,
                                     "停车业务结果提交失败");

        case PARKING_EVENT_RESET:
            if (controller->state != PARKING_STATE_ERROR ||
                controller->gate_state != CONTROLLER_GATE_CLOSED) {
                set_error(controller,
                          "只有 ERROR 且 Gate CLOSED 时才能复位");
                return -1;
            }
            clear_active_operation(controller);
            controller->last_error[0] = '\0';
            enter_state(controller, PARKING_STATE_IDLE,
                        event->occurred_at_us);
            return 0;

        default:
            set_error(controller, "未知停车业务事件: %d",
                      (int)event->type);
            return -1;
    }
}

static int timeout_elapsed(const parking_controller_t *controller,
                           uint64_t now_us,
                           uint64_t timeout_us)
{
    return now_us >= controller->state_started_us &&
           now_us - controller->state_started_us >= timeout_us;
}

int parking_controller_tick(parking_controller_t *controller,
                            uint64_t now_us,
                            uint64_t last_plate_seen_us,
                            parking_command_list_t *commands)
{
    uint64_t passage_reference_us;

    if (controller == NULL || commands == NULL)
        return -1;

    clear_commands(commands);
    if (now_us < controller->state_started_us) {
        set_error(controller, "单调时钟发生倒退");
        return -1;
    }

    switch (controller->state) {
        case PARKING_STATE_CHECKING:
            if (timeout_elapsed(controller, now_us,
                                controller->config.check_timeout_us)) {
                return enter_error_state(
                    controller, commands, PARKING_REASON_CHECK_TIMEOUT,
                    now_us, "停车业务检查超时");
            }
            break;

        case PARKING_STATE_WAIT_PAYMENT:
            if (timeout_elapsed(controller, now_us,
                                controller->config.payment_timeout_us)) {
                return begin_reject_cooldown(
                    controller, commands, now_us,
                    PARKING_REASON_PAYMENT_TIMEOUT);
            }
            break;

        case PARKING_STATE_OPENING:
        case PARKING_STATE_CLOSING:
            if (timeout_elapsed(controller, now_us,
                                controller->config.gate_timeout_us)) {
                return enter_error_state(
                    controller, commands, PARKING_REASON_GATE_TIMEOUT,
                    now_us, "Gate 动作超时");
            }
            break;

        case PARKING_STATE_WAIT_PASSAGE:
            passage_reference_us = controller->state_started_us;
            if (last_plate_seen_us > passage_reference_us &&
                last_plate_seen_us <= now_us)
                passage_reference_us = last_plate_seen_us;

            if (now_us - passage_reference_us >=
                controller->config.passage_timeout_us) {
                return begin_gate_closing(
                    controller, commands, now_us,
                    PARKING_PASSAGE_VISION_TIMEOUT);
            }
            break;

        case PARKING_STATE_COMPLETING:
            if (timeout_elapsed(controller, now_us,
                                controller->config.commit_timeout_us)) {
                return enter_error_state(
                    controller, commands, PARKING_REASON_COMMIT_TIMEOUT,
                    now_us, "停车业务结果提交超时");
            }
            break;

        case PARKING_STATE_REJECT_COOLDOWN:
            if (timeout_elapsed(controller, now_us,
                                controller->config.reject_cooldown_us)) {
                clear_active_operation(controller);
                enter_state(controller, PARKING_STATE_IDLE, now_us);
            }
            break;

        default:
            break;
    }

    return 0;
}

parking_state_t parking_controller_state(
    const parking_controller_t *controller)
{
    return controller != NULL ? controller->state : PARKING_STATE_ERROR;
}

parking_lane_mode_t parking_controller_mode(
    const parking_controller_t *controller)
{
    return controller != NULL ? controller->config.mode
                              : PARKING_LANE_ENTRY;
}

int parking_controller_copy_active_plate(
    const parking_controller_t *controller,
    char *buffer,
    size_t capacity)
{
    if (controller == NULL || buffer == NULL || capacity == 0)
        return -1;

    snprintf(buffer, capacity, "%s", controller->plate_number);
    return 0;
}

int parking_controller_copy_last_error(
    const parking_controller_t *controller,
    char *buffer,
    size_t capacity)
{
    if (controller == NULL || buffer == NULL || capacity == 0)
        return -1;

    snprintf(buffer, capacity, "%s", controller->last_error);
    return 0;
}

const char *parking_controller_state_name(parking_state_t state)
{
    switch (state) {
        case PARKING_STATE_INITIALIZING:
            return "INITIALIZING";
        case PARKING_STATE_IDLE:
            return "IDLE";
        case PARKING_STATE_CHECKING:
            return "CHECKING";
        case PARKING_STATE_WAIT_PAYMENT:
            return "WAIT_PAYMENT";
        case PARKING_STATE_OPENING:
            return "OPENING";
        case PARKING_STATE_WAIT_PASSAGE:
            return "WAIT_PASSAGE";
        case PARKING_STATE_CLOSING:
            return "CLOSING";
        case PARKING_STATE_COMPLETING:
            return "COMPLETING";
        case PARKING_STATE_REJECT_COOLDOWN:
            return "REJECT_COOLDOWN";
        case PARKING_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

const char *parking_controller_command_name(parking_command_type_t command)
{
    switch (command) {
        case PARKING_COMMAND_CHECK_ENTRY:
            return "CHECK_ENTRY";
        case PARKING_COMMAND_CHECK_EXIT:
            return "CHECK_EXIT";
        case PARKING_COMMAND_REQUEST_PAYMENT:
            return "REQUEST_PAYMENT";
        case PARKING_COMMAND_OPEN_GATE:
            return "OPEN_GATE";
        case PARKING_COMMAND_CLOSE_GATE:
            return "CLOSE_GATE";
        case PARKING_COMMAND_COMPLETE_ENTRY:
            return "COMPLETE_ENTRY";
        case PARKING_COMMAND_COMPLETE_EXIT:
            return "COMPLETE_EXIT";
        case PARKING_COMMAND_REJECT:
            return "REJECT";
        case PARKING_COMMAND_REPORT_ERROR:
            return "REPORT_ERROR";
        default:
            return "UNKNOWN";
    }
}

const char *parking_controller_reason_name(parking_reason_t reason)
{
    switch (reason) {
        case PARKING_REASON_NONE:
            return "NONE";
        case PARKING_REASON_INVALID_PLATE:
            return "INVALID_PLATE";
        case PARKING_REASON_LANE_BUSY:
            return "LANE_BUSY";
        case PARKING_REASON_LANE_UNAVAILABLE:
            return "LANE_UNAVAILABLE";
        case PARKING_REASON_ALREADY_INSIDE:
            return "ALREADY_INSIDE";
        case PARKING_REASON_LOT_FULL:
            return "LOT_FULL";
        case PARKING_REASON_NO_ACTIVE_SESSION:
            return "NO_ACTIVE_SESSION";
        case PARKING_REASON_CHECK_REJECTED:
            return "CHECK_REJECTED";
        case PARKING_REASON_CHECK_TIMEOUT:
            return "CHECK_TIMEOUT";
        case PARKING_REASON_PAYMENT_FAILED:
            return "PAYMENT_FAILED";
        case PARKING_REASON_PAYMENT_TIMEOUT:
            return "PAYMENT_TIMEOUT";
        case PARKING_REASON_GATE_FAILURE:
            return "GATE_FAILURE";
        case PARKING_REASON_GATE_TIMEOUT:
            return "GATE_TIMEOUT";
        case PARKING_REASON_COMMIT_FAILURE:
            return "COMMIT_FAILURE";
        case PARKING_REASON_COMMIT_TIMEOUT:
            return "COMMIT_TIMEOUT";
        case PARKING_REASON_UNEXPECTED_EVENT:
            return "UNEXPECTED_EVENT";
        case PARKING_REASON_INTERNAL_ERROR:
            return "INTERNAL_ERROR";
        default:
            return "UNKNOWN";
    }
}

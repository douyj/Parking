#include "gate.h"
#include "log.h"
#include "parking_controller.h"
#include "parking_fee.h"
#include "parking_store.h"
#include "parking_ui.h"
#include "plate_pipeline.h"

#include <errno.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CAMERA_DEVICE           "/dev/video0"
#define DRM_DEVICE              "/dev/dri/card0"

#define CAMERA_WIDTH            1280U
#define CAMERA_HEIGHT           720U
#define CAMERA_FPS              30U

#define DETECT_MODEL            "models/yolo26s-plate-detect-rk3576.rknn"
#define RECOGNITION_MODEL       "models/plate_rec_color-rk3576.rknn"
#define FONT_PATH               "fonts/platech.ttf"
#define DEFAULT_DATABASE_PATH   "parking.db"

#define GATE_PWMCHIP_PATH       "/sys/class/pwm/pwmchip2"
#define GATE_PWM_CHANNEL        0U
#define GATE_PERIOD_NS          20000000ULL
#define GATE_OPEN_PULSE_NS      2400000ULL
#define GATE_CLOSE_PULSE_NS     1400000ULL
#define GATE_MOVEMENT_TIME_MS   800U
#define GATE_HOLD_AFTER_MOVE    0

#define PIPELINE_WAIT_MS        100U
#define ERROR_MESSAGE_CAPACITY  256U
#define DEMO_MAX_DISPATCH_DEPTH 8U

typedef struct {
    parking_controller_t *controller;
    parking_fee_t *fee;
    parking_store_t *store;
    parking_ui_t *ui;
    gate_t *gate;
    gate_state_t observed_gate_state;
    parking_store_status_t store_status;
    parking_reason_t ui_reason;
    char last_plate[PARKING_PLATE_CAPACITY];
    char last_plate_color[32];
    float last_confidence;
    uint64_t amount_cents;
    int enable_gate;
} parking_demo_t;

static volatile sig_atomic_t running = 1;

static int monotonic_us(uint64_t *value)
{
    struct timespec now;

    if (value == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;

    *value = (uint64_t)now.tv_sec * 1000000ULL +
             (uint64_t)now.tv_nsec / 1000ULL;
    return 0;
}

static int realtime_us(uint64_t *value)
{
    struct timespec now;

    if (value == NULL || clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;

    *value = (uint64_t)now.tv_sec * 1000000ULL +
             (uint64_t)now.tv_nsec / 1000ULL;
    return 0;
}

static const char *lane_mode_name(parking_lane_mode_t mode)
{
    return mode == PARKING_LANE_EXIT ? "exit" : "entry";
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [--lane-mode entry|exit] [--database PATH] "
            "[--enable-gate]\n"
            "  默认模式       Gate 为 DRY-RUN\n"
            "  --enable-gate  直接驱动真实 Gate，不进行交互确认\n",
            program != NULL ? program : "parking");
}

static int parse_arguments(int argc, char **argv,
                           parking_lane_mode_t *mode,
                           const char **database_path,
                           int *enable_gate)
{
    int i;

    if (argv == NULL || mode == NULL || database_path == NULL ||
        enable_gate == NULL)
        return -1;

    *mode = PARKING_LANE_ENTRY;
    *database_path = DEFAULT_DATABASE_PATH;
    *enable_gate = 0;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }

        if (strcmp(argv[i], "--lane-mode") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return -1;
            }

            ++i;
            if (strcmp(argv[i], "entry") == 0)
                *mode = PARKING_LANE_ENTRY;
            else if (strcmp(argv[i], "exit") == 0)
                *mode = PARKING_LANE_EXIT;
            else {
                print_usage(argv[0]);
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--database") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                print_usage(argv[0]);
                return -1;
            }

            *database_path = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--enable-gate") == 0) {
            *enable_gate = 1;
            continue;
        }

        print_usage(argv[0]);
        return -1;
    }

    return 0;
}

static const char *gate_state_name(gate_state_t state)
{
    switch (state) {
        case GATE_STATE_CLOSED:
            return "CLOSED";
        case GATE_STATE_OPENING:
            return "OPENING";
        case GATE_STATE_OPEN:
            return "OPEN";
        case GATE_STATE_CLOSING:
            return "CLOSING";
        case GATE_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

static gate_t *create_real_gate(void)
{
    gate_config_t config;
    gate_t *gate;

    memset(&config, 0, sizeof(config));
    config.pwmchip_path = GATE_PWMCHIP_PATH;
    config.channel = GATE_PWM_CHANNEL;
    config.period_ns = GATE_PERIOD_NS;
    config.open_pulse_ns = GATE_OPEN_PULSE_NS;
    config.close_pulse_ns = GATE_CLOSE_PULSE_NS;
    config.movement_time_ms = GATE_MOVEMENT_TIME_MS;
    config.hold_after_move = GATE_HOLD_AFTER_MOVE;

    LOG_WARN("真实 Gate 已启用；创建时将立即执行关闸，"
             "当前系统没有限位、地感或防砸反馈");
    LOG_INFO("Gate 配置: %s/pwm%u, period=%" PRIu64
             " ns, open=%" PRIu64 " ns, close=%" PRIu64
             " ns, movement=%u ms",
             config.pwmchip_path, config.channel, config.period_ns,
             config.open_pulse_ns, config.close_pulse_ns,
             config.movement_time_ms);

    gate = gate_create(&config);
    if (gate == NULL)
        LOG_ERROR("创建真实 Gate 失败");

    return gate;
}

// 信号处理函数，处理ctrl+c信号，kill等信号，退出程序
static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;       //当这个信号发生时，就调用 handle_signal()
    sigemptyset(&action.sa_mask);           // 信号处理期间不额外阻塞其他信号

    //如果是中断或终止信号，就调用 handle_signal() 函数
    if(sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0){
        LOG_ERROR("安装信号处理失败: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int dispatch_demo_event(parking_demo_t *demo,
                               const parking_event_t *event,
                               unsigned int depth);

static int update_real_gate(parking_demo_t *demo)
{
    parking_event_t event;
    gate_state_t state;
    uint64_t now_us;
    int update_result;

    if (demo == NULL)
        return -1;
    if (!demo->enable_gate)
        return 0;
    if (demo->gate == NULL)
        return -1;

    update_result = gate_update(demo->gate);
    state = gate_get_state(demo->gate);
    if (update_result != GATE_OK || state == GATE_STATE_ERROR) {
        LOG_ERROR("更新真实 Gate 失败: %s", gate_last_error(demo->gate));
        if (monotonic_us(&now_us) != 0)
            return -1;

        memset(&event, 0, sizeof(event));
        event.type = PARKING_EVENT_GATE_FAILED;
        event.occurred_at_us = now_us;
        event.data.failure.reason = PARKING_REASON_GATE_FAILURE;
        demo->observed_gate_state = GATE_STATE_ERROR;
        return dispatch_demo_event(demo, &event, 0);
    }

    if (state == demo->observed_gate_state)
        return 0;

    LOG_INFO("真实 Gate 状态: %s -> %s",
             gate_state_name(demo->observed_gate_state),
             gate_state_name(state));
    demo->observed_gate_state = state;

    if (state != GATE_STATE_OPEN && state != GATE_STATE_CLOSED)
        return 0;

    if (monotonic_us(&now_us) != 0)
        return -1;

    memset(&event, 0, sizeof(event));
    event.type = state == GATE_STATE_OPEN
        ? PARKING_EVENT_GATE_OPENED
        : PARKING_EVENT_GATE_CLOSED;
    event.occurred_at_us = now_us;
    return dispatch_demo_event(demo, &event, 0);
}

static void log_store_error(parking_demo_t *demo, const char *operation)
{
    char error[ERROR_MESSAGE_CAPACITY];

    memset(error, 0, sizeof(error));
    if (demo != NULL && demo->store != NULL)
        parking_store_copy_last_error(demo->store, error, sizeof(error));

    LOG_ERROR("%s: %s", operation,
              error[0] != '\0' ? error : "未知 SQLite 错误");
}

static void copy_check_status(parking_demo_t *demo,
                              const parking_store_check_result_t *result)
{
    demo->store_status.total_spaces = result->total_spaces;
    demo->store_status.occupied_spaces = result->occupied_spaces;
    demo->store_status.pending_entry_operations =
        result->pending_entry_operations;
    demo->store_status.available_spaces = result->available_spaces;
}

static int refresh_store_status(parking_demo_t *demo)
{
    if (parking_store_get_status(demo->store, &demo->store_status) != 0) {
        log_store_error(demo, "刷新停车场状态失败");
        return -1;
    }

    return 0;
}

static gate_state_t ui_gate_state(const parking_demo_t *demo)
{
    parking_state_t state;

    if (demo->enable_gate)
        return demo->observed_gate_state;

    state = parking_controller_state(demo->controller);
    switch (state) {
        case PARKING_STATE_OPENING:
            return GATE_STATE_OPENING;
        case PARKING_STATE_WAIT_PASSAGE:
            return GATE_STATE_OPEN;
        case PARKING_STATE_CLOSING:
            return GATE_STATE_CLOSING;
        case PARKING_STATE_ERROR:
            return GATE_STATE_ERROR;
        default:
            return GATE_STATE_CLOSED;
    }
}

static int update_parking_ui(parking_demo_t *demo)
{
    parking_ui_status_t status;

    memset(&status, 0, sizeof(status));
    status.lane_mode = parking_controller_mode(demo->controller);
    status.business_state = parking_controller_state(demo->controller);
    status.reason = demo->ui_reason;
    status.gate_state = ui_gate_state(demo);
    status.gate_is_real = demo->enable_gate;
    status.plate_number = demo->last_plate;
    status.plate_color = demo->last_plate_color;
    status.confidence = demo->last_confidence;
    status.amount_cents = demo->amount_cents;
    status.total_spaces = demo->store_status.total_spaces;
    status.occupied_spaces = demo->store_status.occupied_spaces;
    status.available_spaces = demo->store_status.available_spaces;
    return parking_ui_set_status(demo->ui, &status);
}

static parking_reason_t map_store_reason(parking_store_reason_t reason)
{
    switch (reason) {
        case PARKING_STORE_REASON_ALREADY_INSIDE:
            return PARKING_REASON_ALREADY_INSIDE;
        case PARKING_STORE_REASON_LOT_FULL:
            return PARKING_REASON_LOT_FULL;
        case PARKING_STORE_REASON_NO_ACTIVE_SESSION:
            return PARKING_REASON_NO_ACTIVE_SESSION;
        case PARKING_STORE_REASON_NONE:
        default:
            return PARKING_REASON_CHECK_REJECTED;
    }
}

static int calculate_parking_fee(void *context,
                                 uint64_t entry_time_unix_us,
                                 uint64_t exit_time_unix_us,
                                 uint64_t *fee_cents)
{
    parking_fee_status_t status;

    status = parking_fee_calc((parking_fee_t *)context,
                              entry_time_unix_us,
                              exit_time_unix_us,
                              fee_cents);
    if (status == PARKING_FEE_OK)
        return 0;

    LOG_ERROR("计算停车费失败: %s", parking_fee_status_name(status));
    return -1;
}

static int execute_demo_commands(parking_demo_t *demo,
                                 const parking_command_list_t *commands,
                                 uint64_t now_us,
                                 unsigned int depth)
{
    size_t i;

    if (demo == NULL || commands == NULL)
        return -1;

    for (i = 0; i < commands->count; ++i) {
        const parking_command_t *command = &commands->items[i];
        parking_event_t response;
        parking_store_check_result_t check_result;
        uint64_t database_now_us;
        int has_response = 1;

        memset(&response, 0, sizeof(response));
        response.occurred_at_us = now_us;

        switch (command->type) {
            case PARKING_COMMAND_CHECK_ENTRY: {
                if (realtime_us(&database_now_us) != 0) {
                    LOG_ERROR("获取入场数据库时间失败");
                    return -1;
                }

                memset(&check_result, 0, sizeof(check_result));
                if (parking_store_check_entry(
                        demo->store, command->plate_number,
                        database_now_us, &check_result) != 0) {
                    log_store_error(demo, "SQLite 入场检查失败");
                    return -1;
                }
                copy_check_status(demo, &check_result);

                if (!check_result.allowed) {
                    LOG_WARN("SQLite 入场检查拒绝: %s, 原因=%s",
                             command->plate_number,
                             parking_store_reason_name(check_result.reason));
                    response.type = PARKING_EVENT_CHECK_REJECTED;
                    response.data.rejection.reason =
                        map_store_reason(check_result.reason);
                    demo->ui_reason = response.data.rejection.reason;
                    break;
                }

                LOG_INFO("SQLite 入场检查通过: %s, operation_id=%" PRIu64
                         ", 可用车位=%u/%u",
                         command->plate_number,
                         check_result.operation_id,
                         check_result.available_spaces,
                         check_result.total_spaces);
                response.type = PARKING_EVENT_CHECK_APPROVED;
                response.data.check.operation_id = check_result.operation_id;
                response.data.check.payment_required = 0;
                demo->amount_cents = 0;
                break;
            }

            case PARKING_COMMAND_CHECK_EXIT: {
                if (realtime_us(&database_now_us) != 0) {
                    LOG_ERROR("获取出场数据库时间失败");
                    return -1;
                }

                memset(&check_result, 0, sizeof(check_result));
                if (parking_store_check_exit(
                        demo->store, command->plate_number,
                        database_now_us, calculate_parking_fee,
                        demo->fee, &check_result) != 0) {
                    log_store_error(demo, "SQLite 出场检查失败");
                    return -1;
                }
                copy_check_status(demo, &check_result);

                if (!check_result.allowed) {
                    LOG_WARN("SQLite 出场检查拒绝: %s, 原因=%s",
                             command->plate_number,
                             parking_store_reason_name(check_result.reason));
                    response.type = PARKING_EVENT_CHECK_REJECTED;
                    response.data.rejection.reason =
                        map_store_reason(check_result.reason);
                    demo->ui_reason = response.data.rejection.reason;
                    break;
                }

                LOG_INFO("SQLite 出场检查通过: %s, operation_id=%" PRIu64
                         ", 费用=%" PRIu64 " 分",
                         command->plate_number,
                         check_result.operation_id,
                         check_result.amount_cents);
                response.type = PARKING_EVENT_CHECK_APPROVED;
                response.data.check.operation_id = check_result.operation_id;
                response.data.check.amount_cents = check_result.amount_cents;
                response.data.check.payment_required =
                    check_result.payment_required;
                demo->amount_cents = check_result.amount_cents;
                break;
            }

            case PARKING_COMMAND_REQUEST_PAYMENT:
                LOG_INFO("[DRY-RUN] 模拟支付成功: %s, 金额=%" PRIu64 " 分",
                         command->plate_number, command->amount_cents);
                response.type = PARKING_EVENT_PAYMENT_SUCCEEDED;
                break;

            case PARKING_COMMAND_OPEN_GATE:
                if (!demo->enable_gate) {
                    LOG_INFO("[DRY-RUN] 请求开闸: %s",
                             command->plate_number);
                    response.type = PARKING_EVENT_GATE_OPENED;
                    break;
                }

                if (gate_open(demo->gate) != GATE_OK) {
                    LOG_ERROR("真实 Gate 开闸失败: %s",
                              gate_last_error(demo->gate));
                    response.type = PARKING_EVENT_GATE_FAILED;
                    response.data.failure.reason =
                        PARKING_REASON_GATE_FAILURE;
                    demo->ui_reason = PARKING_REASON_GATE_FAILURE;
                    break;
                }

                demo->observed_gate_state = gate_get_state(demo->gate);
                LOG_INFO("真实 Gate 开始开闸: %s",
                         command->plate_number);
                has_response = 0;
                break;

            case PARKING_COMMAND_CLOSE_GATE:
                if (!demo->enable_gate) {
                    LOG_INFO("[DRY-RUN] 请求关闸: %s, 通行来源=%s",
                             command->plate_number,
                             parking_controller_passage_source_name(
                                 command->passage_source));
                    response.type = PARKING_EVENT_GATE_CLOSED;
                    break;
                }

                if (gate_close(demo->gate) != GATE_OK) {
                    LOG_ERROR("真实 Gate 关闸失败: %s",
                              gate_last_error(demo->gate));
                    response.type = PARKING_EVENT_GATE_FAILED;
                    response.data.failure.reason =
                        PARKING_REASON_GATE_FAILURE;
                    demo->ui_reason = PARKING_REASON_GATE_FAILURE;
                    break;
                }

                demo->observed_gate_state = gate_get_state(demo->gate);
                LOG_INFO("真实 Gate 开始关闸: %s, 通行来源=%s",
                         command->plate_number,
                         parking_controller_passage_source_name(
                             command->passage_source));
                has_response = 0;
                break;

            case PARKING_COMMAND_COMPLETE_ENTRY:
                if (realtime_us(&database_now_us) != 0) {
                    LOG_ERROR("获取入场提交时间失败");
                    return -1;
                }
                if (parking_store_complete_entry(
                        demo->store, command->operation_id,
                        command->plate_number, database_now_us) != 0) {
                    log_store_error(demo, "SQLite 入场提交失败");
                    response.type = PARKING_EVENT_COMMIT_FAILED;
                    demo->ui_reason = PARKING_REASON_COMMIT_FAILURE;
                } else {
                    LOG_INFO("SQLite 入场完成: %s, operation_id=%" PRIu64,
                             command->plate_number,
                             command->operation_id);
                    response.type = PARKING_EVENT_COMMIT_SUCCEEDED;
                    if (refresh_store_status(demo) != 0)
                        return -1;
                }
                break;

            case PARKING_COMMAND_COMPLETE_EXIT:
                if (realtime_us(&database_now_us) != 0) {
                    LOG_ERROR("获取出场提交时间失败");
                    return -1;
                }
                if (parking_store_complete_exit(
                        demo->store, command->operation_id,
                        command->plate_number, database_now_us) != 0) {
                    log_store_error(demo, "SQLite 出场提交失败");
                    response.type = PARKING_EVENT_COMMIT_FAILED;
                    demo->ui_reason = PARKING_REASON_COMMIT_FAILURE;
                } else {
                    LOG_INFO("SQLite 出场完成: %s, operation_id=%" PRIu64,
                             command->plate_number,
                             command->operation_id);
                    response.type = PARKING_EVENT_COMMIT_SUCCEEDED;
                    if (refresh_store_status(demo) != 0)
                        return -1;
                }
                break;

            case PARKING_COMMAND_REJECT:
                LOG_WARN("[DRY-RUN] 业务拒绝: %s, 原因=%s",
                         command->plate_number,
                         parking_controller_reason_name(command->reason));
                demo->ui_reason = command->reason;
                has_response = 0;
                break;

            case PARKING_COMMAND_REPORT_ERROR: {
                char error[ERROR_MESSAGE_CAPACITY];

                memset(error, 0, sizeof(error));
                parking_controller_copy_last_error(
                    demo->controller, error, sizeof(error));
                LOG_ERROR("停车业务状态机失败: %s, 原因=%s",
                          error[0] != '\0' ? error : "未知错误",
                          parking_controller_reason_name(command->reason));
                demo->ui_reason = command->reason;
                return -1;
            }

            default:
                LOG_ERROR("未知停车业务命令: %d", (int)command->type);
                return -1;
        }

        if (has_response &&
            dispatch_demo_event(demo, &response, depth + 1U) != 0)
            return -1;
    }

    return 0;
}

static int dispatch_demo_event(parking_demo_t *demo,
                               const parking_event_t *event,
                               unsigned int depth)
{
    parking_command_list_t commands;

    if (demo == NULL || demo->controller == NULL || event == NULL)
        return -1;

    if (depth > DEMO_MAX_DISPATCH_DEPTH) {
        LOG_ERROR("停车业务事件递归超过安全深度");
        return -1;
    }

    memset(&commands, 0, sizeof(commands));
    if (parking_controller_handle_event(
            demo->controller, event, &commands) != 0) {
        char error[ERROR_MESSAGE_CAPACITY];

        memset(error, 0, sizeof(error));
        parking_controller_copy_last_error(
            demo->controller, error, sizeof(error));
        LOG_ERROR("处理停车业务事件失败: %s",
                  error[0] != '\0' ? error : "未知错误");
        return -1;
    }

    return execute_demo_commands(demo, &commands,
                                 event->occurred_at_us, depth);
}

static int process_plate_events(plate_pipeline_t *pipeline,
                                parking_demo_t *demo)
{
    for (;;) {
        plate_pipeline_event_t pipeline_event;
        parking_event_t business_event;
        int result;

        memset(&pipeline_event, 0, sizeof(pipeline_event));
        result = plate_pipeline_take_event(pipeline, &pipeline_event);
        if (result < 0) {
            LOG_ERROR("读取稳定车牌事件失败");
            return -1;
        }
        if (result == 0)
            return 0;

        LOG_INFO("稳定车牌: %s, 颜色：%s, 置信度：%.2f",
                 pipeline_event.result.plate_number,
                 pipeline_event.result.color_name,
                 pipeline_event.result.detect_confidence);

        snprintf(demo->last_plate, sizeof(demo->last_plate), "%s",
                 pipeline_event.result.plate_number);
        snprintf(demo->last_plate_color,
                 sizeof(demo->last_plate_color), "%s",
                 pipeline_event.result.color_name);
        demo->last_confidence =
            pipeline_event.result.detect_confidence;
        demo->amount_cents = 0;
        demo->ui_reason = PARKING_REASON_NONE;

        memset(&business_event, 0, sizeof(business_event));
        business_event.type = PARKING_EVENT_PLATE_CONFIRMED;
        business_event.occurred_at_us = pipeline_event.confirmed_at_us;
        snprintf(business_event.data.plate.plate_number,
                 sizeof(business_event.data.plate.plate_number),
                 "%s", pipeline_event.result.plate_number);
        business_event.data.plate.confidence =
            pipeline_event.result.detect_confidence;

        if (dispatch_demo_event(demo, &business_event, 0) != 0)
            return -1;
    }
}

static int update_parking_controller(plate_pipeline_t *pipeline,
                                     parking_demo_t *demo)
{
    parking_command_list_t commands;
    uint64_t now_us;
    uint64_t last_plate_seen_us;
    int result;

    last_plate_seen_us = plate_pipeline_last_plate_seen_us(pipeline);
    if (monotonic_us(&now_us) != 0) {
        LOG_ERROR("获取停车业务单调时间失败");
        return -1;
    }

    memset(&commands, 0, sizeof(commands));
    if (parking_controller_tick(demo->controller, now_us,
                                last_plate_seen_us, &commands) != 0) {
        char error[ERROR_MESSAGE_CAPACITY];

        memset(error, 0, sizeof(error));
        parking_controller_copy_last_error(
            demo->controller, error, sizeof(error));
        LOG_ERROR("更新停车业务状态机失败: %s",
                  error[0] != '\0' ? error : "未知错误");
        return -1;
    }

    result = execute_demo_commands(demo, &commands, now_us, 0);
    if (result == 0 &&
        parking_controller_state(demo->controller) == PARKING_STATE_IDLE)
        demo->ui_reason = PARKING_REASON_NONE;
    return result;
}

static int run_loop(plate_pipeline_t *pipeline, parking_demo_t *demo)
{
    while(running){
        pipeline_frame_t frame;
        int result;

        if (update_real_gate(demo) != 0)
            return -1;

        if (process_plate_events(pipeline, demo) != 0)
            return -1;

        if (update_parking_controller(pipeline, demo) != 0)
            return -1;

        if (update_parking_ui(demo) != 0)
            return -1;
        parking_ui_process(demo->ui);

        memset(&frame, 0, sizeof(frame));
        result = plate_pipeline_take_frame(pipeline, &frame, PIPELINE_WAIT_MS);

        if(result == PLATE_PIPELINE_TIMEOUT)
            continue;

        if(result == PLATE_PIPELINE_CLOSED)
            break;

        if(result != PLATE_PIPELINE_OK){
            LOG_ERROR("读取识别帧失败");
            return -1;
        }

        if (parking_ui_present_bgr(demo->ui, frame.image.data,
                                   frame.image.width, frame.image.height,
                                   frame.image.stride) != 0) {
            pipeline_frame_release(&frame);
            LOG_ERROR("显示图像失败");
            return -1;
        }

        pipeline_frame_release(&frame);
        parking_ui_process(demo->ui);
    }

    return plate_pipeline_failed(pipeline) ? -1 : 0;
}


int main(int argc, char **argv)
{
    plate_pipeline_config_t pipeline_config;
    parking_controller_config_t controller_config;
    parking_store_config_t store_config;
    parking_store_status_t store_status;
    parking_ui_config_t ui_config;
    parking_demo_t demo;
    plate_pipeline_t *pipeline = NULL;
    parking_controller_t *controller = NULL;
    parking_fee_t *fee = NULL;
    parking_store_t *store = NULL;
    parking_ui_t *ui = NULL;
    gate_t *gate = NULL;
    parking_lane_mode_t lane_mode;
    const char *database_path;
    int enable_gate;
    int argument_result;
    int exit_code = 1;

    argument_result = parse_arguments(argc, argv, &lane_mode,
                                      &database_path, &enable_gate);
    if (argument_result > 0)
        return 0;
    if (argument_result < 0)
        return 1;

    if(install_signal_handlers() != 0){
        return 1;
    }

    memset(&controller_config, 0, sizeof(controller_config));
    controller_config.mode = lane_mode;
    controller_config.gate_initially_closed = enable_gate ? 0 : 1;

    controller = parking_controller_create(&controller_config);
    if (controller == NULL)
        goto cleanup;

    fee = parking_fee_create();
    if (fee == NULL) {
        LOG_ERROR("创建停车计费模块失败");
        goto cleanup;
    }

    memset(&store_config, 0, sizeof(store_config));
    store_config.database_path = database_path;
    store_config.initial_total_spaces =
        PARKING_STORE_DEFAULT_TOTAL_SPACES;
    store_config.busy_timeout_ms =
        PARKING_STORE_DEFAULT_BUSY_TIMEOUT_MS;

    store = parking_store_open(&store_config);
    if (store == NULL) {
        LOG_ERROR("打开停车数据库失败: %s", database_path);
        goto cleanup;
    }

    memset(&store_status, 0, sizeof(store_status));
    if (parking_store_get_status(store, &store_status) != 0) {
        parking_demo_t error_demo;

        memset(&error_demo, 0, sizeof(error_demo));
        error_demo.store = store;
        log_store_error(&error_demo, "读取停车数据库状态失败");
        goto cleanup;
    }

    memset(&demo, 0, sizeof(demo));
    demo.controller = controller;
    demo.fee = fee;
    demo.store = store;
    demo.enable_gate = enable_gate;
    demo.store_status = store_status;

    memset(&ui_config, 0, sizeof(ui_config));
    ui_config.device = DRM_DEVICE;
    ui_config.connector_id = -1;
    ui_config.font_path = FONT_PATH;

    ui = parking_ui_create(&ui_config);
    if (ui == NULL) {
        LOG_ERROR("创建 LVGL 停车界面失败");
        goto cleanup;
    }
    demo.ui = ui;

    memset(&pipeline_config, 0, sizeof(pipeline_config));
    pipeline_config.camera.device = CAMERA_DEVICE;
    pipeline_config.camera.width = CAMERA_WIDTH;
    pipeline_config.camera.height = CAMERA_HEIGHT;
    pipeline_config.camera.fps = CAMERA_FPS;
    pipeline_config.camera.pixel_format = V4L2_PIX_FMT_MJPEG;

    pipeline_config.detect_model_path = DETECT_MODEL;
    pipeline_config.recognition_model_path = RECOGNITION_MODEL;
    pipeline_config.font_path = FONT_PATH;
    pipeline_config.confidence_threshold = 0.30F;

    pipeline_config.confirmation.required_frames = 3U;
    pipeline_config.confirmation.max_missed_frames = 3U;
    pipeline_config.confirmation.same_plate_cooldown_us = 10000000ULL;

    pipeline_config.camera_timeout_ms = 1000;
    pipeline_config.frame_wait_ms = PIPELINE_WAIT_MS;

    pipeline = plate_pipeline_create(&pipeline_config);
    if (pipeline == NULL)
        goto cleanup;

    if (enable_gate) {
        gate = create_real_gate();
        if (gate == NULL)
            goto cleanup;
        demo.gate = gate;
        demo.observed_gate_state = gate_get_state(gate);
    }

    if (plate_pipeline_start(pipeline) != PLATE_PIPELINE_OK)
        goto cleanup;

    LOG_INFO("Parking 主程序启动，车道模式=%s，SQLite=%s，"
             "可用车位=%u/%u，未完成入场=%u，"
             "支付=DRY-RUN，Gate=%s，按 Ctrl+C 退出",
             lane_mode_name(lane_mode), database_path,
             store_status.available_spaces,
             store_status.total_spaces,
             store_status.pending_entry_operations,
             enable_gate ? "REAL" : "DRY-RUN");

    if (run_loop(pipeline, &demo) == 0)
        exit_code = 0;

cleanup:
    plate_pipeline_stop(pipeline);
    plate_pipeline_destroy(pipeline);
    parking_ui_destroy(ui);
    parking_store_close(store);
    parking_fee_destroy(fee);
    parking_controller_destroy(controller);
    gate_destroy(gate);

    return exit_code;
}

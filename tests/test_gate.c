#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gate.h"

#define INPUT_SIZE 64

/*
 * 睡眠指定毫秒
 */
static void sleep_ms(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    nanosleep(&ts, NULL);
}


/*
 * 状态转字符串
 */
static const char *gate_state_to_string(gate_state_t state)
{
    switch (state) {
    case GATE_STATE_OPEN:
        return "OPEN";

    case GATE_STATE_CLOSED:
        return "CLOSED";

    case GATE_STATE_OPENING:
        return "OPENING";

    case GATE_STATE_CLOSING:
        return "CLOSING";

    case GATE_STATE_ERROR:
        return "ERROR";

    default:
        return "UNKNOWN";
    }
}


/*
 * 等待闸门动作完成
 *
 * gate_open() / gate_close() 只是启动动作，
 * 真正从 OPENING/CLOSING 切换到 OPEN/CLOSED
 * 需要不断调用 gate_update()
 */
static int wait_gate_finished(gate_t *gate)
{
    gate_state_t state;

    while (1) {

        if (gate_update(gate) != GATE_OK) {
            printf("[ERROR] gate_update failed: %s\n",
                   gate_last_error(gate));
            return -1;
        }

        state = gate_get_state(gate);

        if (state != GATE_STATE_OPENING &&
            state != GATE_STATE_CLOSING) {
            break;
        }

        sleep_ms(20);
    }

    printf("[INFO] gate state: %s\n",
           gate_state_to_string(state));

    return 0;
}


int main(int argc, char *argv[])
{
    gate_config_t config;
    gate_t *gate;
    char command[INPUT_SIZE];

    /*
     * 默认配置：
     *
     * PWM 周期：20ms = 50Hz
     * 开闸：2.0ms
     * 关闸：1.0ms
     * 动作时间：800ms
     * 动作完成后关闭 PWM
     *
     * 具体脉宽根据你的舵机实际情况调整。
     */
    const char *pwmchip_path = "/sys/class/pwm/pwmchip2";
    unsigned int channel = 0;

    memset(&config, 0, sizeof(config));

    config.pwmchip_path = pwmchip_path;
    config.channel = channel;

    config.period_ns = 20000000ULL;       /* 20 ms */
    config.open_pulse_ns = 2400000ULL;    /* 2.4 ms */
    config.close_pulse_ns = 1400000ULL;   /* 1.4 ms */

    config.movement_time_ms = 800;
    config.hold_after_move = 0;

    printf("========================================\n");
    printf(" RK3576 Gate PWM Test\n");
    printf("========================================\n");

    printf("PWM chip       : %s\n", config.pwmchip_path);
    printf("PWM channel    : %u\n", config.channel);
    printf("period         : %llu ns\n",
           (unsigned long long)config.period_ns);
    printf("open pulse     : %llu ns\n",
           (unsigned long long)config.open_pulse_ns);
    printf("close pulse    : %llu ns\n",
           (unsigned long long)config.close_pulse_ns);
    printf("movement time  : %u ms\n",
           config.movement_time_ms);
    printf("hold after move: %d\n",
           config.hold_after_move);

    printf("----------------------------------------\n");


    /*
     * 创建 Gate
     *
     * 注意：
     * 你的 gate_create() 内部会主动调用 gate_close()，
     * 因此创建后状态实际上会先进入 CLOSING。
     */
    gate = gate_create(&config);

    if (gate == NULL) {
        printf("[ERROR] gate_create failed\n");
        return EXIT_FAILURE;
    }

    printf("[INFO] gate created\n");
    printf("[INFO] initial state: %s\n",
           gate_state_to_string(gate_get_state(gate)));


    /*
     * 等待初始化时的关闸动作完成
     */
    if (wait_gate_finished(gate) != 0) {
        gate_destroy(gate);
        return EXIT_FAILURE;
    }


    printf("\nCommands:\n");
    printf("  open   - 开闸\n");
    printf("  close  - 关闸\n");
    printf("  status - 查看状态\n");
    printf("  quit   - 退出\n\n");


    while (1) {

        printf("gate> ");
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }

        /*
         * 删除换行符
         */
        command[strcspn(command, "\r\n")] = '\0';


        /*
         * 开闸
         */
        if (strcmp(command, "open") == 0) {

            printf("[INFO] opening gate...\n");

            int ret = gate_open(gate);

            if (ret != GATE_OK) {
                printf("[ERROR] gate_open failed, ret=%d, error=%s\n",
                       ret,
                       gate_last_error(gate));
                continue;
            }

            if (wait_gate_finished(gate) != 0)
                break;
        }


        /*
         * 关闸
         */
        else if (strcmp(command, "close") == 0) {

            printf("[INFO] closing gate...\n");

            int ret = gate_close(gate);

            if (ret != GATE_OK) {
                printf("[ERROR] gate_close failed, ret=%d, error=%s\n",
                       ret,
                       gate_last_error(gate));
                continue;
            }

            if (wait_gate_finished(gate) != 0)
                break;
        }


        /*
         * 查看状态
         */
        else if (strcmp(command, "status") == 0) {

            printf("[INFO] gate state: %s\n",
                   gate_state_to_string(
                       gate_get_state(gate)));
        }


        /*
         * 退出
         */
        else if (strcmp(command, "quit") == 0 ||
                 strcmp(command, "exit") == 0) {

            printf("[INFO] exit\n");
            break;
        }


        else if (command[0] == '\0') {
            continue;
        }


        else {
            printf("Unknown command: %s\n", command);
            printf("Available: open / close / status / quit\n");
        }
    }


    gate_destroy(gate);

    printf("[INFO] gate destroyed\n");

    return EXIT_SUCCESS;
}
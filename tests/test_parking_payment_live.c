#include "parking_payment.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void wait_100ms(void)
{
    struct timespec delay;

    delay.tv_sec = 0;
    delay.tv_nsec = 100000000L;
    (void)nanosleep(&delay, NULL);
}

int main(int argc, char **argv)
{
    parking_payment_config_t config;
    parking_payment_snapshot_t snapshot;
    parking_payment_t *payment;
    const char *base_url = argc > 1 ? argv[1] : "http://127.0.0.1:8000";
    unsigned int attempt;
    int exit_code = 1;

    memset(&config, 0, sizeof(config));
    config.base_url = base_url;
    config.site_id = "payment-test";
    config.device_id = "host-test";
    config.poll_interval_ms = 100U;
    config.network_timeout_ms = 1000U;

    payment = parking_payment_create(&config);
    if (payment == NULL || parking_payment_start(payment) != 0) {
        fprintf(stderr, "创建支付客户端失败\n");
        parking_payment_destroy(payment);
        return 1;
    }
    if (parking_payment_request(payment, 9001U, "浙A66666", 50U) != 0) {
        fprintf(stderr, "创建支付订单失败\n");
        goto cleanup;
    }

    for (attempt = 0U; attempt < 50U; ++attempt) {
        if (parking_payment_get_snapshot(payment, &snapshot) != 0)
            goto cleanup;
        if (snapshot.state == PARKING_PAYMENT_PENDING ||
            snapshot.state == PARKING_PAYMENT_PAID)
            break;
        wait_100ms();
    }
    if (snapshot.payment_url[0] == '\0') {
        fprintf(stderr, "等待支付二维码失败: %s\n", snapshot.last_error);
        goto cleanup;
    }
    printf("PAY_URL=%s\n", snapshot.payment_url);
    fflush(stdout);

    for (attempt = 0U; attempt < 100U; ++attempt) {
        if (parking_payment_get_snapshot(payment, &snapshot) != 0)
            goto cleanup;
        if (snapshot.state == PARKING_PAYMENT_PAID) {
            printf("支付状态: %s, operation_id=%" PRIu64 "\n",
                   parking_payment_state_name(snapshot.state),
                   snapshot.operation_id);
            exit_code = 0;
            break;
        }
        if (snapshot.state == PARKING_PAYMENT_FAILED) {
            fprintf(stderr, "支付失败: %s\n", snapshot.last_error);
            break;
        }
        wait_100ms();
    }

cleanup:
    parking_payment_destroy(payment);
    return exit_code;
}

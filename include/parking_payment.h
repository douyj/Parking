#ifndef PARKING_PAYMENT_H
#define PARKING_PAYMENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARKING_PAYMENT_URL_CAPACITY 512U
#define PARKING_PAYMENT_ERROR_CAPACITY 256U

typedef struct parking_payment parking_payment_t;

typedef enum {
    PARKING_PAYMENT_IDLE = 0,
    PARKING_PAYMENT_CREATING,
    PARKING_PAYMENT_PENDING,
    PARKING_PAYMENT_PAID,
    PARKING_PAYMENT_FAILED
} parking_payment_state_t;

typedef struct {
    const char *base_url;
    const char *site_id;
    const char *device_id;
    unsigned int poll_interval_ms;
    unsigned int network_timeout_ms;
} parking_payment_config_t;

typedef struct {
    parking_payment_state_t state;
    uint64_t operation_id;
    uint64_t amount_cents;
    char payment_url[PARKING_PAYMENT_URL_CAPACITY];
    char last_error[PARKING_PAYMENT_ERROR_CAPACITY];
} parking_payment_snapshot_t;

parking_payment_t *parking_payment_create(
    const parking_payment_config_t *config);

int parking_payment_start(parking_payment_t *payment);

int parking_payment_request(parking_payment_t *payment,
                            uint64_t operation_id,
                            const char *plate_number,
                            uint64_t amount_cents);

int parking_payment_get_snapshot(
    parking_payment_t *payment,
    parking_payment_snapshot_t *snapshot);

void parking_payment_reset(parking_payment_t *payment);
void parking_payment_stop(parking_payment_t *payment);
void parking_payment_destroy(parking_payment_t *payment);

const char *parking_payment_state_name(parking_payment_state_t state);

#ifdef __cplusplus
}
#endif

#endif

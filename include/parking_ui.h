#ifndef PARKING_UI_H
#define PARKING_UI_H

#include "gate.h"
#include "parking_controller.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parking_ui parking_ui_t;

typedef struct {
    const char *device;
    int64_t connector_id;
    const char *font_path;
} parking_ui_config_t;

typedef struct {
    parking_lane_mode_t lane_mode;
    parking_state_t business_state;
    parking_reason_t reason;
    gate_state_t gate_state;
    int gate_is_real;

    const char *plate_number;
    const char *plate_color;
    float confidence;
    uint64_t amount_cents;

    uint32_t total_spaces;
    uint32_t occupied_spaces;
    uint32_t available_spaces;
} parking_ui_status_t;

parking_ui_t *parking_ui_create(const parking_ui_config_t *config);

int parking_ui_present_bgr(parking_ui_t *ui, const unsigned char *data,
                           unsigned int width, unsigned int height,
                           unsigned int stride);

int parking_ui_set_status(parking_ui_t *ui,
                          const parking_ui_status_t *status);

void parking_ui_process(parking_ui_t *ui);
void parking_ui_destroy(parking_ui_t *ui);

#ifdef __cplusplus
}
#endif

#endif

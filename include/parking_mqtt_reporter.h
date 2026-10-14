#ifndef PARKING_MQTT_REPORTER_H
#define PARKING_MQTT_REPORTER_H

#include "parking_mqtt.h"
#include "parking_protocol.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARKING_MQTT_REPORTER_DEFAULT_HEARTBEAT_MS 10000U

typedef struct parking_mqtt_reporter parking_mqtt_reporter_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *site_id;
    const char *device_id;
    const char *username;
    const char *password;
    const char *ca_file;
    const char *software_version;
    uint64_t heartbeat_interval_ms;
} parking_mqtt_reporter_config_t;

typedef struct {
    parking_protocol_lane_mode_t lane_mode;
    parking_protocol_business_state_t business_state;
    parking_protocol_gate_mode_t gate_mode;
    parking_protocol_gate_state_t gate_state;
    parking_protocol_component_state_t camera_state;
    parking_protocol_component_state_t recognizer_state;
    parking_protocol_component_state_t database_state;
    parking_protocol_spaces_t spaces;
} parking_mqtt_reporter_status_t;

typedef struct {
    parking_protocol_vehicle_event_type_t event_type;
    uint64_t operation_id;
    uint64_t committed_at_unix_us;
    const char *plate_number;
    const char *plate_color;
    float detect_confidence;
    uint64_t amount_cents;
    parking_protocol_passage_source_t passage_source;
    parking_protocol_spaces_t spaces;
} parking_mqtt_reporter_vehicle_event_t;

parking_mqtt_reporter_t *parking_mqtt_reporter_create(
    const parking_mqtt_reporter_config_t *config);

int parking_mqtt_reporter_start(parking_mqtt_reporter_t *reporter);
void parking_mqtt_reporter_stop(parking_mqtt_reporter_t *reporter);

int parking_mqtt_reporter_publish_status(
    parking_mqtt_reporter_t *reporter,
    const parking_mqtt_reporter_status_t *status);

int parking_mqtt_reporter_publish_vehicle_event(
    parking_mqtt_reporter_t *reporter,
    const parking_mqtt_reporter_vehicle_event_t *event);

int parking_mqtt_reporter_get_stats(
    parking_mqtt_reporter_t *reporter,
    parking_mqtt_stats_t *stats);

int parking_mqtt_reporter_copy_last_error(
    parking_mqtt_reporter_t *reporter,
    char *buffer,
    size_t capacity);

void parking_mqtt_reporter_destroy(parking_mqtt_reporter_t *reporter);

#ifdef __cplusplus
}
#endif

#endif

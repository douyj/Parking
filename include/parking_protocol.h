#ifndef PARKING_PROTOCOL_H
#define PARKING_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARKING_PROTOCOL_SCHEMA_VERSION 1U
#define PARKING_PROTOCOL_TOPIC_PREFIX "parking/v1"

#define PARKING_PROTOCOL_TOPIC_LEVEL_MAX_LENGTH 64U
#define PARKING_PROTOCOL_ID_MAX_LENGTH 64U
#define PARKING_PROTOCOL_VERSION_MAX_LENGTH 32U
#define PARKING_PROTOCOL_PLATE_MAX_LENGTH 31U
#define PARKING_PROTOCOL_COLOR_MAX_LENGTH 31U

typedef enum {
    PARKING_PROTOCOL_OK = 0,
    PARKING_PROTOCOL_BAD_ARG = -1,
    PARKING_PROTOCOL_INVALID_VALUE = -2,
    PARKING_PROTOCOL_BUFFER_TOO_SMALL = -3
} parking_protocol_status_t;

typedef enum {
    PARKING_PROTOCOL_TOPIC_AVAILABILITY = 0,
    PARKING_PROTOCOL_TOPIC_HEARTBEAT,
    PARKING_PROTOCOL_TOPIC_STATUS,
    PARKING_PROTOCOL_TOPIC_EVENT_ENTRY,
    PARKING_PROTOCOL_TOPIC_EVENT_EXIT,
    PARKING_PROTOCOL_TOPIC_COMMAND_REQUEST,
    PARKING_PROTOCOL_TOPIC_COMMAND_REPLY
} parking_protocol_topic_t;

typedef enum {
    PARKING_PROTOCOL_OFFLINE = 0,
    PARKING_PROTOCOL_ONLINE
} parking_protocol_availability_t;

typedef enum {
    PARKING_PROTOCOL_LANE_ENTRY = 0,
    PARKING_PROTOCOL_LANE_EXIT
} parking_protocol_lane_mode_t;

typedef enum {
    PARKING_PROTOCOL_BUSINESS_INITIALIZING = 0,
    PARKING_PROTOCOL_BUSINESS_IDLE,
    PARKING_PROTOCOL_BUSINESS_CHECKING,
    PARKING_PROTOCOL_BUSINESS_WAIT_PAYMENT,
    PARKING_PROTOCOL_BUSINESS_OPENING,
    PARKING_PROTOCOL_BUSINESS_WAIT_PASSAGE,
    PARKING_PROTOCOL_BUSINESS_CLOSING,
    PARKING_PROTOCOL_BUSINESS_COMPLETING,
    PARKING_PROTOCOL_BUSINESS_REJECT_COOLDOWN,
    PARKING_PROTOCOL_BUSINESS_ERROR
} parking_protocol_business_state_t;

typedef enum {
    PARKING_PROTOCOL_GATE_DRY_RUN = 0,
    PARKING_PROTOCOL_GATE_REAL
} parking_protocol_gate_mode_t;

typedef enum {
    PARKING_PROTOCOL_GATE_CLOSED = 0,
    PARKING_PROTOCOL_GATE_OPENING,
    PARKING_PROTOCOL_GATE_OPEN,
    PARKING_PROTOCOL_GATE_CLOSING,
    PARKING_PROTOCOL_GATE_ERROR
} parking_protocol_gate_state_t;

typedef enum {
    PARKING_PROTOCOL_COMPONENT_OK = 0,
    PARKING_PROTOCOL_COMPONENT_UNAVAILABLE,
    PARKING_PROTOCOL_COMPONENT_ERROR
} parking_protocol_component_state_t;

typedef enum {
    PARKING_PROTOCOL_PASSAGE_SENSOR = 0,
    PARKING_PROTOCOL_PASSAGE_MANUAL,
    PARKING_PROTOCOL_PASSAGE_VISION_TIMEOUT
} parking_protocol_passage_source_t;

typedef enum {
    PARKING_PROTOCOL_EVENT_ENTRY = 0,
    PARKING_PROTOCOL_EVENT_EXIT
} parking_protocol_vehicle_event_type_t;

typedef struct {
    const char *message_id;
    const char *site_id;
    const char *device_id;
    const char *session_id;
    uint64_t timestamp_unix_us;
} parking_protocol_metadata_t;

typedef struct {
    uint32_t total;
    uint32_t occupied;
    uint32_t pending_entries;
    uint32_t available;
} parking_protocol_spaces_t;

typedef struct {
    parking_protocol_metadata_t metadata;
    parking_protocol_availability_t state;
} parking_protocol_availability_message_t;

typedef struct {
    parking_protocol_metadata_t metadata;
    uint64_t sequence;
    uint64_t uptime_us;
} parking_protocol_heartbeat_message_t;

typedef struct {
    parking_protocol_metadata_t metadata;
    const char *software_version;
    parking_protocol_lane_mode_t lane_mode;
    parking_protocol_business_state_t business_state;
    parking_protocol_gate_mode_t gate_mode;
    parking_protocol_gate_state_t gate_state;
    parking_protocol_component_state_t camera_state;
    parking_protocol_component_state_t recognizer_state;
    parking_protocol_component_state_t database_state;
    parking_protocol_spaces_t spaces;
} parking_protocol_status_message_t;

typedef struct {
    parking_protocol_metadata_t metadata;
    parking_protocol_vehicle_event_type_t event_type;
    uint64_t operation_id;
    const char *plate_number;
    const char *plate_color;
    float detect_confidence;
    uint64_t amount_cents;
    parking_protocol_passage_source_t passage_source;
    parking_protocol_spaces_t spaces;
} parking_protocol_vehicle_event_t;

/* site_id 和 device_id 仅允许字母、数字、点、下划线和连字符。 */
parking_protocol_status_t parking_protocol_build_topic(
    const char *site_id,
    const char *device_id,
    parking_protocol_topic_t topic,
    char *buffer,
    size_t capacity,
    size_t *required_size);

/* required_size 返回包含结尾 '\0' 的所需容量，可传入 NULL。 */
parking_protocol_status_t parking_protocol_encode_availability(
    const parking_protocol_availability_message_t *message,
    char *buffer,
    size_t capacity,
    size_t *required_size);

parking_protocol_status_t parking_protocol_encode_heartbeat(
    const parking_protocol_heartbeat_message_t *message,
    char *buffer,
    size_t capacity,
    size_t *required_size);

parking_protocol_status_t parking_protocol_encode_status(
    const parking_protocol_status_message_t *message,
    char *buffer,
    size_t capacity,
    size_t *required_size);

parking_protocol_status_t parking_protocol_encode_vehicle_event(
    const parking_protocol_vehicle_event_t *event,
    char *buffer,
    size_t capacity,
    size_t *required_size);

const char *parking_protocol_status_name(parking_protocol_status_t status);

#ifdef __cplusplus
}
#endif

#endif

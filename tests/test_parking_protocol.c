#include "parking_protocol.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "[FAIL] %s:%d: %s\n",                       \
                    __func__, __LINE__, #condition);                       \
            return -1;                                                     \
        }                                                                  \
    } while (0)

#define RUN_TEST(test_function)                                            \
    do {                                                                   \
        if ((test_function)() != 0) {                                      \
            fprintf(stderr, "%s failed\n", #test_function);             \
            return 1;                                                      \
        }                                                                  \
        ++passed;                                                          \
        printf("[PASS] %s\n", #test_function);                           \
    } while (0)

static parking_protocol_metadata_t metadata(const char *message_id)
{
    parking_protocol_metadata_t value;

    memset(&value, 0, sizeof(value));
    value.message_id = message_id;
    value.site_id = "lot-a";
    value.device_id = "gate01";
    value.session_id = "session-7";
    value.timestamp_unix_us = 1700000000000000ULL;
    return value;
}

static parking_protocol_spaces_t spaces(void)
{
    parking_protocol_spaces_t value;

    memset(&value, 0, sizeof(value));
    value.total = 100U;
    value.occupied = 72U;
    value.pending_entries = 1U;
    value.available = 27U;
    return value;
}

static int test_topics(void)
{
    static const struct {
        parking_protocol_topic_t topic;
        const char *expected;
    } cases[] = {
        {PARKING_PROTOCOL_TOPIC_AVAILABILITY,
         "parking/v1/lot-a/gate01/availability"},
        {PARKING_PROTOCOL_TOPIC_HEARTBEAT,
         "parking/v1/lot-a/gate01/heartbeat"},
        {PARKING_PROTOCOL_TOPIC_STATUS,
         "parking/v1/lot-a/gate01/status"},
        {PARKING_PROTOCOL_TOPIC_EVENT_ENTRY,
         "parking/v1/lot-a/gate01/events/entry"},
        {PARKING_PROTOCOL_TOPIC_EVENT_EXIT,
         "parking/v1/lot-a/gate01/events/exit"},
        {PARKING_PROTOCOL_TOPIC_COMMAND_REQUEST,
         "parking/v1/lot-a/gate01/commands/request"},
        {PARKING_PROTOCOL_TOPIC_COMMAND_REPLY,
         "parking/v1/lot-a/gate01/commands/reply"}
    };
    char buffer[128];
    size_t required_size;
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(parking_protocol_build_topic(
                  "lot-a", "gate01", cases[i].topic,
                  buffer, sizeof(buffer), &required_size) ==
              PARKING_PROTOCOL_OK);
        CHECK(strcmp(buffer, cases[i].expected) == 0);
        CHECK(required_size == strlen(cases[i].expected) + 1U);
    }

    CHECK(parking_protocol_build_topic(
              "lot/a", "gate01", PARKING_PROTOCOL_TOPIC_STATUS,
              buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_INVALID_VALUE);
    CHECK(parking_protocol_build_topic(
              "lot-a", "gate+", PARKING_PROTOCOL_TOPIC_STATUS,
              buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_INVALID_VALUE);
    CHECK(parking_protocol_build_topic(
              "lot-a", "gate01", (parking_protocol_topic_t)100,
              buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_INVALID_VALUE);
    return 0;
}

static int test_availability_and_escaping(void)
{
    static const char expected[] =
        "{\"schema_version\":1,\"type\":\"availability\"," 
        "\"message_id\":\"msg\\\"\\\\\\n\\u0001\"," 
        "\"timestamp_unix_us\":1700000000000000," 
        "\"site_id\":\"lot-a\",\"device_id\":\"gate01\"," 
        "\"session_id\":\"session-7\",\"state\":\"online\"}";
    const char escaped_id[] = {'m', 's', 'g', '"', '\\', '\n', '\x01', '\0'};
    parking_protocol_availability_message_t message;
    char buffer[512];
    size_t required_size = 0U;

    memset(&message, 0, sizeof(message));
    message.metadata = metadata(escaped_id);
    message.state = PARKING_PROTOCOL_ONLINE;

    CHECK(parking_protocol_encode_availability(
              &message, buffer, sizeof(buffer), &required_size) ==
          PARKING_PROTOCOL_OK);
    CHECK(strcmp(buffer, expected) == 0);
    CHECK(required_size == sizeof(expected));
    return 0;
}

static int test_heartbeat(void)
{
    static const char expected[] =
        "{\"schema_version\":1,\"type\":\"heartbeat\"," 
        "\"message_id\":\"heartbeat-9\"," 
        "\"timestamp_unix_us\":1700000000000000," 
        "\"site_id\":\"lot-a\",\"device_id\":\"gate01\"," 
        "\"session_id\":\"session-7\",\"sequence\":9," 
        "\"uptime_us\":123456789}";
    parking_protocol_heartbeat_message_t message;
    char buffer[512];

    memset(&message, 0, sizeof(message));
    message.metadata = metadata("heartbeat-9");
    message.sequence = 9U;
    message.uptime_us = 123456789U;

    CHECK(parking_protocol_encode_heartbeat(
              &message, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_OK);
    CHECK(strcmp(buffer, expected) == 0);
    return 0;
}

static int test_status_message(void)
{
    static const char expected[] =
        "{\"schema_version\":1,\"type\":\"device_status\"," 
        "\"message_id\":\"status-1\"," 
        "\"timestamp_unix_us\":1700000000000000," 
        "\"site_id\":\"lot-a\",\"device_id\":\"gate01\"," 
        "\"session_id\":\"session-7\"," 
        "\"software_version\":\"1.0.0\",\"lane_mode\":\"exit\"," 
        "\"business_state\":\"wait_payment\"," 
        "\"gate\":{\"mode\":\"real\",\"state\":\"closed\"}," 
        "\"components\":{\"camera\":\"ok\",\"recognizer\":\"ok\"," 
        "\"database\":\"ok\"},\"spaces\":{\"total\":100," 
        "\"occupied\":72,\"pending_entries\":1,\"available\":27}}";
    parking_protocol_status_message_t message;
    char buffer[1024];

    memset(&message, 0, sizeof(message));
    message.metadata = metadata("status-1");
    message.software_version = "1.0.0";
    message.lane_mode = PARKING_PROTOCOL_LANE_EXIT;
    message.business_state = PARKING_PROTOCOL_BUSINESS_WAIT_PAYMENT;
    message.gate_mode = PARKING_PROTOCOL_GATE_REAL;
    message.gate_state = PARKING_PROTOCOL_GATE_CLOSED;
    message.camera_state = PARKING_PROTOCOL_COMPONENT_OK;
    message.recognizer_state = PARKING_PROTOCOL_COMPONENT_OK;
    message.database_state = PARKING_PROTOCOL_COMPONENT_OK;
    message.spaces = spaces();

    CHECK(parking_protocol_encode_status(
              &message, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_OK);
    CHECK(strcmp(buffer, expected) == 0);
    return 0;
}

static int test_vehicle_events(void)
{
    static const char expected_entry[] =
        "{\"schema_version\":1,\"type\":\"vehicle_entry\"," 
        "\"message_id\":\"entry-42\"," 
        "\"timestamp_unix_us\":1700000000000000," 
        "\"site_id\":\"lot-a\",\"device_id\":\"gate01\"," 
        "\"session_id\":\"session-7\",\"operation_id\":42," 
        "\"lane_mode\":\"entry\",\"plate_number\":\"浙A66666\"," 
        "\"plate_color\":\"蓝色\",\"detect_confidence\":0.95," 
        "\"amount_cents\":0,\"passage_source\":\"vision_timeout\"," 
        "\"result\":\"committed\",\"spaces\":{\"total\":100," 
        "\"occupied\":72,\"pending_entries\":1,\"available\":27}}";
    parking_protocol_vehicle_event_t event;
    char buffer[1024];

    memset(&event, 0, sizeof(event));
    event.metadata = metadata("entry-42");
    event.event_type = PARKING_PROTOCOL_EVENT_ENTRY;
    event.operation_id = 42U;
    event.plate_number = "浙A66666";
    event.plate_color = "蓝色";
    event.detect_confidence = 0.95F;
    event.passage_source = PARKING_PROTOCOL_PASSAGE_VISION_TIMEOUT;
    event.spaces = spaces();

    CHECK(parking_protocol_encode_vehicle_event(
              &event, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_OK);
    CHECK(strcmp(buffer, expected_entry) == 0);

    event.metadata = metadata("exit-43");
    event.event_type = PARKING_PROTOCOL_EVENT_EXIT;
    event.operation_id = 43U;
    event.amount_cents = 50U;
    event.passage_source = PARKING_PROTOCOL_PASSAGE_SENSOR;
    CHECK(parking_protocol_encode_vehicle_event(
              &event, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_OK);
    CHECK(strstr(buffer, "\"type\":\"vehicle_exit\"") != NULL);
    CHECK(strstr(buffer, "\"lane_mode\":\"exit\"") != NULL);
    CHECK(strstr(buffer, "\"amount_cents\":50") != NULL);
    CHECK(strstr(buffer, "\"passage_source\":\"sensor\"") != NULL);
    return 0;
}

static int test_buffer_measurement(void)
{
    parking_protocol_availability_message_t message;
    char small[12];
    char exact[512];
    size_t required_size = 0U;

    memset(&message, 0, sizeof(message));
    message.metadata = metadata("availability-1");
    message.state = PARKING_PROTOCOL_OFFLINE;

    CHECK(parking_protocol_encode_availability(
              &message, NULL, 0U, &required_size) ==
          PARKING_PROTOCOL_BUFFER_TOO_SMALL);
    CHECK(required_size > sizeof(small));
    CHECK(required_size <= sizeof(exact));

    memset(small, 'x', sizeof(small));
    CHECK(parking_protocol_encode_availability(
              &message, small, sizeof(small), NULL) ==
          PARKING_PROTOCOL_BUFFER_TOO_SMALL);
    CHECK(small[sizeof(small) - 1U] == '\0');

    CHECK(parking_protocol_encode_availability(
              &message, exact, required_size, NULL) ==
          PARKING_PROTOCOL_OK);
    CHECK(strlen(exact) + 1U == required_size);
    return 0;
}

static int test_invalid_values(void)
{
    const char invalid_utf8[] = {'\xE7', '\xB2', '\0'};
    parking_protocol_status_message_t status;
    parking_protocol_vehicle_event_t event;
    char buffer[1024];

    memset(&status, 0, sizeof(status));
    status.metadata = metadata("status-invalid");
    status.software_version = "1.0.0";
    status.lane_mode = PARKING_PROTOCOL_LANE_ENTRY;
    status.business_state = PARKING_PROTOCOL_BUSINESS_IDLE;
    status.gate_mode = PARKING_PROTOCOL_GATE_DRY_RUN;
    status.gate_state = PARKING_PROTOCOL_GATE_CLOSED;
    status.camera_state = PARKING_PROTOCOL_COMPONENT_OK;
    status.recognizer_state = PARKING_PROTOCOL_COMPONENT_OK;
    status.database_state = PARKING_PROTOCOL_COMPONENT_OK;
    status.spaces = spaces();

    status.metadata.timestamp_unix_us = 0U;
    CHECK(parking_protocol_encode_status(
              &status, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_INVALID_VALUE);
    status.metadata = metadata("status-invalid");
    status.spaces.available = 26U;
    CHECK(parking_protocol_encode_status(
              &status, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_INVALID_VALUE);

    memset(&event, 0, sizeof(event));
    event.metadata = metadata("event-invalid");
    event.event_type = PARKING_PROTOCOL_EVENT_ENTRY;
    event.operation_id = 1U;
    event.plate_number = invalid_utf8;
    event.plate_color = "蓝色";
    event.detect_confidence = 0.9F;
    event.passage_source = PARKING_PROTOCOL_PASSAGE_VISION_TIMEOUT;
    event.spaces = spaces();
    CHECK(parking_protocol_encode_vehicle_event(
              &event, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_INVALID_VALUE);

    event.plate_number = "浙A66666";
    event.detect_confidence = NAN;
    CHECK(parking_protocol_encode_vehicle_event(
              &event, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_INVALID_VALUE);

    event.detect_confidence = 0.9F;
    event.amount_cents = 1U;
    CHECK(parking_protocol_encode_vehicle_event(
              &event, buffer, sizeof(buffer), NULL) ==
          PARKING_PROTOCOL_INVALID_VALUE);
    return 0;
}

static int test_status_names(void)
{
    CHECK(strcmp(parking_protocol_status_name(PARKING_PROTOCOL_OK),
                 "OK") == 0);
    CHECK(strcmp(parking_protocol_status_name(PARKING_PROTOCOL_BAD_ARG),
                 "BAD_ARG") == 0);
    CHECK(strcmp(parking_protocol_status_name(
                     PARKING_PROTOCOL_INVALID_VALUE),
                 "INVALID_VALUE") == 0);
    CHECK(strcmp(parking_protocol_status_name(
                     PARKING_PROTOCOL_BUFFER_TOO_SMALL),
                 "BUFFER_TOO_SMALL") == 0);
    CHECK(strcmp(parking_protocol_status_name(
                     (parking_protocol_status_t)100),
                 "UNKNOWN") == 0);
    return 0;
}

int main(void)
{
    unsigned int passed = 0U;

    RUN_TEST(test_topics);
    RUN_TEST(test_availability_and_escaping);
    RUN_TEST(test_heartbeat);
    RUN_TEST(test_status_message);
    RUN_TEST(test_vehicle_events);
    RUN_TEST(test_buffer_measurement);
    RUN_TEST(test_invalid_values);
    RUN_TEST(test_status_names);

    printf("parking_protocol: all %u tests passed\n", passed);
    return 0;
}

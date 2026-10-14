#include "parking_mqtt_reporter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_CAPACITY 8U

typedef struct {
    char topic[PARKING_MQTT_TOPIC_CAPACITY];
    char payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    int qos;
    int retain;
} publish_record_t;

struct parking_mqtt {
    parking_mqtt_heartbeat_fn build_heartbeat;
    void *heartbeat_context;
    char will_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char will_payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    publish_record_t records[RECORD_CAPACITY];
    size_t record_count;
    int publish_result;
    int started;
};

static parking_mqtt_t *last_mqtt;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                             \
            return -1;                                                       \
        }                                                                    \
    } while (0)

parking_mqtt_t *parking_mqtt_create(const parking_mqtt_config_t *config)
{
    parking_mqtt_t *mqtt;

    if (config == NULL)
        return NULL;

    mqtt = calloc(1, sizeof(*mqtt));
    if (mqtt == NULL)
        return NULL;

    snprintf(mqtt->will_topic, sizeof(mqtt->will_topic), "%s",
             config->availability_topic);
    snprintf(mqtt->will_payload, sizeof(mqtt->will_payload), "%s",
             config->offline_payload);
    mqtt->build_heartbeat = config->build_heartbeat;
    mqtt->heartbeat_context = config->heartbeat_context;
    mqtt->publish_result = PARKING_MQTT_OK;
    last_mqtt = mqtt;
    return mqtt;
}

int parking_mqtt_start(parking_mqtt_t *mqtt)
{
    if (mqtt == NULL)
        return PARKING_MQTT_ERROR;

    mqtt->started = 1;
    return PARKING_MQTT_OK;
}

void parking_mqtt_stop(parking_mqtt_t *mqtt)
{
    if (mqtt != NULL)
        mqtt->started = 0;
}

int parking_mqtt_publish(parking_mqtt_t *mqtt,
                         const parking_mqtt_message_t *message)
{
    publish_record_t *record;

    if (mqtt == NULL || message == NULL)
        return PARKING_MQTT_ERROR;
    if (mqtt->publish_result != PARKING_MQTT_OK)
        return mqtt->publish_result;
    if (mqtt->record_count == RECORD_CAPACITY ||
        message->payload_length >= PARKING_MQTT_PAYLOAD_CAPACITY)
        return PARKING_MQTT_ERROR;

    record = &mqtt->records[mqtt->record_count++];
    snprintf(record->topic, sizeof(record->topic), "%s", message->topic);
    memcpy(record->payload, message->payload, message->payload_length);
    record->payload[message->payload_length] = '\0';
    record->qos = message->qos;
    record->retain = message->retain;
    return PARKING_MQTT_OK;
}

int parking_mqtt_get_stats(parking_mqtt_t *mqtt,
                           parking_mqtt_stats_t *stats)
{
    if (mqtt == NULL || stats == NULL)
        return PARKING_MQTT_ERROR;

    memset(stats, 0, sizeof(*stats));
    stats->running = mqtt->started;
    stats->enqueued_messages = mqtt->record_count;
    return PARKING_MQTT_OK;
}

int parking_mqtt_failed(parking_mqtt_t *mqtt)
{
    return mqtt == NULL;
}

int parking_mqtt_copy_last_error(parking_mqtt_t *mqtt,
                                 char *buffer,
                                 size_t capacity)
{
    if (mqtt == NULL || buffer == NULL || capacity == 0U)
        return PARKING_MQTT_ERROR;

    buffer[0] = '\0';
    return PARKING_MQTT_OK;
}

void parking_mqtt_destroy(parking_mqtt_t *mqtt)
{
    if (last_mqtt == mqtt)
        last_mqtt = NULL;
    free(mqtt);
}

static parking_protocol_spaces_t test_spaces(void)
{
    parking_protocol_spaces_t spaces;

    memset(&spaces, 0, sizeof(spaces));
    spaces.total = 100U;
    spaces.occupied = 20U;
    spaces.pending_entries = 1U;
    spaces.available = 79U;
    return spaces;
}

static int test_reporter_messages(void)
{
    parking_mqtt_reporter_config_t config;
    parking_mqtt_reporter_status_t status;
    parking_mqtt_reporter_vehicle_event_t event;
    parking_mqtt_reporter_t *reporter;
    char heartbeat[PARKING_MQTT_PAYLOAD_CAPACITY];
    size_t heartbeat_length = 0U;

    memset(&config, 0, sizeof(config));
    config.host = "broker";
    config.site_id = "lot-a";
    config.device_id = "gate01";
    config.software_version = "1.0.0";

    reporter = parking_mqtt_reporter_create(&config);
    CHECK(reporter != NULL);
    CHECK(last_mqtt != NULL);
    CHECK(strcmp(last_mqtt->will_topic,
                 "parking/v1/lot-a/gate01/availability") == 0);
    CHECK(strstr(last_mqtt->will_payload, "\"state\":\"offline\"") != NULL);
    CHECK(parking_mqtt_reporter_start(reporter) == PARKING_MQTT_OK);
    CHECK(last_mqtt->build_heartbeat(
              last_mqtt->heartbeat_context, 3U, 2000000U,
              heartbeat, sizeof(heartbeat), &heartbeat_length) == 0);
    CHECK(heartbeat_length == strlen(heartbeat));
    CHECK(strstr(heartbeat, "\"sequence\":3") != NULL);
    CHECK(strstr(heartbeat, "\"uptime_us\":2000000") != NULL);

    memset(&status, 0, sizeof(status));
    status.lane_mode = PARKING_PROTOCOL_LANE_ENTRY;
    status.business_state = PARKING_PROTOCOL_BUSINESS_IDLE;
    status.gate_mode = PARKING_PROTOCOL_GATE_DRY_RUN;
    status.gate_state = PARKING_PROTOCOL_GATE_CLOSED;
    status.camera_state = PARKING_PROTOCOL_COMPONENT_OK;
    status.recognizer_state = PARKING_PROTOCOL_COMPONENT_OK;
    status.database_state = PARKING_PROTOCOL_COMPONENT_OK;
    status.spaces = test_spaces();
    CHECK(parking_mqtt_reporter_publish_status(reporter, &status) ==
          PARKING_MQTT_OK);
    CHECK(last_mqtt->record_count == 1U);
    CHECK(strcmp(last_mqtt->records[0].topic,
                 "parking/v1/lot-a/gate01/status") == 0);
    CHECK(last_mqtt->records[0].qos == 1);
    CHECK(last_mqtt->records[0].retain == 1);
    CHECK(strstr(last_mqtt->records[0].payload,
                 "\"business_state\":\"idle\"") != NULL);

    memset(&event, 0, sizeof(event));
    event.event_type = PARKING_PROTOCOL_EVENT_ENTRY;
    event.operation_id = 42U;
    event.committed_at_unix_us = 1700000000000000ULL;
    event.plate_number = "浙A66666";
    event.plate_color = "蓝色";
    event.detect_confidence = 0.95F;
    event.passage_source = PARKING_PROTOCOL_PASSAGE_VISION_TIMEOUT;
    event.spaces = test_spaces();
    CHECK(parking_mqtt_reporter_publish_vehicle_event(reporter, &event) ==
          PARKING_MQTT_OK);
    CHECK(last_mqtt->record_count == 2U);
    CHECK(strcmp(last_mqtt->records[1].topic,
                 "parking/v1/lot-a/gate01/events/entry") == 0);
    CHECK(last_mqtt->records[1].retain == 0);
    CHECK(strstr(last_mqtt->records[1].payload,
                 "\"message_id\":\"entry-42\"") != NULL);
    CHECK(strstr(last_mqtt->records[1].payload,
                 "\"result\":\"committed\"") != NULL);

    event.event_type = PARKING_PROTOCOL_EVENT_EXIT;
    event.operation_id = 43U;
    event.amount_cents = 50U;
    event.passage_source = PARKING_PROTOCOL_PASSAGE_SENSOR;
    CHECK(parking_mqtt_reporter_publish_vehicle_event(reporter, &event) ==
          PARKING_MQTT_OK);
    CHECK(last_mqtt->record_count == 3U);
    CHECK(strcmp(last_mqtt->records[2].topic,
                 "parking/v1/lot-a/gate01/events/exit") == 0);
    CHECK(strstr(last_mqtt->records[2].payload,
                 "\"amount_cents\":50") != NULL);

    last_mqtt->publish_result = PARKING_MQTT_QUEUE_FULL;
    CHECK(parking_mqtt_reporter_publish_status(reporter, &status) ==
          PARKING_MQTT_QUEUE_FULL);

    parking_mqtt_reporter_destroy(reporter);
    return 0;
}

int main(void)
{
    if (test_reporter_messages() != 0)
        return 1;

    printf("parking_mqtt_reporter: all 1 tests passed\n");
    return 0;
}

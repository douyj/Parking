#include "parking_mqtt_reporter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct parking_mqtt_reporter {
    parking_mqtt_t *mqtt;
    char site_id[PARKING_PROTOCOL_TOPIC_LEVEL_MAX_LENGTH + 1U];
    char device_id[PARKING_PROTOCOL_TOPIC_LEVEL_MAX_LENGTH + 1U];
    char software_version[PARKING_PROTOCOL_VERSION_MAX_LENGTH + 1U];
    char session_id[PARKING_PROTOCOL_ID_MAX_LENGTH + 1U];
    char status_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char entry_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char exit_topic[PARKING_MQTT_TOPIC_CAPACITY];
    uint64_t session_started_us;
    uint64_t status_sequence;
};

static int realtime_us(uint64_t *value)
{
    struct timespec now;

    if (value == NULL || clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;

    *value = (uint64_t)now.tv_sec * 1000000ULL +
             (uint64_t)now.tv_nsec / 1000ULL;
    return 0;
}

static int copy_string(char *destination, size_t capacity,
                       const char *source)
{
    size_t length;

    if (destination == NULL || capacity == 0U || source == NULL)
        return -1;

    length = strlen(source);
    if (length == 0U || length >= capacity)
        return -1;

    memcpy(destination, source, length + 1U);
    return 0;
}

static uint64_t hash_identity(const char *site_id, const char *device_id)
{
    static const uint64_t offset_basis = 14695981039346656037ULL;
    static const uint64_t prime = 1099511628211ULL;
    const unsigned char *cursor;
    uint64_t hash = offset_basis;

    for (cursor = (const unsigned char *)site_id; *cursor != '\0'; ++cursor) {
        hash ^= *cursor;
        hash *= prime;
    }
    hash ^= 0xffU;
    hash *= prime;
    for (cursor = (const unsigned char *)device_id; *cursor != '\0'; ++cursor) {
        hash ^= *cursor;
        hash *= prime;
    }
    return hash;
}

static int make_message_id(char *buffer, size_t capacity,
                           const char *format,
                           unsigned long long first,
                           unsigned long long second)
{
    int length;

    length = snprintf(buffer, capacity, format, first, second);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

static void fill_metadata(parking_mqtt_reporter_t *reporter,
                          parking_protocol_metadata_t *metadata,
                          const char *message_id,
                          uint64_t timestamp_us)
{
    metadata->message_id = message_id;
    metadata->site_id = reporter->site_id;
    metadata->device_id = reporter->device_id;
    metadata->session_id = reporter->session_id;
    metadata->timestamp_unix_us = timestamp_us;
}

static int build_availability(parking_mqtt_reporter_t *reporter,
                              parking_protocol_availability_t state,
                              char *payload,
                              size_t capacity)
{
    parking_protocol_availability_message_t message;
    char message_id[PARKING_PROTOCOL_ID_MAX_LENGTH + 1U];
    uint64_t timestamp_us;
    int length;

    if (realtime_us(&timestamp_us) != 0)
        return -1;

    length = snprintf(message_id, sizeof(message_id), "%s-%s",
                      reporter->session_id,
                      state == PARKING_PROTOCOL_ONLINE
                          ? "online" : "offline");
    if (length <= 0 || (size_t)length >= sizeof(message_id))
        return -1;

    memset(&message, 0, sizeof(message));
    fill_metadata(reporter, &message.metadata, message_id, timestamp_us);
    message.state = state;
    return parking_protocol_encode_availability(
               &message, payload, capacity, NULL) == PARKING_PROTOCOL_OK
        ? 0 : -1;
}

static int build_heartbeat(void *context,
                           uint64_t sequence,
                           uint64_t uptime_us,
                           char *payload,
                           size_t capacity,
                           size_t *payload_length)
{
    parking_mqtt_reporter_t *reporter = context;
    parking_protocol_heartbeat_message_t message;
    char message_id[PARKING_PROTOCOL_ID_MAX_LENGTH + 1U];
    uint64_t timestamp_us;
    size_t required_size;
    int length;

    if (reporter == NULL || payload_length == NULL ||
        realtime_us(&timestamp_us) != 0)
        return -1;

    length = snprintf(message_id, sizeof(message_id), "hb-%s-%llu",
                      reporter->session_id,
                      (unsigned long long)sequence);
    if (length <= 0 || (size_t)length >= sizeof(message_id))
        return -1;

    memset(&message, 0, sizeof(message));
    fill_metadata(reporter, &message.metadata, message_id, timestamp_us);
    message.sequence = sequence;
    message.uptime_us = uptime_us;
    if (parking_protocol_encode_heartbeat(
            &message, payload, capacity, &required_size) !=
            PARKING_PROTOCOL_OK || required_size == 0U)
        return -1;

    *payload_length = required_size - 1U;
    return 0;
}

static int publish_payload(parking_mqtt_reporter_t *reporter,
                           const char *topic,
                           const char *payload,
                           int retain)
{
    parking_mqtt_message_t message;

    memset(&message, 0, sizeof(message));
    message.topic = topic;
    message.payload = payload;
    message.payload_length = strlen(payload);
    message.qos = 1;
    message.retain = retain;
    return parking_mqtt_publish(reporter->mqtt, &message);
}

parking_mqtt_reporter_t *parking_mqtt_reporter_create(
    const parking_mqtt_reporter_config_t *config)
{
    parking_mqtt_reporter_t *reporter;
    parking_mqtt_config_t mqtt_config;
    char availability_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char heartbeat_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char online_payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    char offline_payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    char client_id[PARKING_MQTT_CLIENT_ID_CAPACITY];
    uint64_t timestamp_us;
    int length;

    if (config == NULL || config->host == NULL || config->host[0] == '\0')
        return NULL;

    reporter = calloc(1, sizeof(*reporter));
    if (reporter == NULL)
        return NULL;

    if (copy_string(reporter->site_id, sizeof(reporter->site_id),
                    config->site_id) != 0 ||
        copy_string(reporter->device_id, sizeof(reporter->device_id),
                    config->device_id) != 0 ||
        copy_string(reporter->software_version,
                    sizeof(reporter->software_version),
                    config->software_version) != 0 ||
        realtime_us(&timestamp_us) != 0)
        goto fail;
    reporter->session_started_us = timestamp_us;

    length = snprintf(reporter->session_id, sizeof(reporter->session_id),
                      "session-%llu-%ld", (unsigned long long)timestamp_us,
                      (long)getpid());
    if (length <= 0 || (size_t)length >= sizeof(reporter->session_id))
        goto fail;

    length = snprintf(client_id, sizeof(client_id), "parking-%016llx",
                      (unsigned long long)hash_identity(
                          reporter->site_id, reporter->device_id));
    if (length <= 0 || (size_t)length >= sizeof(client_id))
        goto fail;

    if (parking_protocol_build_topic(
            reporter->site_id, reporter->device_id,
            PARKING_PROTOCOL_TOPIC_AVAILABILITY, availability_topic,
            sizeof(availability_topic), NULL) != PARKING_PROTOCOL_OK ||
        parking_protocol_build_topic(
            reporter->site_id, reporter->device_id,
            PARKING_PROTOCOL_TOPIC_HEARTBEAT, heartbeat_topic,
            sizeof(heartbeat_topic), NULL) != PARKING_PROTOCOL_OK ||
        parking_protocol_build_topic(
            reporter->site_id, reporter->device_id,
            PARKING_PROTOCOL_TOPIC_STATUS, reporter->status_topic,
            sizeof(reporter->status_topic), NULL) != PARKING_PROTOCOL_OK ||
        parking_protocol_build_topic(
            reporter->site_id, reporter->device_id,
            PARKING_PROTOCOL_TOPIC_EVENT_ENTRY, reporter->entry_topic,
            sizeof(reporter->entry_topic), NULL) != PARKING_PROTOCOL_OK ||
        parking_protocol_build_topic(
            reporter->site_id, reporter->device_id,
            PARKING_PROTOCOL_TOPIC_EVENT_EXIT, reporter->exit_topic,
            sizeof(reporter->exit_topic), NULL) != PARKING_PROTOCOL_OK ||
        build_availability(reporter, PARKING_PROTOCOL_ONLINE,
                           online_payload, sizeof(online_payload)) != 0 ||
        build_availability(reporter, PARKING_PROTOCOL_OFFLINE,
                           offline_payload, sizeof(offline_payload)) != 0)
        goto fail;

    memset(&mqtt_config, 0, sizeof(mqtt_config));
    mqtt_config.host = config->host;
    mqtt_config.port = config->port;
    mqtt_config.client_id = client_id;
    mqtt_config.username = config->username;
    mqtt_config.password = config->password;
    mqtt_config.ca_file = config->ca_file;
    mqtt_config.availability_topic = availability_topic;
    mqtt_config.online_payload = online_payload;
    mqtt_config.offline_payload = offline_payload;
    mqtt_config.heartbeat_topic = heartbeat_topic;
    mqtt_config.heartbeat_interval_ms = config->heartbeat_interval_ms != 0U
        ? config->heartbeat_interval_ms
        : PARKING_MQTT_REPORTER_DEFAULT_HEARTBEAT_MS;
    mqtt_config.build_heartbeat = build_heartbeat;
    mqtt_config.heartbeat_context = reporter;

    reporter->mqtt = parking_mqtt_create(&mqtt_config);
    if (reporter->mqtt == NULL)
        goto fail;

    return reporter;

fail:
    parking_mqtt_reporter_destroy(reporter);
    return NULL;
}

int parking_mqtt_reporter_start(parking_mqtt_reporter_t *reporter)
{
    if (reporter == NULL)
        return PARKING_MQTT_ERROR;

    return parking_mqtt_start(reporter->mqtt);
}

void parking_mqtt_reporter_stop(parking_mqtt_reporter_t *reporter)
{
    if (reporter != NULL)
        parking_mqtt_stop(reporter->mqtt);
}

int parking_mqtt_reporter_publish_status(
    parking_mqtt_reporter_t *reporter,
    const parking_mqtt_reporter_status_t *status)
{
    parking_protocol_status_message_t message;
    char message_id[PARKING_PROTOCOL_ID_MAX_LENGTH + 1U];
    char payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    uint64_t timestamp_us;
    uint64_t sequence;
    int result;

    if (reporter == NULL || status == NULL || realtime_us(&timestamp_us) != 0)
        return PARKING_MQTT_ERROR;

    sequence = reporter->status_sequence + 1U;
    if (make_message_id(message_id, sizeof(message_id), "status-%llu-%llu",
                        (unsigned long long)reporter->session_started_us,
                        (unsigned long long)sequence) != 0)
        return PARKING_MQTT_ERROR;

    memset(&message, 0, sizeof(message));
    fill_metadata(reporter, &message.metadata, message_id, timestamp_us);
    message.software_version = reporter->software_version;
    message.lane_mode = status->lane_mode;
    message.business_state = status->business_state;
    message.gate_mode = status->gate_mode;
    message.gate_state = status->gate_state;
    message.camera_state = status->camera_state;
    message.recognizer_state = status->recognizer_state;
    message.database_state = status->database_state;
    message.spaces = status->spaces;
    if (parking_protocol_encode_status(
            &message, payload, sizeof(payload), NULL) != PARKING_PROTOCOL_OK)
        return PARKING_MQTT_ERROR;

    result = publish_payload(reporter, reporter->status_topic, payload, 1);
    if (result == PARKING_MQTT_OK)
        reporter->status_sequence = sequence;
    return result;
}

int parking_mqtt_reporter_publish_vehicle_event(
    parking_mqtt_reporter_t *reporter,
    const parking_mqtt_reporter_vehicle_event_t *event)
{
    parking_protocol_vehicle_event_t message;
    char message_id[PARKING_PROTOCOL_ID_MAX_LENGTH + 1U];
    char payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    const char *topic;
    const char *prefix;
    int length;

    if (reporter == NULL || event == NULL)
        return PARKING_MQTT_ERROR;

    prefix = event->event_type == PARKING_PROTOCOL_EVENT_ENTRY
        ? "entry" : "exit";
    topic = event->event_type == PARKING_PROTOCOL_EVENT_ENTRY
        ? reporter->entry_topic : reporter->exit_topic;
    length = snprintf(message_id, sizeof(message_id), "%s-%llu", prefix,
                      (unsigned long long)event->operation_id);
    if (length <= 0 || (size_t)length >= sizeof(message_id))
        return PARKING_MQTT_ERROR;

    memset(&message, 0, sizeof(message));
    fill_metadata(reporter, &message.metadata, message_id,
                  event->committed_at_unix_us);
    message.event_type = event->event_type;
    message.operation_id = event->operation_id;
    message.plate_number = event->plate_number;
    message.plate_color = event->plate_color;
    message.detect_confidence = event->detect_confidence;
    message.amount_cents = event->amount_cents;
    message.passage_source = event->passage_source;
    message.spaces = event->spaces;
    if (parking_protocol_encode_vehicle_event(
            &message, payload, sizeof(payload), NULL) != PARKING_PROTOCOL_OK)
        return PARKING_MQTT_ERROR;

    return publish_payload(reporter, topic, payload, 0);
}

int parking_mqtt_reporter_get_stats(
    parking_mqtt_reporter_t *reporter,
    parking_mqtt_stats_t *stats)
{
    if (reporter == NULL)
        return PARKING_MQTT_ERROR;

    return parking_mqtt_get_stats(reporter->mqtt, stats);
}

int parking_mqtt_reporter_copy_last_error(
    parking_mqtt_reporter_t *reporter,
    char *buffer,
    size_t capacity)
{
    if (reporter == NULL)
        return PARKING_MQTT_ERROR;

    return parking_mqtt_copy_last_error(reporter->mqtt, buffer, capacity);
}

void parking_mqtt_reporter_destroy(parking_mqtt_reporter_t *reporter)
{
    if (reporter == NULL)
        return;

    parking_mqtt_destroy(reporter->mqtt);
    free(reporter);
}

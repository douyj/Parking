#include "parking_mqtt.h"
#include "parking_mqtt_transport.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAKE_RECORD_CAPACITY 128U
#define FAKE_ACK_CAPACITY 128U
#define TEST_TIMEOUT_MS 3000U

typedef struct {
    char topic[PARKING_MQTT_TOPIC_CAPACITY];
    char payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    int qos;
    int retain;
} fake_record_t;

struct parking_mqtt_transport {
    pthread_mutex_t mutex;
    parking_mqtt_transport_callbacks_t callbacks;
    void *callback_context;
    fake_record_t records[FAKE_RECORD_CAPACITY];
    int pending_acks[FAKE_ACK_CAPACITY];
    size_t record_count;
    size_t ack_head;
    size_t ack_count;
    int next_message_id;
    int connect_attempts;
    int fail_connect_attempts;
    int pending_connect;
    int force_disconnect;
    int connected;
    int hold_application_acks;
    char availability_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char will_payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    char last_error[128];
};

static parking_mqtt_transport_t *last_transport;
static int configured_connect_failures;

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec duration;

    duration.tv_sec = milliseconds / 1000U;
    duration.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&duration, NULL);
}

static int fake_create(
    const parking_mqtt_transport_config_t *config,
    const parking_mqtt_transport_callbacks_t *callbacks,
    void *callback_context,
    parking_mqtt_transport_t **output)
{
    parking_mqtt_transport_t *transport;

    if (config == NULL || callbacks == NULL || output == NULL)
        return -1;

    transport = calloc(1, sizeof(*transport));
    if (transport == NULL)
        return -1;
    if (pthread_mutex_init(&transport->mutex, NULL) != 0) {
        free(transport);
        return -1;
    }

    transport->callbacks = *callbacks;
    transport->callback_context = callback_context;
    transport->next_message_id = 1;
    transport->fail_connect_attempts = configured_connect_failures;
    snprintf(transport->availability_topic,
             sizeof(transport->availability_topic), "%s",
             config->will_topic);
    if (config->will_payload_length >= sizeof(transport->will_payload)) {
        pthread_mutex_destroy(&transport->mutex);
        free(transport);
        return -1;
    }
    memcpy(transport->will_payload, config->will_payload,
           config->will_payload_length);
    transport->will_payload[config->will_payload_length] = '\0';
    last_transport = transport;
    *output = transport;
    return 0;
}

static int fake_connect(parking_mqtt_transport_t *transport)
{
    int result = 0;

    pthread_mutex_lock(&transport->mutex);
    ++transport->connect_attempts;
    if (transport->connect_attempts <= transport->fail_connect_attempts) {
        snprintf(transport->last_error, sizeof(transport->last_error),
                 "模拟连接失败");
        result = -1;
    } else {
        transport->pending_connect = 1;
    }
    pthread_mutex_unlock(&transport->mutex);
    return result;
}

static int queue_ack_locked(parking_mqtt_transport_t *transport,
                            int message_id)
{
    size_t index;

    if (transport->ack_count == FAKE_ACK_CAPACITY)
        return -1;

    index = (transport->ack_head + transport->ack_count) %
            FAKE_ACK_CAPACITY;
    transport->pending_acks[index] = message_id;
    ++transport->ack_count;
    return 0;
}

static int fake_publish(parking_mqtt_transport_t *transport,
                        const parking_mqtt_message_t *message,
                        int *message_id)
{
    fake_record_t *record;
    int is_application;

    pthread_mutex_lock(&transport->mutex);
    if (!transport->connected ||
        transport->record_count == FAKE_RECORD_CAPACITY ||
        message->payload_length >= PARKING_MQTT_PAYLOAD_CAPACITY) {
        pthread_mutex_unlock(&transport->mutex);
        return -1;
    }

    record = &transport->records[transport->record_count++];
    snprintf(record->topic, sizeof(record->topic), "%s", message->topic);
    memcpy(record->payload, message->payload, message->payload_length);
    record->payload[message->payload_length] = '\0';
    record->qos = message->qos;
    record->retain = message->retain;

    *message_id = transport->next_message_id++;
    is_application = strcmp(message->topic,
                            transport->availability_topic) != 0 &&
                     strstr(message->topic, "/heartbeat") == NULL;
    if (!is_application || !transport->hold_application_acks) {
        if (queue_ack_locked(transport, *message_id) != 0) {
            pthread_mutex_unlock(&transport->mutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&transport->mutex);
    return 0;
}

static int fake_loop(parking_mqtt_transport_t *transport,
                     unsigned int timeout_ms)
{
    int notify_connect = 0;
    int notify_disconnect = 0;
    int notify_publish = 0;
    int message_id = 0;

    pthread_mutex_lock(&transport->mutex);
    if (transport->pending_connect) {
        transport->pending_connect = 0;
        transport->connected = 1;
        notify_connect = 1;
    }
    if (transport->force_disconnect) {
        transport->force_disconnect = 0;
        transport->connected = 0;
        notify_disconnect = 1;
    }
    if (transport->ack_count > 0U && !notify_disconnect) {
        message_id = transport->pending_acks[transport->ack_head];
        transport->ack_head = (transport->ack_head + 1U) %
                              FAKE_ACK_CAPACITY;
        --transport->ack_count;
        notify_publish = 1;
    }
    pthread_mutex_unlock(&transport->mutex);

    if (notify_connect)
        transport->callbacks.connected(transport->callback_context, 0);
    if (notify_disconnect)
        transport->callbacks.disconnected(transport->callback_context, 1);
    if (notify_publish)
        transport->callbacks.published(
            transport->callback_context, message_id);

    sleep_ms(timeout_ms > 2U ? 2U : timeout_ms);
    return 0;
}

static int fake_disconnect(parking_mqtt_transport_t *transport)
{
    pthread_mutex_lock(&transport->mutex);
    transport->connected = 0;
    pthread_mutex_unlock(&transport->mutex);
    return 0;
}

static int fake_copy_last_error(parking_mqtt_transport_t *transport,
                                char *buffer,
                                size_t capacity)
{
    pthread_mutex_lock(&transport->mutex);
    snprintf(buffer, capacity, "%s", transport->last_error);
    pthread_mutex_unlock(&transport->mutex);
    return 0;
}

static void fake_destroy(parking_mqtt_transport_t *transport)
{
    if (transport == NULL)
        return;

    if (last_transport == transport)
        last_transport = NULL;
    pthread_mutex_destroy(&transport->mutex);
    free(transport);
}

static const parking_mqtt_transport_ops_t fake_operations = {
    fake_create,
    fake_connect,
    fake_publish,
    fake_loop,
    fake_disconnect,
    fake_copy_last_error,
    fake_destroy
};

static int build_heartbeat(void *context, uint64_t sequence,
                           uint64_t uptime_us, char *payload,
                           size_t capacity, size_t *payload_length)
{
    int length;

    (void)context;
    length = snprintf(payload, capacity,
                      "{\"sequence\":%llu,\"uptime_us\":%llu}",
                      (unsigned long long)sequence,
                      (unsigned long long)uptime_us);
    if (length <= 0 || (size_t)length >= capacity)
        return -1;

    *payload_length = (size_t)length;
    return 0;
}

static void fill_config(parking_mqtt_config_t *config,
                        size_t queue_capacity)
{
    memset(config, 0, sizeof(*config));
    config->host = "broker.test";
    config->port = 1883U;
    config->client_id = "parking-test";
    config->availability_topic = "parking/v1/test/device/availability";
    config->online_payload = "{\"state\":\"online\"}";
    config->offline_payload = "{\"state\":\"offline\"}";
    config->heartbeat_topic = "parking/v1/test/device/heartbeat";
    config->heartbeat_interval_ms = 10U;
    config->build_heartbeat = build_heartbeat;
    config->queue_capacity = queue_capacity;
    config->reconnect_initial_ms = 5U;
    config->reconnect_max_ms = 20U;
    config->loop_timeout_ms = 1U;
    config->shutdown_timeout_ms = 100U;
}

static int wait_for_stats(parking_mqtt_t *mqtt,
                          int (*predicate)(const parking_mqtt_stats_t *))
{
    unsigned int elapsed;

    for (elapsed = 0U; elapsed < TEST_TIMEOUT_MS; ++elapsed) {
        parking_mqtt_stats_t stats;

        memset(&stats, 0, sizeof(stats));
        if (parking_mqtt_get_stats(mqtt, &stats) != PARKING_MQTT_OK)
            return -1;
        if (predicate(&stats))
            return 0;
        sleep_ms(1U);
    }

    return -1;
}

static int connected_with_heartbeat(const parking_mqtt_stats_t *stats)
{
    return stats->connected && stats->heartbeat_messages > 0U;
}

static int one_message_acknowledged(const parking_mqtt_stats_t *stats)
{
    return stats->acknowledged_messages >= 1U;
}

static int one_disconnect_seen(const parking_mqtt_stats_t *stats)
{
    return stats->disconnects >= 1U;
}

static size_t count_payload(parking_mqtt_transport_t *transport,
                            const char *payload)
{
    size_t count = 0U;
    size_t i;

    pthread_mutex_lock(&transport->mutex);
    for (i = 0U; i < transport->record_count; ++i) {
        if (strcmp(transport->records[i].payload, payload) == 0)
            ++count;
    }
    pthread_mutex_unlock(&transport->mutex);
    return count;
}

static int publish_text(parking_mqtt_t *mqtt, const char *payload)
{
    parking_mqtt_message_t message;

    memset(&message, 0, sizeof(message));
    message.topic = "parking/v1/test/device/events/entry";
    message.payload = payload;
    message.payload_length = strlen(payload);
    message.qos = 1;
    return parking_mqtt_publish(mqtt, &message);
}

static int test_reconnect_heartbeat_and_shutdown(void)
{
    parking_mqtt_config_t config;
    parking_mqtt_stats_t stats;
    parking_mqtt_t *mqtt;
    int failed = 0;

    configured_connect_failures = 2;
    fill_config(&config, 4U);
    mqtt = parking_mqtt_create_with_transport(&config, &fake_operations);
    if (mqtt == NULL || parking_mqtt_start(mqtt) != PARKING_MQTT_OK)
        failed = 1;
    if (!failed && wait_for_stats(mqtt, connected_with_heartbeat) != 0)
        failed = 1;
    if (!failed && publish_text(mqtt, "event-a") != PARKING_MQTT_OK)
        failed = 1;
    if (!failed && wait_for_stats(mqtt, one_message_acknowledged) != 0)
        failed = 1;

    parking_mqtt_stop(mqtt);
    memset(&stats, 0, sizeof(stats));
    if (mqtt == NULL || parking_mqtt_get_stats(mqtt, &stats) !=
                            PARKING_MQTT_OK ||
        stats.connect_attempts < 3U || stats.successful_connections != 1U ||
        stats.heartbeat_messages == 0U ||
        count_payload(last_transport, config.online_payload) == 0U ||
        count_payload(last_transport, config.offline_payload) == 0U)
        failed = 1;

    parking_mqtt_destroy(mqtt);
    return failed ? -1 : 0;
}

static int test_bounded_queue(void)
{
    parking_mqtt_config_t config;
    parking_mqtt_stats_t stats;
    parking_mqtt_t *mqtt;
    int failed = 0;

    configured_connect_failures = 0;
    fill_config(&config, 2U);
    mqtt = parking_mqtt_create_with_transport(&config, &fake_operations);
    if (mqtt == NULL || parking_mqtt_start(mqtt) != PARKING_MQTT_OK)
        failed = 1;
    if (!failed && wait_for_stats(mqtt, connected_with_heartbeat) != 0)
        failed = 1;

    if (!failed) {
        pthread_mutex_lock(&last_transport->mutex);
        last_transport->hold_application_acks = 1;
        pthread_mutex_unlock(&last_transport->mutex);
    }
    if (!failed && publish_text(mqtt, "event-1") != PARKING_MQTT_OK)
        failed = 1;
    if (!failed && publish_text(mqtt, "event-2") != PARKING_MQTT_OK)
        failed = 1;
    if (!failed && publish_text(mqtt, "event-3") !=
                       PARKING_MQTT_QUEUE_FULL)
        failed = 1;

    memset(&stats, 0, sizeof(stats));
    if (!failed &&
        (parking_mqtt_get_stats(mqtt, &stats) != PARKING_MQTT_OK ||
         stats.queue_depth != 2U || stats.queue_high_watermark != 2U ||
         stats.rejected_messages != 1U))
        failed = 1;

    parking_mqtt_destroy(mqtt);
    return failed ? -1 : 0;
}

static int test_qos1_retry_after_disconnect(void)
{
    parking_mqtt_config_t config;
    parking_mqtt_stats_t stats;
    parking_mqtt_t *mqtt;
    unsigned int elapsed;
    int failed = 0;

    configured_connect_failures = 0;
    fill_config(&config, 4U);
    mqtt = parking_mqtt_create_with_transport(&config, &fake_operations);
    if (mqtt == NULL || parking_mqtt_start(mqtt) != PARKING_MQTT_OK)
        failed = 1;
    if (!failed && wait_for_stats(mqtt, connected_with_heartbeat) != 0)
        failed = 1;

    if (!failed) {
        pthread_mutex_lock(&last_transport->mutex);
        last_transport->hold_application_acks = 1;
        pthread_mutex_unlock(&last_transport->mutex);
    }
    if (!failed && publish_text(mqtt, "retry-event") != PARKING_MQTT_OK)
        failed = 1;

    for (elapsed = 0U; !failed && elapsed < TEST_TIMEOUT_MS; ++elapsed) {
        if (count_payload(last_transport, "retry-event") >= 1U)
            break;
        sleep_ms(1U);
    }
    if (!failed && elapsed == TEST_TIMEOUT_MS)
        failed = 1;

    if (!failed) {
        pthread_mutex_lock(&last_transport->mutex);
        last_transport->hold_application_acks = 0;
        last_transport->force_disconnect = 1;
        pthread_mutex_unlock(&last_transport->mutex);
    }
    if (!failed && wait_for_stats(mqtt, one_disconnect_seen) != 0)
        failed = 1;
    if (!failed && wait_for_stats(mqtt, one_message_acknowledged) != 0)
        failed = 1;

    memset(&stats, 0, sizeof(stats));
    if (!failed &&
        (parking_mqtt_get_stats(mqtt, &stats) != PARKING_MQTT_OK ||
         stats.retried_messages == 0U ||
         count_payload(last_transport, "retry-event") < 2U))
        failed = 1;

    parking_mqtt_destroy(mqtt);
    return failed ? -1 : 0;
}

int main(void)
{
    unsigned int passed = 0U;

    if (test_reconnect_heartbeat_and_shutdown() != 0) {
        fprintf(stderr, "parking_mqtt: reconnect/heartbeat test failed\n");
        return 1;
    }
    ++passed;

    if (test_bounded_queue() != 0) {
        fprintf(stderr, "parking_mqtt: bounded queue test failed\n");
        return 1;
    }
    ++passed;

    if (test_qos1_retry_after_disconnect() != 0) {
        fprintf(stderr, "parking_mqtt: QoS 1 retry test failed\n");
        return 1;
    }
    ++passed;

    printf("parking_mqtt: all %u tests passed\n", passed);
    return 0;
}

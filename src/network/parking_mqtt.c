#include "parking_mqtt.h"
#include "parking_mqtt_transport.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PARKING_MQTT_ERROR_CAPACITY 256U
#define PARKING_MQTT_MAX_QUEUE_CAPACITY 1024U

typedef struct {
    char topic[PARKING_MQTT_TOPIC_CAPACITY];
    unsigned char payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    size_t payload_length;
    int qos;
    int retain;
} parking_mqtt_queue_item_t;

struct parking_mqtt {
    parking_mqtt_config_t config;
    char host[PARKING_MQTT_HOST_CAPACITY];
    char client_id[PARKING_MQTT_CLIENT_ID_CAPACITY];
    char username[PARKING_MQTT_CREDENTIAL_CAPACITY];
    char password[PARKING_MQTT_CREDENTIAL_CAPACITY];
    char ca_file[PARKING_MQTT_PATH_CAPACITY];
    char availability_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char online_payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    char offline_payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    char heartbeat_topic[PARKING_MQTT_TOPIC_CAPACITY];

    const parking_mqtt_transport_ops_t *transport_ops;
    parking_mqtt_transport_t *transport;

    parking_mqtt_queue_item_t *queue;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;

    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t worker_thread;

    parking_mqtt_stats_t stats;
    uint64_t started_us;
    uint64_t next_reconnect_us;
    uint64_t reconnect_delay_ms;
    uint64_t next_heartbeat_us;
    uint64_t heartbeat_sequence;

    int queue_inflight;
    int queue_message_id;
    int online_pending;
    int online_inflight;
    int online_message_id;
    int offline_inflight;
    int offline_message_id;
    int offline_acknowledged;

    int connecting;
    int reconnect_scheduled;
    int stop_requested;
    int closed;
    int thread_started;
    int mutex_initialized;
    int condition_initialized;
    char last_error[PARKING_MQTT_ERROR_CAPACITY];
};

static int monotonic_us(uint64_t *value)
{
    struct timespec now;

    if (value == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;

    *value = (uint64_t)now.tv_sec * 1000000ULL +
             (uint64_t)now.tv_nsec / 1000ULL;
    return 0;
}

static int copy_string(char *destination, size_t capacity,
                       const char *source, int allow_empty)
{
    size_t length;

    if (destination == NULL || capacity == 0U || source == NULL)
        return -1;

    length = strlen(source);
    if ((!allow_empty && length == 0U) || length >= capacity)
        return -1;

    memcpy(destination, source, length + 1U);
    return 0;
}

static int valid_qos(int qos)
{
    return qos == 0 || qos == 1;
}

static int validate_message(const parking_mqtt_message_t *message)
{
    if (message == NULL || message->topic == NULL ||
        message->topic[0] == '\0' || message->payload == NULL ||
        message->payload_length == 0U ||
        strlen(message->topic) >= PARKING_MQTT_TOPIC_CAPACITY ||
        message->payload_length >= PARKING_MQTT_PAYLOAD_CAPACITY ||
        !valid_qos(message->qos) ||
        (message->retain != 0 && message->retain != 1))
        return -1;

    return 0;
}

static int validate_config(const parking_mqtt_config_t *config)
{
    size_t queue_capacity;
    uint64_t reconnect_initial_ms;
    uint64_t reconnect_max_ms;

    if (config == NULL || config->host == NULL ||
        config->client_id == NULL ||
        config->availability_topic == NULL ||
        config->online_payload == NULL ||
        config->offline_payload == NULL ||
        config->heartbeat_topic == NULL ||
        config->build_heartbeat == NULL ||
        config->heartbeat_interval_ms == 0U)
        return -1;

    if (config->host[0] == '\0' || config->client_id[0] == '\0' ||
        config->availability_topic[0] == '\0' ||
        config->online_payload[0] == '\0' ||
        config->offline_payload[0] == '\0' ||
        config->heartbeat_topic[0] == '\0')
        return -1;

    if (config->password != NULL && config->password[0] != '\0' &&
        (config->username == NULL || config->username[0] == '\0'))
        return -1;

    queue_capacity = config->queue_capacity != 0U
        ? config->queue_capacity : PARKING_MQTT_DEFAULT_QUEUE_CAPACITY;
    if (queue_capacity > PARKING_MQTT_MAX_QUEUE_CAPACITY)
        return -1;

    reconnect_initial_ms = config->reconnect_initial_ms != 0U
        ? config->reconnect_initial_ms
        : PARKING_MQTT_DEFAULT_RECONNECT_INITIAL_MS;
    reconnect_max_ms = config->reconnect_max_ms != 0U
        ? config->reconnect_max_ms
        : PARKING_MQTT_DEFAULT_RECONNECT_MAX_MS;
    if (reconnect_initial_ms > reconnect_max_ms)
        return -1;

    return 0;
}

static void set_error_locked(parking_mqtt_t *mqtt, const char *message)
{
    if (message == NULL)
        message = "未知 MQTT 错误";

    snprintf(mqtt->last_error, sizeof(mqtt->last_error), "%s", message);
}

static void copy_transport_error(parking_mqtt_t *mqtt,
                                 const char *fallback)
{
    char message[PARKING_MQTT_ERROR_CAPACITY];

    memset(message, 0, sizeof(message));
    if (mqtt->transport_ops->copy_last_error(
            mqtt->transport, message, sizeof(message)) != 0 ||
        message[0] == '\0') {
        snprintf(message, sizeof(message), "%s", fallback);
    }

    pthread_mutex_lock(&mqtt->mutex);
    set_error_locked(mqtt, message);
    pthread_mutex_unlock(&mqtt->mutex);
}

static void schedule_reconnect_locked(parking_mqtt_t *mqtt,
                                      uint64_t now_us)
{
    uint64_t delay_us;

    if (mqtt->stop_requested || mqtt->reconnect_scheduled)
        return;

    if (mqtt->reconnect_delay_ms > UINT64_MAX / 1000ULL)
        delay_us = UINT64_MAX;
    else
        delay_us = mqtt->reconnect_delay_ms * 1000ULL;

    mqtt->next_reconnect_us = now_us > UINT64_MAX - delay_us
        ? UINT64_MAX : now_us + delay_us;
    mqtt->reconnect_scheduled = 1;
    ++mqtt->stats.reconnect_waits;

    if (mqtt->reconnect_delay_ms < mqtt->config.reconnect_max_ms) {
        uint64_t doubled = mqtt->reconnect_delay_ms > UINT64_MAX / 2U
            ? UINT64_MAX : mqtt->reconnect_delay_ms * 2U;

        mqtt->reconnect_delay_ms =
            doubled < mqtt->config.reconnect_max_ms
                ? doubled : mqtt->config.reconnect_max_ms;
    }
}

static void mark_disconnected_locked(parking_mqtt_t *mqtt,
                                     uint64_t now_us)
{
    if (mqtt->stats.connected)
        ++mqtt->stats.disconnects;

    mqtt->stats.connected = 0;
    mqtt->connecting = 0;
    mqtt->online_inflight = 0;
    mqtt->online_pending = 1;

    if (mqtt->queue_inflight) {
        mqtt->queue_inflight = 0;
        ++mqtt->stats.retried_messages;
    }

    schedule_reconnect_locked(mqtt, now_us);
}

static void transport_connected(void *context, int result)
{
    parking_mqtt_t *mqtt = context;
    uint64_t now_us = 0;

    if (mqtt == NULL)
        return;
    monotonic_us(&now_us);

    pthread_mutex_lock(&mqtt->mutex);
    mqtt->connecting = 0;
    if (result == 0) {
        mqtt->stats.connected = 1;
        ++mqtt->stats.successful_connections;
        mqtt->reconnect_delay_ms = mqtt->config.reconnect_initial_ms;
        mqtt->reconnect_scheduled = 0;
        mqtt->online_pending = 1;
        mqtt->online_inflight = 0;
        mqtt->next_heartbeat_us = now_us;
        mqtt->last_error[0] = '\0';
    } else {
        set_error_locked(mqtt, "MQTT Broker 拒绝连接");
        mark_disconnected_locked(mqtt, now_us);
    }
    pthread_cond_broadcast(&mqtt->condition);
    pthread_mutex_unlock(&mqtt->mutex);
}

static void transport_disconnected(void *context, int result)
{
    parking_mqtt_t *mqtt = context;
    uint64_t now_us = 0;

    (void)result;
    if (mqtt == NULL)
        return;
    monotonic_us(&now_us);

    pthread_mutex_lock(&mqtt->mutex);
    mark_disconnected_locked(mqtt, now_us);
    pthread_cond_broadcast(&mqtt->condition);
    pthread_mutex_unlock(&mqtt->mutex);
}

static void pop_queue_locked(parking_mqtt_t *mqtt)
{
    parking_mqtt_queue_item_t *item;

    if (mqtt->queue_count == 0U)
        return;

    item = &mqtt->queue[mqtt->queue_head];
    memset(item, 0, sizeof(*item));
    mqtt->queue_head = (mqtt->queue_head + 1U) % mqtt->stats.queue_capacity;
    --mqtt->queue_count;
    mqtt->stats.queue_depth = mqtt->queue_count;
    ++mqtt->stats.acknowledged_messages;
}

static void transport_published(void *context, int message_id)
{
    parking_mqtt_t *mqtt = context;

    if (mqtt == NULL)
        return;

    pthread_mutex_lock(&mqtt->mutex);
    if (mqtt->offline_inflight &&
        message_id == mqtt->offline_message_id) {
        mqtt->offline_inflight = 0;
        mqtt->offline_acknowledged = 1;
    } else if (mqtt->online_inflight &&
               message_id == mqtt->online_message_id) {
        mqtt->online_inflight = 0;
        mqtt->online_pending = 0;
    } else if (mqtt->queue_inflight &&
               message_id == mqtt->queue_message_id) {
        mqtt->queue_inflight = 0;
        pop_queue_locked(mqtt);
    }
    pthread_cond_broadcast(&mqtt->condition);
    pthread_mutex_unlock(&mqtt->mutex);
}

static int publish_online(parking_mqtt_t *mqtt)
{
    parking_mqtt_message_t message;
    int message_id = 0;

    memset(&message, 0, sizeof(message));
    message.topic = mqtt->availability_topic;
    message.payload = mqtt->online_payload;
    message.payload_length = strlen(mqtt->online_payload);
    message.qos = 1;
    message.retain = 1;

    if (mqtt->transport_ops->publish(
            mqtt->transport, &message, &message_id) != 0)
        return -1;

    pthread_mutex_lock(&mqtt->mutex);
    mqtt->online_inflight = 1;
    mqtt->online_message_id = message_id;
    pthread_mutex_unlock(&mqtt->mutex);
    return 0;
}

static int publish_queue_head(parking_mqtt_t *mqtt)
{
    parking_mqtt_queue_item_t *item;
    parking_mqtt_message_t message;
    int message_id = 0;

    pthread_mutex_lock(&mqtt->mutex);
    if (mqtt->queue_count == 0U || mqtt->queue_inflight ||
        !mqtt->stats.connected || mqtt->online_pending) {
        pthread_mutex_unlock(&mqtt->mutex);
        return 0;
    }

    item = &mqtt->queue[mqtt->queue_head];
    memset(&message, 0, sizeof(message));
    message.topic = item->topic;
    message.payload = item->payload;
    message.payload_length = item->payload_length;
    message.qos = item->qos;
    message.retain = item->retain;
    mqtt->queue_inflight = 1;
    pthread_mutex_unlock(&mqtt->mutex);

    if (mqtt->transport_ops->publish(
            mqtt->transport, &message, &message_id) != 0) {
        pthread_mutex_lock(&mqtt->mutex);
        mqtt->queue_inflight = 0;
        pthread_mutex_unlock(&mqtt->mutex);
        return -1;
    }

    pthread_mutex_lock(&mqtt->mutex);
    mqtt->queue_message_id = message_id;
    pthread_mutex_unlock(&mqtt->mutex);
    return 0;
}

static int publish_heartbeat(parking_mqtt_t *mqtt, uint64_t now_us)
{
    parking_mqtt_message_t message;
    char payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    size_t payload_length = 0U;
    uint64_t sequence;
    uint64_t uptime_us;
    int message_id = 0;

    pthread_mutex_lock(&mqtt->mutex);
    if (!mqtt->stats.connected || mqtt->online_pending ||
        now_us < mqtt->next_heartbeat_us) {
        pthread_mutex_unlock(&mqtt->mutex);
        return 0;
    }
    sequence = mqtt->heartbeat_sequence + 1U;
    uptime_us = now_us >= mqtt->started_us
        ? now_us - mqtt->started_us : 0U;
    pthread_mutex_unlock(&mqtt->mutex);

    memset(payload, 0, sizeof(payload));
    if (mqtt->config.build_heartbeat(
            mqtt->config.heartbeat_context, sequence, uptime_us,
            payload, sizeof(payload), &payload_length) != 0 ||
        payload_length == 0U || payload_length >= sizeof(payload)) {
        pthread_mutex_lock(&mqtt->mutex);
        set_error_locked(mqtt, "生成 MQTT 心跳失败");
        mqtt->stats.failed = 1;
        mqtt->stop_requested = 1;
        pthread_mutex_unlock(&mqtt->mutex);
        return -1;
    }

    memset(&message, 0, sizeof(message));
    message.topic = mqtt->heartbeat_topic;
    message.payload = payload;
    message.payload_length = payload_length;
    message.qos = 0;
    message.retain = 0;
    if (mqtt->transport_ops->publish(
            mqtt->transport, &message, &message_id) != 0)
        return -1;

    pthread_mutex_lock(&mqtt->mutex);
    mqtt->heartbeat_sequence = sequence;
    ++mqtt->stats.heartbeat_messages;
    if (mqtt->config.heartbeat_interval_ms > UINT64_MAX / 1000ULL ||
        now_us > UINT64_MAX -
                     mqtt->config.heartbeat_interval_ms * 1000ULL) {
        mqtt->next_heartbeat_us = UINT64_MAX;
    } else {
        mqtt->next_heartbeat_us = now_us +
            mqtt->config.heartbeat_interval_ms * 1000ULL;
    }
    pthread_mutex_unlock(&mqtt->mutex);
    return 0;
}

static void wait_until_locked(parking_mqtt_t *mqtt, uint64_t deadline_us)
{
    struct timespec deadline;

    deadline.tv_sec = (time_t)(deadline_us / 1000000ULL);
    deadline.tv_nsec = (long)(deadline_us % 1000000ULL) * 1000L;
    pthread_cond_timedwait(&mqtt->condition, &mqtt->mutex, &deadline);
}

static void handle_transport_failure(parking_mqtt_t *mqtt,
                                     const char *fallback)
{
    uint64_t now_us = 0;

    copy_transport_error(mqtt, fallback);
    monotonic_us(&now_us);
    pthread_mutex_lock(&mqtt->mutex);
    mark_disconnected_locked(mqtt, now_us);
    pthread_mutex_unlock(&mqtt->mutex);
}

static void publish_graceful_offline(parking_mqtt_t *mqtt)
{
    parking_mqtt_message_t message;
    uint64_t started_us;
    uint64_t now_us;
    uint64_t timeout_us;
    uint64_t deadline_us;
    int message_id = 0;

    pthread_mutex_lock(&mqtt->mutex);
    if (!mqtt->stats.connected) {
        pthread_mutex_unlock(&mqtt->mutex);
        return;
    }
    pthread_mutex_unlock(&mqtt->mutex);

    memset(&message, 0, sizeof(message));
    message.topic = mqtt->availability_topic;
    message.payload = mqtt->offline_payload;
    message.payload_length = strlen(mqtt->offline_payload);
    message.qos = 1;
    message.retain = 1;
    if (mqtt->transport_ops->publish(
            mqtt->transport, &message, &message_id) != 0)
        return;

    pthread_mutex_lock(&mqtt->mutex);
    mqtt->offline_inflight = 1;
    mqtt->offline_message_id = message_id;
    mqtt->offline_acknowledged = 0;
    pthread_mutex_unlock(&mqtt->mutex);

    if (monotonic_us(&started_us) != 0)
        return;
    timeout_us = (uint64_t)mqtt->config.shutdown_timeout_ms * 1000ULL;
    deadline_us = started_us > UINT64_MAX - timeout_us
        ? UINT64_MAX : started_us + timeout_us;

    for (;;) {
        pthread_mutex_lock(&mqtt->mutex);
        if (mqtt->offline_acknowledged || !mqtt->stats.connected) {
            pthread_mutex_unlock(&mqtt->mutex);
            break;
        }
        pthread_mutex_unlock(&mqtt->mutex);

        if (monotonic_us(&now_us) != 0 || now_us >= deadline_us)
            break;
        if (mqtt->transport_ops->loop(
                mqtt->transport, mqtt->config.loop_timeout_ms) != 0)
            break;
    }
}

static void *worker_main(void *context)
{
    parking_mqtt_t *mqtt = context;

    monotonic_us(&mqtt->started_us);
    pthread_mutex_lock(&mqtt->mutex);
    mqtt->stats.running = 1;
    mqtt->next_reconnect_us = mqtt->started_us;
    pthread_cond_broadcast(&mqtt->condition);
    pthread_mutex_unlock(&mqtt->mutex);

    for (;;) {
        uint64_t now_us = 0;
        int connected;
        int connecting;
        int stop_requested;
        int publish_online_now;

        monotonic_us(&now_us);
        pthread_mutex_lock(&mqtt->mutex);
        stop_requested = mqtt->stop_requested;
        connected = mqtt->stats.connected;
        connecting = mqtt->connecting;

        if (!stop_requested && !connected && !connecting &&
            now_us < mqtt->next_reconnect_us) {
            wait_until_locked(mqtt, mqtt->next_reconnect_us);
            pthread_mutex_unlock(&mqtt->mutex);
            continue;
        }

        if (!stop_requested && !connected && !connecting) {
            mqtt->connecting = 1;
            mqtt->reconnect_scheduled = 0;
            ++mqtt->stats.connect_attempts;
            pthread_mutex_unlock(&mqtt->mutex);

            if (mqtt->transport_ops->connect(mqtt->transport) != 0)
                handle_transport_failure(mqtt, "连接 MQTT Broker 失败");
            continue;
        }

        publish_online_now = connected && mqtt->online_pending &&
                             !mqtt->online_inflight;
        pthread_mutex_unlock(&mqtt->mutex);

        if (stop_requested)
            break;

        if (publish_online_now && publish_online(mqtt) != 0) {
            handle_transport_failure(mqtt, "发布 MQTT online 状态失败");
            continue;
        }

        if (connected && publish_queue_head(mqtt) != 0) {
            handle_transport_failure(mqtt, "发布 MQTT 队列消息失败");
            continue;
        }

        monotonic_us(&now_us);
        if (connected && publish_heartbeat(mqtt, now_us) != 0) {
            pthread_mutex_lock(&mqtt->mutex);
            stop_requested = mqtt->stop_requested;
            pthread_mutex_unlock(&mqtt->mutex);
            if (!stop_requested)
                handle_transport_failure(mqtt, "发布 MQTT 心跳失败");
            continue;
        }

        if (connected || connecting) {
            if (mqtt->transport_ops->loop(
                    mqtt->transport, mqtt->config.loop_timeout_ms) != 0)
                handle_transport_failure(mqtt, "MQTT 网络循环失败");
        }
    }

    publish_graceful_offline(mqtt);
    mqtt->transport_ops->disconnect(mqtt->transport);

    pthread_mutex_lock(&mqtt->mutex);
    mqtt->stats.connected = 0;
    mqtt->stats.running = 0;
    mqtt->closed = 1;
    pthread_cond_broadcast(&mqtt->condition);
    pthread_mutex_unlock(&mqtt->mutex);
    return NULL;
}

parking_mqtt_t *parking_mqtt_create_with_transport(
    const parking_mqtt_config_t *config,
    const parking_mqtt_transport_ops_t *transport_ops)
{
    static const parking_mqtt_transport_callbacks_t callbacks = {
        transport_connected,
        transport_disconnected,
        transport_published
    };
    parking_mqtt_transport_config_t transport_config;
    pthread_condattr_t condition_attributes;
    parking_mqtt_t *mqtt;
    int condition_attributes_initialized = 0;

    if (validate_config(config) != 0 || transport_ops == NULL ||
        transport_ops->create == NULL || transport_ops->connect == NULL ||
        transport_ops->publish == NULL || transport_ops->loop == NULL ||
        transport_ops->disconnect == NULL ||
        transport_ops->copy_last_error == NULL ||
        transport_ops->destroy == NULL)
        return NULL;

    mqtt = calloc(1, sizeof(*mqtt));
    if (mqtt == NULL)
        return NULL;

    mqtt->config = *config;
    mqtt->config.port = config->port != 0U
        ? config->port : PARKING_MQTT_DEFAULT_PORT;
    mqtt->config.keepalive_seconds = config->keepalive_seconds != 0U
        ? config->keepalive_seconds
        : PARKING_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    mqtt->config.queue_capacity = config->queue_capacity != 0U
        ? config->queue_capacity : PARKING_MQTT_DEFAULT_QUEUE_CAPACITY;
    mqtt->config.reconnect_initial_ms = config->reconnect_initial_ms != 0U
        ? config->reconnect_initial_ms
        : PARKING_MQTT_DEFAULT_RECONNECT_INITIAL_MS;
    mqtt->config.reconnect_max_ms = config->reconnect_max_ms != 0U
        ? config->reconnect_max_ms : PARKING_MQTT_DEFAULT_RECONNECT_MAX_MS;
    mqtt->config.loop_timeout_ms = config->loop_timeout_ms != 0U
        ? config->loop_timeout_ms : PARKING_MQTT_DEFAULT_LOOP_TIMEOUT_MS;
    mqtt->config.shutdown_timeout_ms = config->shutdown_timeout_ms != 0U
        ? config->shutdown_timeout_ms
        : PARKING_MQTT_DEFAULT_SHUTDOWN_TIMEOUT_MS;

    if (copy_string(mqtt->host, sizeof(mqtt->host), config->host, 0) != 0 ||
        copy_string(mqtt->client_id, sizeof(mqtt->client_id),
                    config->client_id, 0) != 0 ||
        copy_string(mqtt->availability_topic,
                    sizeof(mqtt->availability_topic),
                    config->availability_topic, 0) != 0 ||
        copy_string(mqtt->online_payload, sizeof(mqtt->online_payload),
                    config->online_payload, 0) != 0 ||
        copy_string(mqtt->offline_payload, sizeof(mqtt->offline_payload),
                    config->offline_payload, 0) != 0 ||
        copy_string(mqtt->heartbeat_topic, sizeof(mqtt->heartbeat_topic),
                    config->heartbeat_topic, 0) != 0)
        goto fail;

    if (config->username != NULL &&
        copy_string(mqtt->username, sizeof(mqtt->username),
                    config->username, 1) != 0)
        goto fail;
    if (config->password != NULL &&
        copy_string(mqtt->password, sizeof(mqtt->password),
                    config->password, 1) != 0)
        goto fail;
    if (config->ca_file != NULL &&
        copy_string(mqtt->ca_file, sizeof(mqtt->ca_file),
                    config->ca_file, 1) != 0)
        goto fail;

    mqtt->config.host = mqtt->host;
    mqtt->config.client_id = mqtt->client_id;
    mqtt->config.username = mqtt->username[0] != '\0'
        ? mqtt->username : NULL;
    mqtt->config.password = mqtt->password[0] != '\0'
        ? mqtt->password : NULL;
    mqtt->config.ca_file = mqtt->ca_file[0] != '\0'
        ? mqtt->ca_file : NULL;
    mqtt->config.availability_topic = mqtt->availability_topic;
    mqtt->config.online_payload = mqtt->online_payload;
    mqtt->config.offline_payload = mqtt->offline_payload;
    mqtt->config.heartbeat_topic = mqtt->heartbeat_topic;

    mqtt->queue = calloc(mqtt->config.queue_capacity,
                         sizeof(*mqtt->queue));
    if (mqtt->queue == NULL)
        goto fail;

    if (pthread_mutex_init(&mqtt->mutex, NULL) != 0)
        goto fail;
    mqtt->mutex_initialized = 1;

    if (pthread_condattr_init(&condition_attributes) != 0)
        goto fail;
    condition_attributes_initialized = 1;
    if (pthread_condattr_setclock(
            &condition_attributes, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&mqtt->condition,
                          &condition_attributes) != 0)
        goto fail;
    mqtt->condition_initialized = 1;
    pthread_condattr_destroy(&condition_attributes);
    condition_attributes_initialized = 0;

    mqtt->stats.queue_capacity = mqtt->config.queue_capacity;
    mqtt->reconnect_delay_ms = mqtt->config.reconnect_initial_ms;
    mqtt->online_pending = 1;
    mqtt->transport_ops = transport_ops;

    memset(&transport_config, 0, sizeof(transport_config));
    transport_config.host = mqtt->host;
    transport_config.port = mqtt->config.port;
    transport_config.client_id = mqtt->client_id;
    transport_config.username = mqtt->config.username;
    transport_config.password = mqtt->config.password;
    transport_config.ca_file = mqtt->config.ca_file;
    transport_config.keepalive_seconds = mqtt->config.keepalive_seconds;
    transport_config.will_topic = mqtt->availability_topic;
    transport_config.will_payload = mqtt->offline_payload;
    transport_config.will_payload_length = strlen(mqtt->offline_payload);
    transport_config.will_qos = 1;
    transport_config.will_retain = 1;
    if (transport_ops->create(&transport_config, &callbacks, mqtt,
                              &mqtt->transport) != 0)
        goto fail;

    return mqtt;

fail:
    if (condition_attributes_initialized)
        pthread_condattr_destroy(&condition_attributes);
    parking_mqtt_destroy(mqtt);
    return NULL;
}

parking_mqtt_t *parking_mqtt_create(const parking_mqtt_config_t *config)
{
    return parking_mqtt_create_with_transport(
        config, parking_mqtt_mosquitto_transport_ops());
}

int parking_mqtt_start(parking_mqtt_t *mqtt)
{
    int error;

    if (mqtt == NULL)
        return PARKING_MQTT_ERROR;

    pthread_mutex_lock(&mqtt->mutex);
    if (mqtt->thread_started || mqtt->closed || mqtt->stop_requested ||
        mqtt->stats.failed) {
        pthread_mutex_unlock(&mqtt->mutex);
        return PARKING_MQTT_ERROR;
    }
    mqtt->thread_started = 1;
    pthread_mutex_unlock(&mqtt->mutex);

    error = pthread_create(&mqtt->worker_thread, NULL, worker_main, mqtt);
    if (error != 0) {
        pthread_mutex_lock(&mqtt->mutex);
        mqtt->thread_started = 0;
        mqtt->stats.failed = 1;
        set_error_locked(mqtt, "创建 MQTT 工作线程失败");
        pthread_mutex_unlock(&mqtt->mutex);
        return PARKING_MQTT_ERROR;
    }

    return PARKING_MQTT_OK;
}

void parking_mqtt_stop(parking_mqtt_t *mqtt)
{
    if (mqtt == NULL || !mqtt->mutex_initialized)
        return;

    pthread_mutex_lock(&mqtt->mutex);
    mqtt->stop_requested = 1;
    mqtt->closed = 1;
    if (mqtt->condition_initialized)
        pthread_cond_broadcast(&mqtt->condition);
    pthread_mutex_unlock(&mqtt->mutex);

    if (mqtt->thread_started) {
        pthread_join(mqtt->worker_thread, NULL);
        mqtt->thread_started = 0;
    }
}

int parking_mqtt_publish(parking_mqtt_t *mqtt,
                         const parking_mqtt_message_t *message)
{
    parking_mqtt_queue_item_t *item;

    if (mqtt == NULL || validate_message(message) != 0)
        return PARKING_MQTT_ERROR;

    pthread_mutex_lock(&mqtt->mutex);
    if (mqtt->closed || mqtt->stop_requested || !mqtt->thread_started) {
        pthread_mutex_unlock(&mqtt->mutex);
        return PARKING_MQTT_CLOSED;
    }

    if (mqtt->queue_count == mqtt->stats.queue_capacity) {
        ++mqtt->stats.rejected_messages;
        pthread_mutex_unlock(&mqtt->mutex);
        return PARKING_MQTT_QUEUE_FULL;
    }

    item = &mqtt->queue[mqtt->queue_tail];
    memset(item, 0, sizeof(*item));
    memcpy(item->topic, message->topic, strlen(message->topic) + 1U);
    memcpy(item->payload, message->payload, message->payload_length);
    item->payload_length = message->payload_length;
    item->qos = message->qos;
    item->retain = message->retain;

    mqtt->queue_tail = (mqtt->queue_tail + 1U) % mqtt->stats.queue_capacity;
    ++mqtt->queue_count;
    ++mqtt->stats.enqueued_messages;
    mqtt->stats.queue_depth = mqtt->queue_count;
    if (mqtt->queue_count > mqtt->stats.queue_high_watermark)
        mqtt->stats.queue_high_watermark = mqtt->queue_count;

    pthread_cond_broadcast(&mqtt->condition);
    pthread_mutex_unlock(&mqtt->mutex);
    return PARKING_MQTT_OK;
}

int parking_mqtt_get_stats(parking_mqtt_t *mqtt,
                           parking_mqtt_stats_t *stats)
{
    if (mqtt == NULL || stats == NULL)
        return PARKING_MQTT_ERROR;

    pthread_mutex_lock(&mqtt->mutex);
    *stats = mqtt->stats;
    pthread_mutex_unlock(&mqtt->mutex);
    return PARKING_MQTT_OK;
}

int parking_mqtt_failed(parking_mqtt_t *mqtt)
{
    int failed;

    if (mqtt == NULL)
        return 1;

    pthread_mutex_lock(&mqtt->mutex);
    failed = mqtt->stats.failed;
    pthread_mutex_unlock(&mqtt->mutex);
    return failed;
}

int parking_mqtt_copy_last_error(parking_mqtt_t *mqtt,
                                 char *buffer,
                                 size_t capacity)
{
    if (mqtt == NULL || buffer == NULL || capacity == 0U)
        return PARKING_MQTT_ERROR;

    pthread_mutex_lock(&mqtt->mutex);
    snprintf(buffer, capacity, "%s", mqtt->last_error);
    pthread_mutex_unlock(&mqtt->mutex);
    return PARKING_MQTT_OK;
}

static void clear_sensitive_data(parking_mqtt_t *mqtt)
{
    volatile unsigned char *cursor;
    size_t i;

    cursor = (volatile unsigned char *)mqtt->password;
    for (i = 0U; i < sizeof(mqtt->password); ++i)
        cursor[i] = 0U;
}

void parking_mqtt_destroy(parking_mqtt_t *mqtt)
{
    if (mqtt == NULL)
        return;

    parking_mqtt_stop(mqtt);
    if (mqtt->transport != NULL && mqtt->transport_ops != NULL)
        mqtt->transport_ops->destroy(mqtt->transport);
    if (mqtt->condition_initialized)
        pthread_cond_destroy(&mqtt->condition);
    if (mqtt->mutex_initialized)
        pthread_mutex_destroy(&mqtt->mutex);

    clear_sensitive_data(mqtt);
    free(mqtt->queue);
    free(mqtt);
}

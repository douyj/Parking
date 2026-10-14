#include "parking_mqtt.h"
#include "parking_protocol.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_SITE_ID "parking-test"
#define DEFAULT_DEVICE_ID "rk3576"
#define DEFAULT_DURATION_SECONDS 20U

typedef struct {
    const char *site_id;
    const char *device_id;
    const char *session_id;
} heartbeat_context_t;

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0 ? 0 : -1;
}

static int realtime_us(uint64_t *value)
{
    struct timespec now;

    if (value == NULL || clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;

    *value = (uint64_t)now.tv_sec * 1000000ULL +
             (uint64_t)now.tv_nsec / 1000ULL;
    return 0;
}

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec duration;

    duration.tv_sec = milliseconds / 1000U;
    duration.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
        if (!running)
            break;
    }
}

static int build_heartbeat(void *context, uint64_t sequence,
                           uint64_t uptime_us, char *payload,
                           size_t capacity, size_t *payload_length)
{
    heartbeat_context_t *heartbeat = context;
    parking_protocol_heartbeat_message_t message;
    char message_id[PARKING_PROTOCOL_ID_MAX_LENGTH + 1U];
    uint64_t timestamp_us;
    size_t required_size;
    int length;

    if (heartbeat == NULL || payload_length == NULL ||
        realtime_us(&timestamp_us) != 0)
        return -1;

    length = snprintf(message_id, sizeof(message_id), "hb-%s-%llu",
                      heartbeat->session_id,
                      (unsigned long long)sequence);
    if (length <= 0 || (size_t)length >= sizeof(message_id))
        return -1;

    memset(&message, 0, sizeof(message));
    message.metadata.message_id = message_id;
    message.metadata.site_id = heartbeat->site_id;
    message.metadata.device_id = heartbeat->device_id;
    message.metadata.session_id = heartbeat->session_id;
    message.metadata.timestamp_unix_us = timestamp_us;
    message.sequence = sequence;
    message.uptime_us = uptime_us;

    if (parking_protocol_encode_heartbeat(
            &message, payload, capacity, &required_size) !=
            PARKING_PROTOCOL_OK || required_size == 0U)
        return -1;

    *payload_length = required_size - 1U;
    return 0;
}

static int build_availability(const char *site_id,
                              const char *device_id,
                              const char *session_id,
                              parking_protocol_availability_t state,
                              char *payload,
                              size_t capacity)
{
    parking_protocol_availability_message_t message;
    char message_id[PARKING_PROTOCOL_ID_MAX_LENGTH + 1U];
    uint64_t timestamp_us;
    size_t required_size;
    int length;

    if (realtime_us(&timestamp_us) != 0)
        return -1;

    length = snprintf(message_id, sizeof(message_id), "%s-%s", session_id,
                      state == PARKING_PROTOCOL_ONLINE
                          ? "online" : "offline");
    if (length <= 0 || (size_t)length >= sizeof(message_id))
        return -1;

    memset(&message, 0, sizeof(message));
    message.metadata.message_id = message_id;
    message.metadata.site_id = site_id;
    message.metadata.device_id = device_id;
    message.metadata.session_id = session_id;
    message.metadata.timestamp_unix_us = timestamp_us;
    message.state = state;
    return parking_protocol_encode_availability(
               &message, payload, capacity, &required_size) ==
           PARKING_PROTOCOL_OK ? 0 : -1;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [--host HOST] [--port PORT] [--site ID] "
            "[--device ID] [--duration SECONDS] [--username USER] "
            "[--password PASS] [--ca-file PATH]\n",
            program);
}

static int parse_uint(const char *text, unsigned int maximum,
                      unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > maximum)
        return -1;

    *value = (unsigned int)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    parking_mqtt_config_t config;
    parking_mqtt_stats_t stats;
    heartbeat_context_t heartbeat;
    parking_mqtt_t *mqtt = NULL;
    const char *host = DEFAULT_HOST;
    const char *site_id = DEFAULT_SITE_ID;
    const char *device_id = DEFAULT_DEVICE_ID;
    const char *username = NULL;
    const char *password = NULL;
    const char *ca_file = NULL;
    unsigned int port = PARKING_MQTT_DEFAULT_PORT;
    unsigned int duration_seconds = DEFAULT_DURATION_SECONDS;
    char availability_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char heartbeat_topic[PARKING_MQTT_TOPIC_CAPACITY];
    char online_payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    char offline_payload[PARKING_MQTT_PAYLOAD_CAPACITY];
    char client_id[PARKING_MQTT_CLIENT_ID_CAPACITY];
    char session_id[PARKING_PROTOCOL_ID_MAX_LENGTH + 1U];
    uint64_t timestamp_us;
    unsigned int elapsed_ms;
    int exit_code = 1;
    int i;

    for (i = 1; i < argc; ++i) {
        const char **string_value = NULL;

        if (strcmp(argv[i], "--host") == 0)
            string_value = &host;
        else if (strcmp(argv[i], "--site") == 0)
            string_value = &site_id;
        else if (strcmp(argv[i], "--device") == 0)
            string_value = &device_id;
        else if (strcmp(argv[i], "--username") == 0)
            string_value = &username;
        else if (strcmp(argv[i], "--password") == 0)
            string_value = &password;
        else if (strcmp(argv[i], "--ca-file") == 0)
            string_value = &ca_file;

        if (string_value != NULL) {
            if (++i >= argc || argv[i][0] == '\0') {
                print_usage(argv[0]);
                return 1;
            }
            *string_value = argv[i];
            continue;
        }

        if (strcmp(argv[i], "--port") == 0) {
            if (++i >= argc || parse_uint(argv[i], 65535U, &port) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--duration") == 0) {
            if (++i >= argc ||
                parse_uint(argv[i], 86400U, &duration_seconds) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        print_usage(argv[0]);
        return 1;
    }

    if (install_signal_handlers() != 0 || realtime_us(&timestamp_us) != 0)
        return 1;

    if (snprintf(session_id, sizeof(session_id), "session-%llu-%ld",
                 (unsigned long long)timestamp_us, (long)getpid()) >=
            (int)sizeof(session_id) ||
        snprintf(client_id, sizeof(client_id), "%s-%s-%ld",
                 site_id, device_id, (long)getpid()) >=
            (int)sizeof(client_id)) {
        fprintf(stderr, "MQTT 测试 ID 过长\n");
        return 1;
    }

    if (parking_protocol_build_topic(
            site_id, device_id, PARKING_PROTOCOL_TOPIC_AVAILABILITY,
            availability_topic, sizeof(availability_topic), NULL) !=
            PARKING_PROTOCOL_OK ||
        parking_protocol_build_topic(
            site_id, device_id, PARKING_PROTOCOL_TOPIC_HEARTBEAT,
            heartbeat_topic, sizeof(heartbeat_topic), NULL) !=
            PARKING_PROTOCOL_OK ||
        build_availability(site_id, device_id, session_id,
                           PARKING_PROTOCOL_ONLINE, online_payload,
                           sizeof(online_payload)) != 0 ||
        build_availability(site_id, device_id, session_id,
                           PARKING_PROTOCOL_OFFLINE, offline_payload,
                           sizeof(offline_payload)) != 0) {
        fprintf(stderr, "生成 MQTT 测试消息失败\n");
        return 1;
    }

    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.site_id = site_id;
    heartbeat.device_id = device_id;
    heartbeat.session_id = session_id;

    memset(&config, 0, sizeof(config));
    config.host = host;
    config.port = (uint16_t)port;
    config.client_id = client_id;
    config.username = username;
    config.password = password;
    config.ca_file = ca_file;
    config.availability_topic = availability_topic;
    config.online_payload = online_payload;
    config.offline_payload = offline_payload;
    config.heartbeat_topic = heartbeat_topic;
    config.heartbeat_interval_ms = 2000U;
    config.build_heartbeat = build_heartbeat;
    config.heartbeat_context = &heartbeat;
    config.queue_capacity = 8U;

    mqtt = parking_mqtt_create(&config);
    if (mqtt == NULL) {
        fprintf(stderr, "创建真实 Mosquitto 客户端失败\n");
        goto cleanup;
    }
    if (parking_mqtt_start(mqtt) != PARKING_MQTT_OK) {
        fprintf(stderr, "启动 MQTT 工作线程失败\n");
        goto cleanup;
    }

    printf("MQTT 测试开始: %s:%u, topic=%s, 持续=%u 秒\n",
           host, port, availability_topic, duration_seconds);
    for (elapsed_ms = 0U;
         running && elapsed_ms < duration_seconds * 1000U;
         elapsed_ms += 100U) {
        sleep_ms(100U);
        if (parking_mqtt_failed(mqtt))
            break;
    }

    parking_mqtt_stop(mqtt);
    memset(&stats, 0, sizeof(stats));
    if (parking_mqtt_get_stats(mqtt, &stats) != PARKING_MQTT_OK)
        goto cleanup;

    printf("MQTT 测试结束: 连接尝试=%llu, 成功连接=%llu, "
           "断线=%llu, 心跳=%llu\n",
           (unsigned long long)stats.connect_attempts,
           (unsigned long long)stats.successful_connections,
           (unsigned long long)stats.disconnects,
           (unsigned long long)stats.heartbeat_messages);
    if (!stats.failed && stats.successful_connections > 0U &&
        stats.heartbeat_messages > 0U)
        exit_code = 0;

cleanup:
    if (exit_code != 0 && mqtt != NULL) {
        char error[256];

        memset(error, 0, sizeof(error));
        parking_mqtt_copy_last_error(mqtt, error, sizeof(error));
        if (error[0] != '\0')
            fprintf(stderr, "MQTT 错误: %s\n", error);
    }
    parking_mqtt_destroy(mqtt);
    return exit_code;
}

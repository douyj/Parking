#ifndef PARKING_MQTT_H
#define PARKING_MQTT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARKING_MQTT_DEFAULT_PORT 1883U
#define PARKING_MQTT_DEFAULT_KEEPALIVE_SECONDS 30U
#define PARKING_MQTT_DEFAULT_QUEUE_CAPACITY 64U
#define PARKING_MQTT_DEFAULT_RECONNECT_INITIAL_MS 500U
#define PARKING_MQTT_DEFAULT_RECONNECT_MAX_MS 30000U
#define PARKING_MQTT_DEFAULT_LOOP_TIMEOUT_MS 100U
#define PARKING_MQTT_DEFAULT_SHUTDOWN_TIMEOUT_MS 2000U

#define PARKING_MQTT_HOST_CAPACITY 256U
#define PARKING_MQTT_CLIENT_ID_CAPACITY 128U
#define PARKING_MQTT_CREDENTIAL_CAPACITY 256U
#define PARKING_MQTT_PATH_CAPACITY 4096U
#define PARKING_MQTT_TOPIC_CAPACITY 256U
#define PARKING_MQTT_PAYLOAD_CAPACITY 2048U

typedef struct parking_mqtt parking_mqtt_t;

typedef enum {
    PARKING_MQTT_ERROR = -1,
    PARKING_MQTT_CLOSED = 0,
    PARKING_MQTT_OK = 1,
    PARKING_MQTT_QUEUE_FULL = 2
} parking_mqtt_result_t;

typedef struct {
    const char *topic;
    const void *payload;
    size_t payload_length;
    int qos;
    int retain;
} parking_mqtt_message_t;

/*
 * 由 MQTT 工作线程周期调用。回调只能生成 Payload，不能执行网络 I/O，
 * 成功时把实际字节数写入 payload_length。
 */
typedef int (*parking_mqtt_heartbeat_fn)(
    void *context,
    uint64_t sequence,
    uint64_t uptime_us,
    char *payload,
    size_t capacity,
    size_t *payload_length);

typedef struct {
    const char *host;
    uint16_t port;
    const char *client_id;
    const char *username;
    const char *password;
    const char *ca_file;
    unsigned int keepalive_seconds;

    const char *availability_topic;
    const char *online_payload;
    const char *offline_payload;

    const char *heartbeat_topic;
    uint64_t heartbeat_interval_ms;
    parking_mqtt_heartbeat_fn build_heartbeat;
    void *heartbeat_context;

    size_t queue_capacity;
    uint64_t reconnect_initial_ms;
    uint64_t reconnect_max_ms;
    unsigned int loop_timeout_ms;
    unsigned int shutdown_timeout_ms;
} parking_mqtt_config_t;

typedef struct {
    uint64_t connect_attempts;
    uint64_t successful_connections;
    uint64_t disconnects;
    uint64_t reconnect_waits;

    uint64_t enqueued_messages;
    uint64_t acknowledged_messages;
    uint64_t retried_messages;
    uint64_t rejected_messages;
    uint64_t heartbeat_messages;

    size_t queue_depth;
    size_t queue_capacity;
    size_t queue_high_watermark;

    int running;
    int connected;
    int failed;
} parking_mqtt_stats_t;

/*
 * 创建对象时只加载 Mosquitto 并复制配置，不连接 Broker。
 * 对象为一次性启动语义；stop 后需要 destroy 并重新 create。
 */
parking_mqtt_t *parking_mqtt_create(const parking_mqtt_config_t *config);

int parking_mqtt_start(parking_mqtt_t *mqtt);
void parking_mqtt_stop(parking_mqtt_t *mqtt);

/*
 * 非阻塞发布接口。成功后模块已复制 Topic 和 Payload。
 * 队列满时拒绝当前消息，不覆盖已有消息。
 */
int parking_mqtt_publish(parking_mqtt_t *mqtt,
                         const parking_mqtt_message_t *message);

int parking_mqtt_get_stats(parking_mqtt_t *mqtt,
                           parking_mqtt_stats_t *stats);
int parking_mqtt_failed(parking_mqtt_t *mqtt);
int parking_mqtt_copy_last_error(parking_mqtt_t *mqtt,
                                 char *buffer,
                                 size_t capacity);

void parking_mqtt_destroy(parking_mqtt_t *mqtt);

#ifdef __cplusplus
}
#endif

#endif

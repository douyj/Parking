#include "parking_mqtt_transport.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOSQUITTO_LIBRARY_ERROR_CAPACITY 256U

typedef struct {
    int (*lib_init)(void);
    int (*lib_cleanup)(void);
    struct mosquitto *(*new_client)(const char *, bool, void *);
    void (*destroy_client)(struct mosquitto *);
    int (*will_set)(struct mosquitto *, const char *, int,
                    const void *, int, bool);
    int (*username_pw_set)(struct mosquitto *, const char *, const char *);
    int (*tls_set)(struct mosquitto *, const char *, const char *,
                   const char *, const char *,
                   int (*)(char *, int, int, void *));
    int (*connect_client)(struct mosquitto *, const char *, int, int);
    int (*reconnect_client)(struct mosquitto *);
    int (*disconnect_client)(struct mosquitto *);
    int (*publish)(struct mosquitto *, int *, const char *, int,
                   const void *, int, bool);
    int (*loop)(struct mosquitto *, int, int);
    void (*connect_callback_set)(
        struct mosquitto *, void (*)(struct mosquitto *, void *, int));
    void (*disconnect_callback_set)(
        struct mosquitto *, void (*)(struct mosquitto *, void *, int));
    void (*publish_callback_set)(
        struct mosquitto *, void (*)(struct mosquitto *, void *, int));
    const char *(*error_string)(int);
} mosquitto_api_t;

struct parking_mqtt_transport {
    void *library;
    mosquitto_api_t api;
    struct mosquitto *client;
    parking_mqtt_transport_callbacks_t callbacks;
    void *callback_context;

    char host[PARKING_MQTT_HOST_CAPACITY];
    uint16_t port;
    unsigned int keepalive_seconds;
    int connected_once;
    int library_initialized;
    char last_error[MOSQUITTO_LIBRARY_ERROR_CAPACITY];
};

static pthread_mutex_t mosquitto_library_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int mosquitto_library_references;

static int acquire_mosquitto_library(parking_mqtt_transport_t *transport)
{
    int result = MOSQ_ERR_SUCCESS;

    pthread_mutex_lock(&mosquitto_library_mutex);
    if (mosquitto_library_references == 0U)
        result = transport->api.lib_init();
    if (result == MOSQ_ERR_SUCCESS)
        ++mosquitto_library_references;
    pthread_mutex_unlock(&mosquitto_library_mutex);
    return result;
}

static void release_mosquitto_library(parking_mqtt_transport_t *transport)
{
    pthread_mutex_lock(&mosquitto_library_mutex);
    if (mosquitto_library_references > 0U) {
        --mosquitto_library_references;
        if (mosquitto_library_references == 0U)
            transport->api.lib_cleanup();
    }
    pthread_mutex_unlock(&mosquitto_library_mutex);
}

static int load_symbol(void *library, const char *name,
                       void *destination, size_t destination_size)
{
    void *symbol;

    if (library == NULL || name == NULL || destination == NULL ||
        destination_size != sizeof(symbol))
        return -1;

    dlerror();
    symbol = dlsym(library, name);
    if (symbol == NULL || dlerror() != NULL)
        return -1;

    memcpy(destination, &symbol, sizeof(symbol));
    return 0;
}

#define LOAD_MOSQUITTO_SYMBOL(transport, member, symbol_name)              \
    do {                                                                   \
        if (load_symbol((transport)->library, (symbol_name),               \
                        &(transport)->api.member,                          \
                        sizeof((transport)->api.member)) != 0) {           \
            snprintf((transport)->last_error,                              \
                     sizeof((transport)->last_error),                      \
                     "Mosquitto 缺少符号 %s", (symbol_name));            \
            return -1;                                                     \
        }                                                                  \
    } while (0)

static int load_mosquitto(parking_mqtt_transport_t *transport)
{
    static const char *library_names[] = {
        "libmosquitto.so.1",
        "libmosquitto.so"
    };
    size_t i;

    for (i = 0U; i < sizeof(library_names) / sizeof(library_names[0]);
         ++i) {
        transport->library = dlopen(library_names[i], RTLD_NOW | RTLD_LOCAL);
        if (transport->library != NULL)
            break;
    }

    if (transport->library == NULL) {
        const char *message = dlerror();

        snprintf(transport->last_error, sizeof(transport->last_error),
                 "无法加载 Mosquitto 共享库: %s",
                 message != NULL ? message : "未知错误");
        return -1;
    }

    LOAD_MOSQUITTO_SYMBOL(transport, lib_init, "mosquitto_lib_init");
    LOAD_MOSQUITTO_SYMBOL(transport, lib_cleanup,
                          "mosquitto_lib_cleanup");
    LOAD_MOSQUITTO_SYMBOL(transport, new_client, "mosquitto_new");
    LOAD_MOSQUITTO_SYMBOL(transport, destroy_client, "mosquitto_destroy");
    LOAD_MOSQUITTO_SYMBOL(transport, will_set, "mosquitto_will_set");
    LOAD_MOSQUITTO_SYMBOL(transport, username_pw_set,
                          "mosquitto_username_pw_set");
    LOAD_MOSQUITTO_SYMBOL(transport, tls_set, "mosquitto_tls_set");
    LOAD_MOSQUITTO_SYMBOL(transport, connect_client, "mosquitto_connect");
    LOAD_MOSQUITTO_SYMBOL(transport, reconnect_client,
                          "mosquitto_reconnect");
    LOAD_MOSQUITTO_SYMBOL(transport, disconnect_client,
                          "mosquitto_disconnect");
    LOAD_MOSQUITTO_SYMBOL(transport, publish, "mosquitto_publish");
    LOAD_MOSQUITTO_SYMBOL(transport, loop, "mosquitto_loop");
    LOAD_MOSQUITTO_SYMBOL(transport, connect_callback_set,
                          "mosquitto_connect_callback_set");
    LOAD_MOSQUITTO_SYMBOL(transport, disconnect_callback_set,
                          "mosquitto_disconnect_callback_set");
    LOAD_MOSQUITTO_SYMBOL(transport, publish_callback_set,
                          "mosquitto_publish_callback_set");
    LOAD_MOSQUITTO_SYMBOL(transport, error_string, "mosquitto_strerror");
    return 0;
}

static void set_mosquitto_error(parking_mqtt_transport_t *transport,
                                const char *operation,
                                int result)
{
    const char *message = NULL;

    if (transport->api.error_string != NULL)
        message = transport->api.error_string(result);

    if (result == MOSQ_ERR_ERRNO) {
        snprintf(transport->last_error, sizeof(transport->last_error),
                 "%s: %s", operation, strerror(errno));
    } else {
        snprintf(transport->last_error, sizeof(transport->last_error),
                 "%s: %s", operation,
                 message != NULL ? message : "未知 Mosquitto 错误");
    }
}

static void on_connect(struct mosquitto *client, void *context, int result)
{
    parking_mqtt_transport_t *transport = context;

    (void)client;
    if (transport != NULL && transport->callbacks.connected != NULL)
        transport->callbacks.connected(transport->callback_context, result);
}

static void on_disconnect(struct mosquitto *client, void *context, int result)
{
    parking_mqtt_transport_t *transport = context;

    (void)client;
    if (transport != NULL && transport->callbacks.disconnected != NULL) {
        transport->callbacks.disconnected(
            transport->callback_context, result);
    }
}

static void on_publish(struct mosquitto *client, void *context,
                       int message_id)
{
    parking_mqtt_transport_t *transport = context;

    (void)client;
    if (transport != NULL && transport->callbacks.published != NULL) {
        transport->callbacks.published(
            transport->callback_context, message_id);
    }
}

static int mosquitto_transport_create(
    const parking_mqtt_transport_config_t *config,
    const parking_mqtt_transport_callbacks_t *callbacks,
    void *callback_context,
    parking_mqtt_transport_t **output)
{
    parking_mqtt_transport_t *transport;
    int result;

    if (config == NULL || callbacks == NULL || output == NULL ||
        config->host == NULL || config->client_id == NULL ||
        config->will_topic == NULL || config->will_payload == NULL ||
        config->will_payload_length == 0U ||
        config->will_payload_length > (size_t)INT_MAX)
        return -1;

    *output = NULL;
    transport = calloc(1, sizeof(*transport));
    if (transport == NULL)
        return -1;

    if (strlen(config->host) >= sizeof(transport->host)) {
        free(transport);
        return -1;
    }
    memcpy(transport->host, config->host, strlen(config->host) + 1U);
    transport->port = config->port;
    transport->keepalive_seconds = config->keepalive_seconds;
    transport->callbacks = *callbacks;
    transport->callback_context = callback_context;

    if (load_mosquitto(transport) != 0)
        goto fail;

    result = acquire_mosquitto_library(transport);
    if (result != MOSQ_ERR_SUCCESS) {
        set_mosquitto_error(transport, "初始化 Mosquitto 失败", result);
        goto fail;
    }
    transport->library_initialized = 1;

    transport->client = transport->api.new_client(
        config->client_id, true, transport);
    if (transport->client == NULL) {
        snprintf(transport->last_error, sizeof(transport->last_error),
                 "创建 Mosquitto 客户端失败");
        goto fail;
    }

    transport->api.connect_callback_set(transport->client, on_connect);
    transport->api.disconnect_callback_set(transport->client, on_disconnect);
    transport->api.publish_callback_set(transport->client, on_publish);

    result = transport->api.will_set(
        transport->client, config->will_topic,
        (int)config->will_payload_length, config->will_payload,
        config->will_qos, config->will_retain != 0);
    if (result != MOSQ_ERR_SUCCESS) {
        set_mosquitto_error(transport, "设置 MQTT LWT 失败", result);
        goto fail;
    }

    if (config->username != NULL) {
        result = transport->api.username_pw_set(
            transport->client, config->username, config->password);
        if (result != MOSQ_ERR_SUCCESS) {
            set_mosquitto_error(
                transport, "设置 MQTT 用户名和密码失败", result);
            goto fail;
        }
    }

    if (config->ca_file != NULL) {
        result = transport->api.tls_set(
            transport->client, config->ca_file, NULL, NULL, NULL, NULL);
        if (result != MOSQ_ERR_SUCCESS) {
            set_mosquitto_error(transport, "设置 MQTT TLS 失败", result);
            goto fail;
        }
    }

    *output = transport;
    return 0;

fail:
    if (transport->client != NULL && transport->api.destroy_client != NULL)
        transport->api.destroy_client(transport->client);
    if (transport->library_initialized)
        release_mosquitto_library(transport);
    if (transport->library != NULL)
        dlclose(transport->library);
    free(transport);
    return -1;
}

static int mosquitto_transport_connect(parking_mqtt_transport_t *transport)
{
    int result;

    if (transport == NULL || transport->client == NULL)
        return -1;

    if (transport->connected_once) {
        result = transport->api.reconnect_client(transport->client);
    } else {
        result = transport->api.connect_client(
            transport->client, transport->host, (int)transport->port,
            (int)transport->keepalive_seconds);
    }

    if (result != MOSQ_ERR_SUCCESS) {
        set_mosquitto_error(transport, "连接 MQTT Broker 失败", result);
        return -1;
    }

    transport->connected_once = 1;
    transport->last_error[0] = '\0';
    return 0;
}

static int mosquitto_transport_publish(
    parking_mqtt_transport_t *transport,
    const parking_mqtt_message_t *message,
    int *message_id)
{
    int result;

    if (transport == NULL || message == NULL || message_id == NULL ||
        message->payload_length > (size_t)INT_MAX)
        return -1;

    result = transport->api.publish(
        transport->client, message_id, message->topic,
        (int)message->payload_length, message->payload,
        message->qos, message->retain != 0);
    if (result != MOSQ_ERR_SUCCESS) {
        set_mosquitto_error(transport, "发布 MQTT 消息失败", result);
        return -1;
    }

    return 0;
}

static int mosquitto_transport_loop(parking_mqtt_transport_t *transport,
                                    unsigned int timeout_ms)
{
    int timeout;
    int result;

    if (transport == NULL)
        return -1;

    timeout = timeout_ms > (unsigned int)INT_MAX
        ? INT_MAX : (int)timeout_ms;
    result = transport->api.loop(transport->client, timeout, 1);
    if (result != MOSQ_ERR_SUCCESS) {
        set_mosquitto_error(transport, "执行 MQTT 网络循环失败", result);
        return -1;
    }

    return 0;
}

static int mosquitto_transport_disconnect(
    parking_mqtt_transport_t *transport)
{
    int result;

    if (transport == NULL || transport->client == NULL)
        return -1;

    result = transport->api.disconnect_client(transport->client);
    if (result == MOSQ_ERR_SUCCESS || result == MOSQ_ERR_NO_CONN)
        return 0;

    set_mosquitto_error(transport, "断开 MQTT Broker 失败", result);
    return -1;
}

static int mosquitto_transport_copy_last_error(
    parking_mqtt_transport_t *transport,
    char *buffer,
    size_t capacity)
{
    if (transport == NULL || buffer == NULL || capacity == 0U)
        return -1;

    snprintf(buffer, capacity, "%s", transport->last_error);
    return 0;
}

static void mosquitto_transport_destroy(
    parking_mqtt_transport_t *transport)
{
    if (transport == NULL)
        return;

    if (transport->client != NULL)
        transport->api.destroy_client(transport->client);
    if (transport->library_initialized)
        release_mosquitto_library(transport);
    if (transport->library != NULL)
        dlclose(transport->library);
    free(transport);
}

const parking_mqtt_transport_ops_t *parking_mqtt_mosquitto_transport_ops(void)
{
    static const parking_mqtt_transport_ops_t operations = {
        mosquitto_transport_create,
        mosquitto_transport_connect,
        mosquitto_transport_publish,
        mosquitto_transport_loop,
        mosquitto_transport_disconnect,
        mosquitto_transport_copy_last_error,
        mosquitto_transport_destroy
    };

    return &operations;
}

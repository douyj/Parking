#ifndef PARKING_MQTT_TRANSPORT_H
#define PARKING_MQTT_TRANSPORT_H

#include "parking_mqtt.h"

#include <stddef.h>

typedef struct parking_mqtt_transport parking_mqtt_transport_t;

typedef struct {
    void (*connected)(void *context, int result);
    void (*disconnected)(void *context, int result);
    void (*published)(void *context, int message_id);
} parking_mqtt_transport_callbacks_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *client_id;
    const char *username;
    const char *password;
    const char *ca_file;
    unsigned int keepalive_seconds;
    const char *will_topic;
    const void *will_payload;
    size_t will_payload_length;
    int will_qos;
    int will_retain;
} parking_mqtt_transport_config_t;

typedef struct {
    int (*create)(const parking_mqtt_transport_config_t *config,
                  const parking_mqtt_transport_callbacks_t *callbacks,
                  void *callback_context,
                  parking_mqtt_transport_t **transport);
    int (*connect)(parking_mqtt_transport_t *transport);
    int (*publish)(parking_mqtt_transport_t *transport,
                   const parking_mqtt_message_t *message,
                   int *message_id);
    int (*loop)(parking_mqtt_transport_t *transport,
                unsigned int timeout_ms);
    int (*disconnect)(parking_mqtt_transport_t *transport);
    int (*copy_last_error)(parking_mqtt_transport_t *transport,
                           char *buffer,
                           size_t capacity);
    void (*destroy)(parking_mqtt_transport_t *transport);
} parking_mqtt_transport_ops_t;

const parking_mqtt_transport_ops_t *parking_mqtt_mosquitto_transport_ops(void);

parking_mqtt_t *parking_mqtt_create_with_transport(
    const parking_mqtt_config_t *config,
    const parking_mqtt_transport_ops_t *transport_ops);

#endif

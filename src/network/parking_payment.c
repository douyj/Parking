#include "parking_payment.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PAYMENT_HOST_CAPACITY 256U
#define PAYMENT_PATH_CAPACITY 768U
#define PAYMENT_SITE_CAPACITY 65U
#define PAYMENT_DEVICE_CAPACITY 65U
#define PAYMENT_PLATE_CAPACITY 32U
#define PAYMENT_TOKEN_CAPACITY 65U
#define PAYMENT_REQUEST_CAPACITY 4096U
#define PAYMENT_RESPONSE_CAPACITY 8192U
#define PAYMENT_DEFAULT_POLL_INTERVAL_MS 500U
#define PAYMENT_DEFAULT_NETWORK_TIMEOUT_MS 2000U

struct parking_payment {
    char host[PAYMENT_HOST_CAPACITY];
    char base_path[PAYMENT_PATH_CAPACITY];
    char site_id[PAYMENT_SITE_CAPACITY];
    char device_id[PAYMENT_DEVICE_CAPACITY];
    char plate_number[PAYMENT_PLATE_CAPACITY];
    char token[PAYMENT_TOKEN_CAPACITY];
    char payment_url[PARKING_PAYMENT_URL_CAPACITY];
    char last_error[PARKING_PAYMENT_ERROR_CAPACITY];
    uint16_t port;
    unsigned int poll_interval_ms;
    unsigned int network_timeout_ms;
    uint64_t operation_id;
    uint64_t amount_cents;
    uint64_t generation;
    parking_payment_state_t state;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    int thread_started;
    int stop_requested;
};

typedef struct {
    uint64_t generation;
    parking_payment_state_t state;
    uint64_t operation_id;
    uint64_t amount_cents;
    char plate_number[PAYMENT_PLATE_CAPACITY];
    char token[PAYMENT_TOKEN_CAPACITY];
} payment_work_t;

static int copy_text(char *destination, size_t capacity, const char *source)
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

static int valid_identifier(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    if (value == NULL || value[0] == '\0')
        return 0;
    while (*cursor != '\0') {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') ||
              *cursor == '.' || *cursor == '_' || *cursor == '-'))
            return 0;
        ++cursor;
    }
    return 1;
}

static void set_error_locked(parking_payment_t *payment, const char *message)
{
    snprintf(payment->last_error, sizeof(payment->last_error), "%s",
             message != NULL ? message : "未知支付错误");
}

static void set_error(parking_payment_t *payment, const char *message)
{
    pthread_mutex_lock(&payment->mutex);
    set_error_locked(payment, message);
    pthread_mutex_unlock(&payment->mutex);
}

static int parse_base_url(parking_payment_t *payment, const char *base_url)
{
    const char *authority;
    const char *path;
    const char *port_text = NULL;
    size_t host_length;
    unsigned long port = 80U;
    char *end = NULL;

    if (strncmp(base_url, "http://", 7U) != 0)
        return -1;

    authority = base_url + 7U;
    path = strchr(authority, '/');
    if (path == NULL)
        path = authority + strlen(authority);
    port_text = memchr(authority, ':', (size_t)(path - authority));
    host_length = port_text != NULL
        ? (size_t)(port_text - authority) : (size_t)(path - authority);
    if (host_length == 0U || host_length >= sizeof(payment->host))
        return -1;

    memcpy(payment->host, authority, host_length);
    payment->host[host_length] = '\0';
    if (port_text != NULL) {
        errno = 0;
        port = strtoul(port_text + 1U, &end, 10);
        if (errno != 0 || end != path || port == 0U || port > 65535U)
            return -1;
    }
    payment->port = (uint16_t)port;

    if (*path == '\0' || strcmp(path, "/") == 0) {
        payment->base_path[0] = '\0';
        return 0;
    }
    if (strlen(path) >= sizeof(payment->base_path))
        return -1;
    memcpy(payment->base_path, path, strlen(path) + 1U);
    while (payment->base_path[0] != '\0' &&
           payment->base_path[strlen(payment->base_path) - 1U] == '/') {
        payment->base_path[strlen(payment->base_path) - 1U] = '\0';
    }
    return 0;
}

static int connect_server(parking_payment_t *payment)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    struct timeval timeout;
    char port_text[16];
    int socket_fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_text, sizeof(port_text), "%u", payment->port);
    if (getaddrinfo(payment->host, port_text, &hints, &addresses) != 0) {
        set_error(payment, "解析支付服务器地址失败");
        return -1;
    }

    timeout.tv_sec = payment->network_timeout_ms / 1000U;
    timeout.tv_usec = (suseconds_t)(payment->network_timeout_ms % 1000U) * 1000;
    for (address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype,
                           address->ai_protocol);
        if (socket_fd < 0)
            continue;
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                         &timeout, sizeof(timeout));
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                         &timeout, sizeof(timeout));
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0)
            break;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0)
        set_error(payment, "连接支付服务器失败");
    return socket_fd;
}

static int send_all(int socket_fd, const char *data, size_t length)
{
    size_t sent = 0U;

    while (sent < length) {
        ssize_t result = send(socket_fd, data + sent, length - sent,
                              MSG_NOSIGNAL);

        if (result <= 0)
            return -1;
        sent += (size_t)result;
    }
    return 0;
}

static int http_request(parking_payment_t *payment,
                        const char *method,
                        const char *path,
                        const char *body,
                        char *response_body,
                        size_t response_capacity)
{
    char request[PAYMENT_REQUEST_CAPACITY];
    char response[PAYMENT_RESPONSE_CAPACITY];
    const char *separator;
    size_t body_length = body != NULL ? strlen(body) : 0U;
    size_t received = 0U;
    int socket_fd;
    int request_length;
    int status_code = 0;

    request_length = snprintf(
        request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: %s:%u\r\n"
        "Accept: application/json\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        method, path, payment->host, payment->port, body_length,
        body != NULL ? body : "");
    if (request_length < 0 || (size_t)request_length >= sizeof(request)) {
        set_error(payment, "支付 HTTP 请求过长");
        return -1;
    }

    socket_fd = connect_server(payment);
    if (socket_fd < 0)
        return -1;
    if (send_all(socket_fd, request, (size_t)request_length) != 0) {
        close(socket_fd);
        set_error(payment, "发送支付 HTTP 请求失败");
        return -1;
    }

    while (received + 1U < sizeof(response)) {
        ssize_t result = recv(socket_fd, response + received,
                              sizeof(response) - received - 1U, 0);

        if (result == 0)
            break;
        if (result < 0) {
            close(socket_fd);
            set_error(payment, "读取支付 HTTP 响应失败");
            return -1;
        }
        received += (size_t)result;
    }
    close(socket_fd);
    response[received] = '\0';

    if (sscanf(response, "HTTP/%*u.%*u %d", &status_code) != 1 ||
        status_code < 200 || status_code >= 300) {
        set_error(payment, "支付服务器返回非成功状态");
        return -1;
    }
    separator = strstr(response, "\r\n\r\n");
    if (separator == NULL || strlen(separator + 4U) >= response_capacity) {
        set_error(payment, "支付 HTTP 响应格式无效");
        return -1;
    }
    memcpy(response_body, separator + 4U, strlen(separator + 4U) + 1U);
    return 0;
}

static int json_escape(const char *source, char *output, size_t capacity)
{
    size_t used = 0U;

    while (*source != '\0') {
        unsigned char value = (unsigned char)*source++;

        if (value < 0x20U)
            return -1;
        if (value == '"' || value == '\\') {
            if (used + 2U >= capacity)
                return -1;
            output[used++] = '\\';
        } else if (used + 1U >= capacity) {
            return -1;
        }
        output[used++] = (char)value;
    }
    if (used >= capacity)
        return -1;
    output[used] = '\0';
    return 0;
}

static int json_string(const char *json, const char *key,
                       char *output, size_t capacity)
{
    char marker[96];
    const char *value;
    const char *end;
    size_t length;

    length = (size_t)snprintf(marker, sizeof(marker), "\"%s\":\"", key);
    if (length >= sizeof(marker))
        return -1;
    value = strstr(json, marker);
    if (value == NULL)
        return -1;
    value += strlen(marker);
    end = strchr(value, '"');
    if (end == NULL)
        return -1;
    length = (size_t)(end - value);
    if (length == 0U || length >= capacity || memchr(value, '\\', length))
        return -1;
    memcpy(output, value, length);
    output[length] = '\0';
    return 0;
}

static int create_order(parking_payment_t *payment,
                        const payment_work_t *work,
                        char *token,
                        char *payment_url,
                        parking_payment_state_t *state)
{
    char escaped_plate[PAYMENT_PLATE_CAPACITY * 2U];
    char body[512];
    char path[PAYMENT_PATH_CAPACITY];
    char response[PAYMENT_RESPONSE_CAPACITY];
    char status[32];
    int length;
    int path_length;

    if (json_escape(work->plate_number, escaped_plate,
                    sizeof(escaped_plate)) != 0)
        return -1;
    length = snprintf(
        body, sizeof(body),
        "{\"site_id\":\"%s\",\"device_id\":\"%s\","
        "\"operation_id\":%llu,\"plate_number\":\"%s\","
        "\"amount_cents\":%llu}",
        payment->site_id, payment->device_id,
        (unsigned long long)work->operation_id, escaped_plate,
        (unsigned long long)work->amount_cents);
    path_length = snprintf(path, sizeof(path), "%s/api/payments",
                           payment->base_path);
    if (length < 0 || (size_t)length >= sizeof(body) ||
        path_length < 0 || (size_t)path_length >= sizeof(path))
        return -1;
    if (http_request(payment, "POST", path, body,
                     response, sizeof(response)) != 0)
        return -1;
    if (json_string(response, "token", token, PAYMENT_TOKEN_CAPACITY) != 0 ||
        json_string(response, "pay_url", payment_url,
                    PARKING_PAYMENT_URL_CAPACITY) != 0 ||
        json_string(response, "status", status, sizeof(status)) != 0) {
        set_error(payment, "支付订单响应缺少字段");
        return -1;
    }
    *state = strcmp(status, "paid") == 0
        ? PARKING_PAYMENT_PAID : PARKING_PAYMENT_PENDING;
    return 0;
}

static int poll_order(parking_payment_t *payment,
                      const payment_work_t *work,
                      parking_payment_state_t *state)
{
    char path[PAYMENT_PATH_CAPACITY];
    char response[PAYMENT_RESPONSE_CAPACITY];
    char status[32];
    int path_length;

    path_length = snprintf(path, sizeof(path), "%s/api/payments/%s",
                           payment->base_path, work->token);
    if (path_length < 0 || (size_t)path_length >= sizeof(path))
        return -1;
    if (http_request(payment, "GET", path, NULL,
                     response, sizeof(response)) != 0)
        return -1;
    if (json_string(response, "status", status, sizeof(status)) != 0) {
        set_error(payment, "支付状态响应缺少字段");
        return -1;
    }
    if (strcmp(status, "paid") == 0)
        *state = PARKING_PAYMENT_PAID;
    else if (strcmp(status, "expired") == 0)
        *state = PARKING_PAYMENT_FAILED;
    else
        *state = PARKING_PAYMENT_PENDING;
    return 0;
}

static void wait_before_retry(parking_payment_t *payment,
                              uint64_t generation)
{
    struct timespec deadline;

    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
        return;
    deadline.tv_sec += payment->poll_interval_ms / 1000U;
    deadline.tv_nsec +=
        (long)(payment->poll_interval_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&payment->mutex);
    if (!payment->stop_requested && payment->generation == generation &&
        (payment->state == PARKING_PAYMENT_CREATING ||
         payment->state == PARKING_PAYMENT_PENDING)) {
        (void)pthread_cond_timedwait(&payment->condition,
                                     &payment->mutex, &deadline);
    }
    pthread_mutex_unlock(&payment->mutex);
}

static void *payment_worker(void *context)
{
    parking_payment_t *payment = context;

    for (;;) {
        payment_work_t work;
        parking_payment_state_t new_state;
        char token[PAYMENT_TOKEN_CAPACITY];
        char payment_url[PARKING_PAYMENT_URL_CAPACITY];
        int result;

        pthread_mutex_lock(&payment->mutex);
        while (!payment->stop_requested &&
               payment->state != PARKING_PAYMENT_CREATING &&
               payment->state != PARKING_PAYMENT_PENDING) {
            pthread_cond_wait(&payment->condition, &payment->mutex);
        }
        if (payment->stop_requested) {
            pthread_mutex_unlock(&payment->mutex);
            break;
        }
        memset(&work, 0, sizeof(work));
        work.generation = payment->generation;
        work.state = payment->state;
        work.operation_id = payment->operation_id;
        work.amount_cents = payment->amount_cents;
        memcpy(work.plate_number, payment->plate_number,
               sizeof(work.plate_number));
        memcpy(work.token, payment->token, sizeof(work.token));
        pthread_mutex_unlock(&payment->mutex);

        memset(token, 0, sizeof(token));
        memset(payment_url, 0, sizeof(payment_url));
        new_state = work.state;
        if (work.state == PARKING_PAYMENT_CREATING) {
            result = create_order(payment, &work, token, payment_url,
                                  &new_state);
        } else {
            result = poll_order(payment, &work, &new_state);
        }

        pthread_mutex_lock(&payment->mutex);
        if (payment->generation == work.generation && result == 0) {
            payment->state = new_state;
            payment->last_error[0] = '\0';
            if (work.state == PARKING_PAYMENT_CREATING) {
                memcpy(payment->token, token, sizeof(payment->token));
                memcpy(payment->payment_url, payment_url,
                       sizeof(payment->payment_url));
            }
        }
        pthread_mutex_unlock(&payment->mutex);
        wait_before_retry(payment, work.generation);
    }
    return NULL;
}

parking_payment_t *parking_payment_create(
    const parking_payment_config_t *config)
{
    parking_payment_t *payment;

    if (config == NULL || config->base_url == NULL ||
        !valid_identifier(config->site_id) ||
        !valid_identifier(config->device_id))
        return NULL;

    payment = calloc(1, sizeof(*payment));
    if (payment == NULL)
        return NULL;
    if (parse_base_url(payment, config->base_url) != 0 ||
        copy_text(payment->site_id, sizeof(payment->site_id),
                  config->site_id) != 0 ||
        copy_text(payment->device_id, sizeof(payment->device_id),
                  config->device_id) != 0)
        goto fail;
    payment->poll_interval_ms = config->poll_interval_ms != 0U
        ? config->poll_interval_ms : PAYMENT_DEFAULT_POLL_INTERVAL_MS;
    payment->network_timeout_ms = config->network_timeout_ms != 0U
        ? config->network_timeout_ms : PAYMENT_DEFAULT_NETWORK_TIMEOUT_MS;
    if (pthread_mutex_init(&payment->mutex, NULL) != 0)
        goto fail;
    if (pthread_cond_init(&payment->condition, NULL) != 0) {
        pthread_mutex_destroy(&payment->mutex);
        goto fail;
    }
    return payment;

fail:
    free(payment);
    return NULL;
}

int parking_payment_start(parking_payment_t *payment)
{
    if (payment == NULL || payment->thread_started)
        return -1;
    if (pthread_create(&payment->thread, NULL,
                       payment_worker, payment) != 0)
        return -1;
    payment->thread_started = 1;
    return 0;
}

int parking_payment_request(parking_payment_t *payment,
                            uint64_t operation_id,
                            const char *plate_number,
                            uint64_t amount_cents)
{
    if (payment == NULL || !payment->thread_started ||
        operation_id == 0U || amount_cents == 0U ||
        plate_number == NULL)
        return -1;

    pthread_mutex_lock(&payment->mutex);
    if (copy_text(payment->plate_number, sizeof(payment->plate_number),
                  plate_number) != 0) {
        pthread_mutex_unlock(&payment->mutex);
        return -1;
    }
    ++payment->generation;
    payment->operation_id = operation_id;
    payment->amount_cents = amount_cents;
    payment->token[0] = '\0';
    payment->payment_url[0] = '\0';
    payment->last_error[0] = '\0';
    payment->state = PARKING_PAYMENT_CREATING;
    pthread_cond_broadcast(&payment->condition);
    pthread_mutex_unlock(&payment->mutex);
    return 0;
}

int parking_payment_get_snapshot(
    parking_payment_t *payment,
    parking_payment_snapshot_t *snapshot)
{
    if (payment == NULL || snapshot == NULL)
        return -1;

    pthread_mutex_lock(&payment->mutex);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = payment->state;
    snapshot->operation_id = payment->operation_id;
    snapshot->amount_cents = payment->amount_cents;
    memcpy(snapshot->payment_url, payment->payment_url,
           sizeof(snapshot->payment_url));
    memcpy(snapshot->last_error, payment->last_error,
           sizeof(snapshot->last_error));
    pthread_mutex_unlock(&payment->mutex);
    return 0;
}

void parking_payment_reset(parking_payment_t *payment)
{
    if (payment == NULL)
        return;
    pthread_mutex_lock(&payment->mutex);
    ++payment->generation;
    payment->state = PARKING_PAYMENT_IDLE;
    payment->operation_id = 0U;
    payment->amount_cents = 0U;
    payment->plate_number[0] = '\0';
    payment->token[0] = '\0';
    payment->payment_url[0] = '\0';
    payment->last_error[0] = '\0';
    pthread_cond_broadcast(&payment->condition);
    pthread_mutex_unlock(&payment->mutex);
}

void parking_payment_stop(parking_payment_t *payment)
{
    if (payment == NULL || !payment->thread_started)
        return;
    pthread_mutex_lock(&payment->mutex);
    payment->stop_requested = 1;
    pthread_cond_broadcast(&payment->condition);
    pthread_mutex_unlock(&payment->mutex);
    pthread_join(payment->thread, NULL);
    payment->thread_started = 0;
}

void parking_payment_destroy(parking_payment_t *payment)
{
    if (payment == NULL)
        return;
    parking_payment_stop(payment);
    pthread_cond_destroy(&payment->condition);
    pthread_mutex_destroy(&payment->mutex);
    free(payment);
}

const char *parking_payment_state_name(parking_payment_state_t state)
{
    switch (state) {
        case PARKING_PAYMENT_IDLE:
            return "IDLE";
        case PARKING_PAYMENT_CREATING:
            return "CREATING";
        case PARKING_PAYMENT_PENDING:
            return "PENDING";
        case PARKING_PAYMENT_PAID:
            return "PAID";
        case PARKING_PAYMENT_FAILED:
            return "FAILED";
        default:
            return "UNKNOWN";
    }
}

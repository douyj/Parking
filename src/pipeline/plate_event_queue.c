#include "plate_event_queue.h"

#include "log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct plate_event_queue {
    pthread_mutex_t mutex;

    plate_pipeline_event_t *events;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;

    uint64_t pushed_events;
    uint64_t popped_events;
    uint64_t dropped_events;
    size_t high_watermark;

    int closed;
};

plate_event_queue_t *plate_event_queue_create(size_t capacity)
{
    plate_event_queue_t *queue;
    int error;

    if (capacity == 0 || capacity > SIZE_MAX / sizeof(plate_pipeline_event_t)) {
        LOG_ERROR("无效的稳定事件队列容量");
        return NULL;
    }

    queue = calloc(1, sizeof(*queue));
    if (queue == NULL) {
        LOG_ERROR("分配稳定事件队列失败");
        return NULL;
    }

    queue->events = calloc(capacity, sizeof(*queue->events));
    if (queue->events == NULL) {
        LOG_ERROR("分配稳定事件队列存储空间失败");
        free(queue);
        return NULL;
    }

    error = pthread_mutex_init(&queue->mutex, NULL);
    if (error != 0) {
        LOG_ERROR("初始化稳定事件队列锁失败");
        free(queue->events);
        free(queue);
        return NULL;
    }

    queue->capacity = capacity;
    return queue;
}

// 向稳定事件队列推送事件
int plate_event_queue_push(plate_event_queue_t *queue, const plate_pipeline_event_t *event)
{
    int error;

    if (queue == NULL || event == NULL)
        return PLATE_EVENT_QUEUE_ERROR;

    // 加锁
    error = pthread_mutex_lock(&queue->mutex);
    if (error != 0)
        return PLATE_EVENT_QUEUE_ERROR;

    // 如果队列已关闭，直接返回
    if (queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        return PLATE_EVENT_QUEUE_CLOSED;
    }

    // 如果队列已满，直接返回
    if (queue->count == queue->capacity) {
        ++queue->dropped_events;
        pthread_mutex_unlock(&queue->mutex);
        return PLATE_EVENT_QUEUE_FULL;
    }

    // 向队列尾部推送事件
    queue->events[queue->tail] = *event;
    queue->tail = (queue->tail + 1U) % queue->capacity;     // tail 指向下一个空位置
    ++queue->count;
    ++queue->pushed_events;

    if (queue->count > queue->high_watermark)
        queue->high_watermark = queue->count;

    pthread_mutex_unlock(&queue->mutex);
    return PLATE_EVENT_QUEUE_OK;
}

// 从稳定事件队列读取事件
int plate_event_queue_take(plate_event_queue_t *queue, plate_pipeline_event_t *event)
{
    int error;

    if (queue == NULL || event == NULL)
        return PLATE_EVENT_QUEUE_ERROR;

    // 加锁
    error = pthread_mutex_lock(&queue->mutex);
    if (error != 0)
        return PLATE_EVENT_QUEUE_ERROR;

    // 如果队列已关闭，直接返回
    if (queue->count == 0) {
        int result = queue->closed
            ? PLATE_EVENT_QUEUE_CLOSED
            : PLATE_EVENT_QUEUE_EMPTY;

        pthread_mutex_unlock(&queue->mutex);
        return result;
    }

    *event = queue->events[queue->head];
    memset(&queue->events[queue->head], 0,
           sizeof(queue->events[queue->head]));

    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    ++queue->popped_events;

    pthread_mutex_unlock(&queue->mutex);
    return PLATE_EVENT_QUEUE_OK;
}


void plate_event_queue_close(plate_event_queue_t *queue)
{
    if (queue == NULL)
        return;

    if (pthread_mutex_lock(&queue->mutex) != 0)
        return;

    queue->closed = 1;

    pthread_mutex_unlock(&queue->mutex);
}

int plate_event_queue_get_stats(plate_event_queue_t *queue, plate_event_queue_stats_t *stats)
{
    if (queue == NULL || stats == NULL)
        return PLATE_EVENT_QUEUE_ERROR;

    if (pthread_mutex_lock(&queue->mutex) != 0)
        return PLATE_EVENT_QUEUE_ERROR;

    stats->pushed_events = queue->pushed_events;
    stats->popped_events = queue->popped_events;
    stats->dropped_events = queue->dropped_events;
    stats->depth = queue->count;
    stats->capacity = queue->capacity;
    stats->high_watermark = queue->high_watermark;

    pthread_mutex_unlock(&queue->mutex);
    return PLATE_EVENT_QUEUE_OK;
}

void plate_event_queue_destroy(plate_event_queue_t *queue)
{
    if (queue == NULL)
        return;

    pthread_mutex_destroy(&queue->mutex);
    free(queue->events);
    free(queue);
}
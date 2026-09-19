#ifndef PLATE_EVENT_QUEUE_H
#define PLATE_EVENT_QUEUE_H

#include "plate_pipeline.h"

#include <stddef.h>
#include <stdint.h>

typedef struct plate_event_queue plate_event_queue_t;

typedef enum {
    PLATE_EVENT_QUEUE_ERROR = -1,
    PLATE_EVENT_QUEUE_CLOSED = 0,
    PLATE_EVENT_QUEUE_OK = 1,
    PLATE_EVENT_QUEUE_EMPTY = 2,
    PLATE_EVENT_QUEUE_FULL = 3
} plate_event_queue_result_t;

typedef struct {
    uint64_t pushed_events;
    uint64_t popped_events;
    uint64_t dropped_events;

    size_t depth;
    size_t capacity;
    size_t high_watermark;
} plate_event_queue_stats_t;

plate_event_queue_t *plate_event_queue_create(size_t capacity);

int plate_event_queue_push(plate_event_queue_t *queue, const plate_pipeline_event_t *event);
int plate_event_queue_take(plate_event_queue_t *queue, plate_pipeline_event_t *event);
void plate_event_queue_close(plate_event_queue_t *queue);
int plate_event_queue_get_stats(plate_event_queue_t *queue, plate_event_queue_stats_t *stats);
void plate_event_queue_destroy(plate_event_queue_t *queue);

#endif
// 

#ifndef LATEST_FRAME_SLOT_H
#define LATEST_FRAME_SLOT_H

#include "pipeline_frame.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct latest_frame_slot latest_frame_slot_t;

typedef enum {
    LATEST_FRAME_SLOT_ERROR = -1,
    LATEST_FRAME_SLOT_CLOSED = 0,
    LATEST_FRAME_SLOT_OK = 1,
    LATEST_FRAME_SLOT_TIMEOUT = 2
} latest_frame_slot_result_t;

/* 创建一个容量为 1 的最新帧槽。 */
latest_frame_slot_t *latest_frame_slot_create(void);

/*
 * 将 frame 的所有权转移给帧槽。
 *
 * 成功后 frame 会被清零。
 * 如果槽中已有旧帧，将释放旧帧并累计丢帧数。
 */
int latest_frame_slot_push(latest_frame_slot_t *slot, pipeline_frame_t *frame);

/*
 * 从帧槽中取得一帧，并将所有权转移给调用者。
 *
 * timeout_ms < 0：一直等待；
 * timeout_ms = 0：立即返回；
 * timeout_ms > 0：最多等待指定毫秒。
 */
int latest_frame_slot_take(latest_frame_slot_t *slot, pipeline_frame_t *frame, int timeout_ms);

/* 关闭帧槽并唤醒所有等待者。关闭后不再接受新帧。 */
void latest_frame_slot_close(latest_frame_slot_t *slot);

/* 返回被新帧覆盖的旧帧数量。 */
uint64_t latest_frame_slot_dropped(latest_frame_slot_t *slot);

/* 释放槽内残留帧、同步对象和帧槽本身。 */
void latest_frame_slot_destroy(latest_frame_slot_t *slot);

#ifdef __cplusplus
}
#endif

#endif

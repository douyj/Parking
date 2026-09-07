#include "latest_frame_slot.h"

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 定义最新帧槽结构体
struct latest_frame_slot{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pipeline_frame_t frame;
    uint64_t dropped_frames;    // 丢失帧的数量
    int has_frame;
    int closed;
};


// 计算“当前时间 + timeout_ms”，得到一个绝对截止时间 deadline
static int make_wait_deadline(struct timespec *deadline, int timeout_ms)
{
    // 获取当前时间
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        LOG_ERROR("获取等待时间失败: %s", strerror(errno));
        return -1;
    }

    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }

    return 0;
}

// 创建一个容量为 1 的最新帧槽
latest_frame_slot_t *latest_frame_slot_create(void)
{
    latest_frame_slot_t *slot;
    int error;

    // 分配内存并初始化为零
    slot = calloc(1, sizeof(*slot));    
    if (slot == NULL) {
        LOG_ERROR("分配最新帧槽失败");
        return NULL;
    }

    // 初始化互斥锁
    error = pthread_mutex_init(&slot->mutex, NULL);
    if (error != 0) {
        LOG_ERROR("初始化帧槽互斥锁失败: %s", strerror(error));
        free(slot);
        return NULL;
    }

    // 初始化条件变量
    error = pthread_cond_init(&slot->condition, NULL);
    if (error != 0) {
        LOG_ERROR("初始化帧槽条件变量失败: %s", strerror(error));
        pthread_mutex_destroy(&slot->mutex);    // 销毁互斥锁
        free(slot);         // 前面已经分配了内存，所以这里需要释放
        return NULL;
    }

    return slot;
}

/*
    @brief: 把调用者手里的 frame 放进最新帧槽 slot 里
    @param slot: 最新帧槽
    @param frame: 要放入的帧
*/
int latest_frame_slot_push(latest_frame_slot_t *slot, pipeline_frame_t *frame)
{
    int error;

    if(slot == NULL || frame == NULL) return LATEST_FRAME_SLOT_ERROR;

    error = pthread_mutex_lock(&slot->mutex);
    if (error != 0) {
        LOG_ERROR("锁定帧槽失败: %s", strerror(error));
        return LATEST_FRAME_SLOT_ERROR;
    }

    if(slot->closed){
        pthread_mutex_unlock(&slot->mutex);
        return LATEST_FRAME_SLOT_CLOSED;
    }

    if(slot->has_frame){
        pipeline_frame_release(&slot->frame);    // 释放旧帧
        slot->dropped_frames++;                   // 统计丢帧数
    }

    slot->frame = *frame;    // 转移所有权，把调用者手里的 frame 放进最新帧槽 slot 里
    memset(frame, 0, sizeof(*frame));    // 清零调用者手里的 frame，避免重复释放
    slot->has_frame = 1;    // 标记最新帧槽里有帧

    pthread_cond_signal(&slot->condition);    // 唤醒等待的线程，通知有新帧可用
    pthread_mutex_unlock(&slot->mutex);    

    return LATEST_FRAME_SLOT_OK;
}

/*
    @brief: 从最新帧槽 slot 取得一帧，并将所有权转移给调用者
    @param slot: 最新帧槽
    @param frame: 从最新帧槽取得的帧
    @param timeout_ms: < 0：一直等待；= 0：立即返回；> 0：最多等待指定毫秒
*/
int latest_frame_slot_take(latest_frame_slot_t *slot, pipeline_frame_t *frame, int timeout_ms)
{
    struct timespec deadline;
    int error;
    int timed_wait = timeout_ms > 0;   // 标志变量，判断是否需要等待指定毫秒

    if(slot == NULL || frame == NULL) return LATEST_FRAME_SLOT_ERROR;

    if(timed_wait && make_wait_deadline(&deadline, timeout_ms) != 0){
        return LATEST_FRAME_SLOT_ERROR;
    }

    // 锁定互斥锁，保护对最新帧槽的访问
    error = pthread_mutex_lock(&slot->mutex);
    if(error != 0){
        LOG_ERROR("锁定帧槽失败: %s", strerror(error));
        return LATEST_FRAME_SLOT_ERROR;
    }

    // 只要“没有帧”并且“还没关闭”，我就继续等
    while(!slot->has_frame && !slot->closed){

        // 调用者明确要求“不要等”
        if(timeout_ms == 0){
            pthread_mutex_unlock(&slot->mutex);
            return LATEST_FRAME_SLOT_TIMEOUT;
        }

        if(timed_wait){
            error = pthread_cond_timedwait(&slot->condition, &slot->mutex, &deadline);  //等待新帧，但是最多等到 deadline
        }else{
            error = pthread_cond_wait(&slot->condition, &slot->mutex);  // timeout_ms < 0，表示一直等
        }

        if (error == ETIMEDOUT) {
            pthread_mutex_unlock(&slot->mutex);
            return LATEST_FRAME_SLOT_TIMEOUT;
        }

        if(error != 0 ){
            LOG_ERROR("等待最新帧失败: %s", strerror(error));
            pthread_mutex_unlock(&slot->mutex);
            return LATEST_FRAME_SLOT_ERROR;
        }
    }


    if(!slot->has_frame){
        pthread_mutex_unlock(&slot->mutex);
        return LATEST_FRAME_SLOT_CLOSED;
    }

    // 有最新帧
    *frame = slot->frame;    // 转移所有权，把最新帧槽里的帧交给调用者
    memset(&slot->frame, 0, sizeof(slot->frame));    
    slot->has_frame = 0;    // 标记最新帧槽里没有

    pthread_mutex_unlock(&slot->mutex);
    return LATEST_FRAME_SLOT_OK;
}


// 关闭最新帧槽 slot 并唤醒所有等待者
void latest_frame_slot_close(latest_frame_slot_t *slot)
{
    int error;

    if(slot == NULL) return;

    error = pthread_mutex_lock(&slot->mutex);
    if(error != 0){
        LOG_ERROR("锁定帧槽失败: %s", strerror(error));
        return;
    }

    slot->closed = 1;
    pthread_cond_broadcast(&slot->condition);    // 唤醒所有等待的线程，通知帧槽已关闭
    pthread_mutex_unlock(&slot->mutex);
}

// 获取丢失帧的数量
uint64_t latest_frame_slot_dropped(latest_frame_slot_t *slot)
{
    uint64_t dropped;
    int error;

    if(slot == NULL) return 0;
    
    error = pthread_mutex_lock(&slot->mutex);
    if(error != 0){
        LOG_ERROR("锁定帧槽失败: %s", strerror(error));
        return 0;
    }
    
    dropped = slot->dropped_frames;

    pthread_mutex_unlock(&slot->mutex);
    return dropped;
}


// 释放槽内残留帧、同步对象和帧槽本身
void latest_frame_slot_destroy(latest_frame_slot_t *slot)
{
    if (slot == NULL)
        return;

    pipeline_frame_release(&slot->frame);
    pthread_cond_destroy(&slot->condition);     // 销毁条件变量
    pthread_mutex_destroy(&slot->mutex);         // 销毁互斥锁
    free(slot);
}

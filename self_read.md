# pipeline
## pipeline_frame
pipeline_frame里面保存的是 一帧 的数据信息，包括：图像数据，时间记录。

## latest_frame_slot
专门在线程之间传递最新一帧图像的小盒子, 其中：最新一帧的slot包括：锁，条件变量，一帧数据，丢失帧的数量，是否有帧，是否关闭。
```
struct latest_frame_slot{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pipeline_frame_t frame;
    uint64_t dropped_frames;    // 丢失帧的数量
    int has_frame;
    int closed;
};
```
# Parking 项目交接文档

> 最后更新：2026-10-11（Asia/Shanghai）
>
> 新会话第一步：完整阅读本文，然后执行 `git status --short`。
>
> 本文记录当前真实状态；`Parking.md` 是长期规划，不代表其中功能已经实现。

## 1. 项目与本阶段任务

项目目录：

```text
/home/dyj/project/Parking
```

这是基于 LubanCat RK3576 的嵌入式 Linux 智慧停车场项目。长期链路：

```text
USB/V4L2 摄像头
    -> MJPG/YUYV 转 BGR888
    -> RK3576 NPU / RKNN 车牌识别
    -> 入场/出场业务状态机
    -> PWM 舵机道闸
    -> SQLite 停车记录与车位管理
    -> MQTT/JSON 与后台通信
    -> LVGL 本地界面、计费、支付、OTA 等
```

上一阶段任务是把已经真机跑通、但重复写在两个测试文件中的多线程识别管线抽成正式 `plate_pipeline` 模块，同时让两个测试改用公开接口，确保 Display 和 Gate 仍留在主线程/业务层。该阶段已经完成。

随后完成了构建依赖整理和第一版正式主程序集成。当前已经完成并确认：

1. `plate_pipeline` 已实现并接入构建；
2. `test_realtime_recognizer_threaded` 已迁移到 `plate_pipeline`，板端运行成功；
3. `test_realtime_plate_gate` 已迁移到 `plate_pipeline`；
4. 迁移后的真实 Gate 模式已在板端运行成功；
5. 稳定车牌仍能触发真实开闸，连续 10 秒没有有效车牌识别仍能自动关闸。
6. `plate_pipeline.c` 已从 `parking_lib` 拆入独立的 `plate_pipeline_lib`；
7. `plate_pipeline_lib` 已明确公开依赖 `parking_lib`、`plate_recognizer_lib` 和 `Threads::Threads`；
8. `src/main.c` 已接入 Display 和 `plate_pipeline`，不再是占位程序；
9. `parking` 主程序已交叉编译为 ARM aarch64，并在 RK3576 板端运行成功；
10. 正式主程序已确认能够显示实时识别画面并输出稳定车牌事件。
11. 稳定车牌事件已从单个 `pending_event` 升级为容量可配置的有界 FIFO；
12. FIFO 默认容量为 32，满队列时拒绝新事件、累计丢弃统计并让管线失败停止；
13. FIFO 版本已通过严格告警检查、RK3576 交叉编译和板端正常运行回归。
14. 纯 C 的单车道 `parking_controller` 状态机核心已实现，支持入口/出口模式、业务检查、模拟支付、Gate 状态、通行来源、超时、完成提交和错误复位；
15. `parking_controller` 已拆入独立 `parking_business_lib` 并通过严格告警检查和 RK3576 交叉编译；
16. 正式 `main.c` 已支持 `--lane-mode entry|exit`，会取空稳定事件 FIFO、驱动状态机，并用日志/dry-run 自动响应检查、支付、开关闸和提交命令；该 dry-run 版本已在板端完成入口和出口回归。
17. 新增纯 C 的 `parking_plate_policy`：验证 UTF-8 和 7/8 字符车牌格式，在单次业务进行中忽略完全相同的重复事件、拒绝编辑距离为 1 的歧义事件，并把其他车辆识别为车道忙碌；不会把相似字符串自动合并成同一车牌。
18. `test_parking_plate_policy` 已在 RK3576 板端运行成功；用户同时确认入口和出口 dry-run 流程验证成功，但未提供完整日志或新增性能数据。
19. 新增独立纯 C `parking_storage_lib` 和 SQLite schema v1，实现停车事件、停车会话、聚合车位统计、事务、唯一约束、持久化 operation ID、重复完成幂等以及进程重启后未完成操作复用；宿主测试和 RK3576 交叉编译均已通过。
20. 正式 `main.c` 已把入/出场检查和完成提交从进程内模拟替换为 SQLite，默认数据库为 `parking.db`，也可用 `--database PATH` 指定；支付和 Gate 仍保持 dry-run，不访问 PWM。
21. 新增纯 C `parking_fee` 模块，默认规则为 30 分钟免费，超出部分每 10 分钟 50 分、不足 10 分钟进一周期，单次封顶 2000 分；规则和金额均使用整数，避免浮点金额误差。
22. SQLite 出场检查已改为读取活动会话的真实入场时间并调用计费模块；首次计算金额随 `PENDING` 出场事件持久化，重复检查、规则变化和进程重启均复用冻结金额。
23. `test_parking_fee` 和扩展后的 `test_parking_store` 已通过宿主严格告警、ASan/UBSan 测试及 RK3576 全量交叉编译。
24. 2026-09-21 用户确认新的计费测试、SQLite 存储测试以及同一数据库的入场、重启、出场计费链路均已在 RK3576 板端测试成功；未提供完整日志或新增性能数据。
25. 正式 `parking` 主程序已接入可选真实 Gate：默认仍为 dry-run，传入 `--enable-gate` 后直接初始化 PWM，不再要求输入 `ENABLE`；开关闸命令通过 `gate_update()` 异步确认 `OPEN/CLOSED` 后再推进状态机，真实关闸完成后才提交 SQLite。该版本已通过严格告警检查和 RK3576 交叉编译；2026-09-22 用户确认已在 RK3576 板端测试完成，能够通过正式主程序真实控制 Gate，但未提供完整日志或逐项异常路径结果。
26. 用户把 `PARKING_DEFAULT_PASSAGE_TIMEOUT_US` 从 10 秒改为 5 秒并完成重新交叉编译；正式主程序现在连续 5 秒无有效车牌后请求关闸。独立的 `test_realtime_plate_gate` 仍使用自己的 10 秒常量。
27. 2026-10-11 重新执行了车牌策略、整数计费和 SQLite 存储的宿主 `-Wall -Wextra -Wpedantic -Werror`、ASan/UBSan 回归，全部通过；正式 `parking` 及三个纯 C 测试也已重新完成 RK3576 交叉编译。该自动回归不等于真实 Gate 的免费出场、收费出场或异常拒绝板端验证。
28. 2026-10-11 用户提供正式主程序完整板端日志：车牌 `浙A66666` 入场检查通过后真实开闸，连续约 5 秒无有效车牌后以 `VISION_TIMEOUT` 真实关闸，并在 `CLOSED` 后提交 SQLite 入场；随后使用同一数据库免费出场，费用为 0 分、跳过支付、真实开关闸，并在关闸完成后提交 SQLite 出场。入口与免费出场真实 Gate 链路已验证成功。
29. 2026-10-11 用户在板端安装了 `sqlite3` 和 `libsqlite3-dev`，收费出场回归改为只在专用测试数据库中用 `sqlite3` 调整活动会话入场时间；不再保留额外的数据库辅助程序。
30. 2026-10-11 用户提供收费出场完整板端日志：专用测试数据库中的活动会话调整为约 31.1 分钟后，正式主程序计算费用为 50 分，执行模拟支付，随后真实开闸；连续约 5 秒无有效车牌后真实关闸，并在 `CLOSED` 后提交 SQLite 出场。收费出场真实 Gate 链路已验证成功。
31. 2026-10-11 用户再次以出口模式识别已经完成出场的 `浙A66666`，SQLite 返回 `NO_ACTIVE_SESSION`，程序只记录业务拒绝，没有发出真实 Gate 开闸命令，也没有错误提交出场。异常拒绝不开闸已完成板端验证。
32. 第一版独立 LVGL 9.6.0 + DRM/KMS 显示测试已实现：使用 `/dev/dri/card0`、XRGB8888 和 dumb buffer，Tiny-TTF 直接加载现有 `fonts/platech.ttf`；已通过严格告警和 RK3576 交叉编译。2026-10-11 板端首次运行确认 DSI-1、1024x600 和 plane 提交正常，但 LVGL plane 56 的 zpos 0 被 fbcon plane 96 的 zpos 1 覆盖；加入最高 zpos 补丁后，用户确认中文停车界面显示成功，Ctrl+C 后 fbcon 也能恢复。
33. 正式 `parking_ui` 已实现并接入 `main.c`：在主线程使用 LVGL/DRM 显示 640x360 实时识别画面、入口/出口模式、车位统计、车牌与颜色、置信度、费用、业务状态、Gate 状态和拒绝原因；原 Camera、识别管线、SQLite、计费、Gate 与业务状态机接口未重写。已通过宿主业务回归、严格告警和 RK3576 全量交叉编译。2026-10-11 用户确认正式 `parking` 已在板端显示成功；本次没有提供逐项状态变化和退出清理日志，因此只确认正式 UI 与主循环能够共同运行。

当前没有已知编译卡点。下一步进入 MQTT/JSON：先冻结 Topic 和 Payload，完成独立、可测试的协议编码模块，再接入带断线重连、心跳和 LWT 的 MQTT 工作线程。不要继续重写已经通过真机验证的 Camera、Recognizer、Gate、计费、SQLite、UI 和管线线程逻辑。

## 2. 当前结论速览

```text
RK3576 交叉编译                              已打通
RKNN 静态图片车牌识别                        已在 RK3576 真机验证
USB 摄像头 V4L2 mmap 采集                   已在 RK3576 真机验证
MJPG/YUYV -> BGR888                         已实现并真机验证
BGR888 -> /dev/fb0 -> MIPI DSI              已实现并真机验证
单线程摄像头实时车牌识别                     已在 RK3576 真机验证
连续 3 帧确认、漏检容忍、10 秒冷却            已在 RK3576 真机验证
多线程最新帧识别与显示                        已在 RK3576 真机验证
pipeline_frame/latest_frame_slot             已抽取并真机验证
plate_confirmation                           已抽取并真机验证
正式 plate_pipeline                          已实现、交叉编译并真机验证
独立 plate_pipeline_lib                      已完成并交叉编译验证
迁移后的 test_realtime_recognizer_threaded   板端运行成功
迁移后的 test_realtime_plate_gate            真实 Gate 模式板端运行成功
稳定车牌 -> 真实 Gate 开闸                    已验证成功
连续 10 秒无有效车牌 -> 自动关闸              已验证成功
src/main.c 正式管线与 Display 集成            已完成并在 RK3576 板端验证
parking 稳定车牌事件日志                      已在 RK3576 板端验证
parking 正式 Gate 集成                        已完成并在 RK3576 板端验证真实控制
parking 真实 Gate 入场链路                    已完成 RK3576 板端验证
parking 真实 Gate 免费出场链路                已完成 RK3576 板端验证
parking 真实 Gate 收费出场链路                已完成 RK3576 板端验证
parking 异常拒绝不开闸                        已完成 RK3576 板端验证
稳定事件有界 FIFO                             已实现、交叉编译并完成板端正常路径验证
parking_controller + main dry-run              已完成入口/出口板端业务回归
parking_plate_policy                           已完成宿主与 RK3576 板端回归
parking_storage_lib + SQLite schema v1         已完成 RK3576 板端验证
parking SQLite 检查/提交集成                   已完成 RK3576 板端验证
parking_fee 按时长计费                         已完成 RK3576 板端验证
SQLite 首次出场计费与金额冻结                  已完成 RK3576 板端验证
LVGL / 原生 DRM/KMS                          独立测试已真机验证，正式 parking_ui 待板端验证
MQTT、正式支付、OTA                            尚未实现
```

有界 FIFO 的正常运行路径已经随正式 `parking` 主程序在板端回归成功，但没有专门制造 32 个事件积压，因此满队列溢出、丢弃统计和失败停止路径尚未真机验证。用户明确不需要为本次 FIFO 新增独立单元测试；不能把未执行的溢出测试写成已经通过。

用户没有提供本次正式 `parking` 主程序的具体 FPS、转换耗时、识别耗时、显示耗时、端到端延迟、丢帧数或芯片温度日志。不得编造这些数据；如需做性能对比，应请用户提供板端输出。

2026-09-15 正式主程序板端日志确认 LCD 为 `1024x600`、Camera 为 `/dev/video0` 的 `1280x720@30`，并输出了多个稳定车牌事件。日志中出现过 `粤A3333G`、`粤A3333` 等相近字符串；不能仅凭日志断言它们来自同一块实体车牌。当前业务策略会把 6 字符的 `粤A3333` 判为 `INVALID_PLATE`，把业务进行中与活动车牌只差 1 个 UTF-8 字符的合法车牌判为 `AMBIGUOUS_PLATE`，不会自动改写或合并后写入停车记录。

Gate 仍无物理限位、地感、防砸或车辆通过信号。正式主程序当前自动关闸只依据“连续 5 秒没有识别到有效车牌”的软件计时（独立 Gate 测试仍为 10 秒），因此不能把当前原型当成可无人看守运行的安全系统。

用户观察到连续 RKNN 推理时芯片很烫。当前识别线程无限速，一帧完成后立即处理最新帧；最新帧槽只防止积压，不降低 NPU 占用。用户明确决定暂不限速，等整个项目完成后再处理；未经请求不要擅自加入 sleep 或改变识别频率。

## 3. 当前正式识别管线

### 3.1 文件

```text
include/plate_pipeline.h
src/pipeline/plate_event_queue.h
src/pipeline/plate_event_queue.c
src/pipeline/plate_pipeline.c
```

`plate_event_queue` 是 `plate_pipeline` 的私有实现模块，不属于业务层公开接口。FIFO 及管线改造已包含在提交 `ae032da`（“更新了FIFO”）中；当前工作树没有这几个文件的未提交修改。不要因为它们已提交就重新设计或恢复旧的单事件槽实现。

### 3.2 责任边界

`plate_pipeline` 内部负责：

```text
Camera 创建/打开/启动/停止/关闭
    -> 采集线程
    -> MJPG/YUYV 转独立 BGR888
    -> captured_frames（容量 1 的最新帧槽）
    -> RKNN 识别线程
    -> 最佳结果选择
    -> plate_confirmation 多帧确认
    -> 稳定车牌事件有界 FIFO + last_plate_seen_us
    -> 绘制识别框
    -> annotated_frames（容量 1 的最新帧槽）
```

`plate_pipeline` 不负责：

```text
Display / LVGL
Gate
10 秒自动关闸策略
入场/出场业务
SQLite / MQTT / 计费 / 支付 / OTA
```

这些必须留在主线程或业务层。不要把 Gate 或 Display 塞回识别管线。

### 3.3 公开接口

```c
plate_pipeline_t *plate_pipeline_create(
    const plate_pipeline_config_t *config);
int plate_pipeline_start(plate_pipeline_t *pipeline);
void plate_pipeline_stop(plate_pipeline_t *pipeline);

int plate_pipeline_take_frame(plate_pipeline_t *pipeline,
                              pipeline_frame_t *frame,
                              int timeout_ms);
int plate_pipeline_take_event(plate_pipeline_t *pipeline,
                              plate_pipeline_event_t *event);
uint64_t plate_pipeline_last_plate_seen_us(
    plate_pipeline_t *pipeline);

int plate_pipeline_get_stats(plate_pipeline_t *pipeline,
                             plate_pipeline_stats_t *stats);
int plate_pipeline_failed(plate_pipeline_t *pipeline);
int plate_pipeline_copy_last_error(plate_pipeline_t *pipeline,
                                   char *buffer,
                                   size_t capacity);
void plate_pipeline_destroy(plate_pipeline_t *pipeline);
```

配置包括 Camera 参数、两个模型路径、字体路径、置信度阈值、确认帧数、漏检阈值、冷却时间、Camera 超时、内部帧等待时间和稳定事件队列容量。`event_queue_capacity == 0` 时使用默认容量 32。实现会把路径深拷贝到内部 `PATH_MAX` 数组，不依赖调用者临时字符串。

### 3.4 生命周期和线程边界

- `plate_pipeline_create()` 创建事件 FIFO、帧槽、确认器和 Recognizer，并打开 Camera，但不启动流和线程；
- `plate_pipeline_start()` 启动 Camera，先创建识别线程，再创建采集线程；
- Camera 只由采集线程调用；
- Recognizer 和确认器只由识别线程调用；
- `plate_pipeline_stop()` 设置停止标志、关闭帧槽和事件 FIFO、join 两个线程，再停止 Camera；
- `plate_pipeline_destroy()` 可重复调用 stop，然后关闭 Camera、销毁 Recognizer、帧槽、事件 FIFO 和 mutex 并释放对象；
- 当前对象是一次性启动语义：调用 stop 后帧槽已永久关闭，不能再次 start；需要重启时应 destroy 后重新 create；
- 测试程序的信号处理函数只设置 `volatile sig_atomic_t running = 0`，实际 stop/destroy 在普通主线程执行。

### 3.5 帧和事件语义

`plate_pipeline_take_frame()` 返回：

```text
PLATE_PIPELINE_OK       成功取得一帧
PLATE_PIPELINE_TIMEOUT  等待期间暂时无帧，属于正常情况
PLATE_PIPELINE_CLOSED   管线已关闭
PLATE_PIPELINE_ERROR    真正错误
```

取得帧后，BGR 内存所有权归调用者，显示完成后必须调用：

```c
pipeline_frame_release(&frame);
```

`display_frame.data` 只是指向 `frame.image.data` 的别名，没有复制图像；不能在 `display_present()` 前释放帧。

`plate_pipeline_take_event()` 是非阻塞接口，返回 `1/0/-1` 表示有事件、无事件、错误。稳定事件现在进入独立的线程安全环形 FIFO，默认容量为 32，也可通过 `event_queue_capacity` 配置。事件按先进先出顺序读取，不再被后续事件静默覆盖。

队列满时不覆盖最旧事件，也不阻塞识别线程；当前新事件会被拒绝，`dropped_events` 增加，管线记录“稳定车牌事件队列已满”并进入失败停止。队列关闭后拒绝新事件，但已经入队的事件仍可继续取完。正式停车记录仍需要 SQLite 事务和幂等业务层，内存 FIFO 不提供断电持久化。

`last_plate_seen_us` 在每一帧出现任意有效最佳车牌时刷新，不受稳定确认和同车牌冷却限制，因此同一车牌持续在画面中会持续延后自动关闸。

### 3.6 当前统计与错误

统计包括：

```text
captured_frames
recognized_frames
confirmed_events
queued_events
consumed_events
dropped_events
total_convert_us
total_recognize_us
capture_dropped_frames
output_dropped_frames
event_queue_depth
event_queue_capacity
event_queue_high_watermark
```

两个测试继续在主线程统计显示耗时和端到端延迟。错误字符串通过 `plate_pipeline_copy_last_error()` 复制到调用者缓冲区，避免把跨线程更新的内部字符串裸指针暴露给外部。

## 4. 两个迁移后的测试

### 4.1 `test_realtime_recognizer_threaded`

```text
tests/test_realtime_recognizer_threaded.c
CMake 目标：test_realtime_recognizer_threaded
```

测试仍是纯 C。它已经删除内部 `pipeline_t`、采集线程、识别线程、Camera/Recognizer/帧槽创建和销毁，只保留：

- SIGINT/SIGTERM；
- Display 创建和显示；
- `plate_pipeline` 配置、启动和清理；
- 稳定车牌事件日志；
- FPS、转换/识别/显示耗时、端到端延迟和丢帧日志。

当前默认配置：

```text
/dev/video0
1280x720@30 MJPG
confidence_threshold = 0.30
required_frames = 3
max_missed_frames = 3
same_plate_cooldown_us = 10000000
camera_timeout_ms = 1000
frame_wait_ms = 100
/dev/fb0
```

2026-09-14 用户确认迁移到正式 `plate_pipeline` 后的该测试已在 RK3576 板端运行成功。

### 4.2 `test_realtime_plate_gate`

```text
tests/test_realtime_plate_gate.c
CMake 目标：test_realtime_plate_gate
```

该测试也已删除重复的 Camera、Recognizer、线程和帧槽代码，改用：

```c
plate_pipeline_create/start/stop/destroy
plate_pipeline_take_frame
plate_pipeline_take_event
plate_pipeline_last_plate_seen_us
plate_pipeline_get_stats
```

Gate、Display、10 秒自动关闸策略和真实 Gate 人工确认仍留在测试主线程。

默认运行仍是 dry-run，不访问 PWM：

```bash
./test_realtime_plate_gate
```

真实 Gate 必须显式传参，并在启动警告后人工输入 `ENABLE`：

```bash
./test_realtime_plate_gate --enable-gate
```

`gate_create()` 会立即执行一次关闸初始化。真实模式参数仍是：

```text
pwmchip_path     = /sys/class/pwm/pwmchip2
channel          = 0
period_ns        = 20000000
open_pulse_ns    = 2400000
close_pulse_ns   = 1400000
movement_time_ms = 800
hold_after_move  = 0
```

联动语义保持不变：

- 同一车牌连续识别 3 帧后才产生稳定事件；
- Gate 只有在 `CLOSED` 时接受开闸请求；
- 主线程持续调用 `gate_update()`；
- 任意有效车牌逐帧刷新 `last_plate_seen_us`；
- 连续 10 秒无有效车牌后才请求关闸；
- dry-run 打印模拟开闸/关闸；
- Display 或 Gate 外部错误会让主循环退出，随后停止管线并清理资源。

2026-09-14 用户明确确认迁移到正式 `plate_pipeline` 后，真实 Gate 模式在 RK3576 板端运行成功。没有收到该次运行的完整日志，因此不要编造具体识别结果、FPS、温度或丢帧数字。

### 4.3 正式 `parking` 主程序

```text
src/main.c
CMake 目标：parking
```

第一版正式主程序已经接入：

```text
SIGINT/SIGTERM 安全退出标志
Display 创建与主线程显示
plate_pipeline 配置、创建、启动、停止和销毁
取空稳定车牌事件 FIFO
parking_controller 状态机驱动
--lane-mode entry|exit
--database PATH（默认 parking.db）
SQLite 入/出场检查和完成提交
按真实入场/出场时间计算费用并冻结待付金额
模拟支付；Gate 默认 dry-run，可用 --enable-gate 启用真实 PWM
绘制帧获取与 pipeline_frame_release
```

`parking` 链接 `plate_pipeline_lib`、`parking_business_lib` 和 `parking_storage_lib`，并通过前者自动获得 `parking_lib`、`plate_recognizer_lib` 和 `Threads::Threads` 依赖。主程序不直接访问 Camera、Recognizer 或管线内部线程。入/出场检查、持久化 operation ID、停车会话提交和聚合车位统计已经使用 SQLite 事务；出口按停车时长计费，首次出场检查后冻结金额。支付仍为日志/dry-run；Gate 默认 dry-run，传入 `--enable-gate` 后直接使用已验证的 `pwmchip2/pwm0` 参数，不进行 `ENABLE` 交互确认。2026-09-22 用户确认正式主程序已在板端成功真实控制 Gate；正式支付、LVGL、MQTT 和 OTA 尚未接入。

真实 Gate 模式下，`gate_create()` 会立即开始关闸，控制器先保持 `INITIALIZING`；主循环持续调用 `gate_update()`，只有观测到真实 `CLOSED` 后才进入 `IDLE`。业务开关闸命令不会立即伪造完成事件，只有实际状态到达 `OPEN/CLOSED` 后才向 `parking_controller` 回报；因此 SQLite 完成提交发生在真实关闸完成之后。Gate 更新或命令失败会发送 `PARKING_EVENT_GATE_FAILED` 并让业务进入错误路径。

数据库时间使用 `CLOCK_REALTIME` 的 Unix epoch 微秒，状态机超时仍使用 `CLOCK_MONOTONIC`；不能把两种时间直接混用。入场检查会创建持久化 `PENDING` 操作并预留容量，正式主程序连续 5 秒无有效车牌、关闸完成后才提交正式在场会话；出场完成后才关闭会话并释放车位。同一车牌的未完成操作在进程重启后会返回原 `operation_id`，完成接口可重复调用。

开发过程中曾出现屏幕无画面，根因是 `src/main.c` 中：

```c
if (result == PLATE_PIPELINE_TIMEOUT);
    continue;

if (result = PLATE_PIPELINE_CLOSED)
    break;
```

第一个多余分号导致无条件 `continue`，显示代码永远不可达且成功取出的帧未释放；第二个使用了赋值而非比较。当前工作树已经修正为：

```c
if (result == PLATE_PIPELINE_TIMEOUT)
    continue;

if (result == PLATE_PIPELINE_CLOSED)
    break;
```

同时已经把 Display 配置初始化修正为 `memset(&display_config, 0, sizeof(display_config))`。修正后 `parking` 交叉编译成功，产物确认为 ARM aarch64；2026-09-15 用户确认板端运行、屏幕显示和稳定车牌事件输出成功。

当前没有收到 Ctrl+C 后完整清理日志，因此只能确认信号处理和清理代码已经实现，不能写成板端退出路径已单独验证。

## 5. 已有基础模块与硬件事实

### 5.1 Camera

```text
include/camera.h
src/camera/camera.c
```

板端主采集节点是 `/dev/video0`，由 `uvcvideo` 驱动。`/dev/video1` 是同一 UVC 设备的另一个节点，不要默认它是主节点。

已确认格式包括：

```text
MJPG: 1920x1080@30, 1280x720@30, 640x480@30 等
YUYV: 1920x1080@5, 1280x720@10, 640x480@30 等
```

`CameraFrame.data` 是 V4L2 mmap buffer 的借用指针；必须先转换/复制为自有 BGR，再调用 `camera_release_frame()`，绝不能将该裸指针跨线程入队。

### 5.2 Image Convert

```text
include/image_convert.h
src/image/image_convert.c
```

支持 MJPG 和 YUYV 转 BGR888。输出 `image_bgr_frame_t.data` 是独立分配内存，必须通过 `image_bgr_frame_release()` 或持有它的 `pipeline_frame_release()` 释放。

### 5.3 Display

```text
include/display.h
src/display/display.c
```

板端 `/dev/fb0` 存在，底层连接 MIPI DSI。当前应用使用 DRM 驱动提供的 fbdev 兼容层，不是原生 DRM/KMS：

```text
/dev/dri/card0                  Rockchip display subsystem
/sys/class/drm/card0-DSI-1      connected
/sys/class/drm/card0-DP-1       disconnected
/dev/dri/card1                  NPU DRM 节点，不是显示屏
```

`display.c` 是测试/诊断后端，CPU 逐像素缩放且无双缓冲/垂直同步。正式 UI 计划使用 LVGL，优先考虑 DRM/KMS 后端；两个显示后端不能同时接管同一个显示设备。

### 5.4 Recognizer

```text
include/plate_recognizer.h
src/recognizer/
models/yolo26s-plate-detect-rk3576.rknn
models/plate_rec_color-rk3576.rknn
fonts/platech.ttf
lib/librknnrt.so
```

识别器内部是 C++/OpenCV/RKNN，外部使用 C ABI，测试文件继续保持 C。

静态图片和摄像头实时识别均已真机成功。旧单线程基线约为：

```text
实际处理 FPS       约 6.2～7.3
平均 MJPG/BGR 转换 约 22.8～28.3 ms
平均 RKNN 识别     约 100.4～104.9 ms
平均整帧处理       约 136.1～161.0 ms
```

这些是旧单线程基线，不能冒充当前正式管线的新数据。

`plate_recognizer_last_error()` 实际返回内部 `std::string::c_str()`，调用者只读且不能 `free()`。当前头文件中“必须由调用者释放”的注释是错误的，以后可只修正文档注释，不要改变所有权。

### 5.5 基础管线模块

```text
include/pipeline_frame.h
src/pipeline/pipeline_frame.c
include/latest_frame_slot.h
src/pipeline/latest_frame_slot.c
include/plate_confirmation.h
src/pipeline/plate_confirmation.c
```

`latest_frame_slot` 容量为 1。新帧覆盖旧帧时先释放旧 BGR 并累计丢帧数；`push()` 成功后输入帧被清零，`take()` 成功后所有权转给消费者。

`plate_confirmation` 当前使用单个候选和单个 `last_confirmed_plate`，配置为连续 3 次确认、连续 3 帧漏检后重置、同车牌 10 秒冷却。这是当前原型语义；未来真正多车业务需要按车牌管理状态。

### 5.6 Gate/PWM

```text
include/gate.h
include/pwm_sysfs.h
src/gate/gate.c
src/gate/pwm_sysfs.c
tests/test_gate.c
```

真实路径已确认：

```text
/sys/class/pwm/pwmchip2/pwm0
```

Gate 命令是非阻塞的，必须周期调用 `gate_update()`。没有物理限位，`movement_time_ms` 到期只是软件推测动作完成。

## 6. 构建与验证

工具链：

```text
cmake/toolchain-rk3576.cmake
```

必须使用 LubanCat SDK GCC 10.3 和配套 sysroot。可靠构建目录：

```text
build-rk3576-release
```

构建命令：

```bash
cd /home/dyj/project/Parking

cmake -S . -B build-rk3576-release \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-rk3576.cmake" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-rk3576-release \
  --target parking \
  -j"$(nproc)"

cmake --build build-rk3576-release \
  --target test_realtime_recognizer_threaded \
  -j"$(nproc)"

cmake --build build-rk3576-release \
  --target test_realtime_plate_gate \
  -j"$(nproc)"

cmake --build build-rk3576-release \
  --target test_parking_plate_policy \
  -j"$(nproc)"

cmake --build build-rk3576-release \
  --target test_parking_store \
  -j"$(nproc)"

```

正式 `parking` 现在默认构建并链接 LVGL/DRM；独立显示测试仍通过下面的选项启用：

```bash
cmake -S . -B build-rk3576-lvgl \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-rk3576.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPARKING_BUILD_LVGL_TEST=ON

cmake --build build-rk3576-lvgl \
  --target test_lvgl_display \
  -j"$(nproc)"
```

项目在 `third_party/lvgl` 自带精简的 LVGL v9.6.0 核心源码、公开头文件、CMake 支持文件和许可证，不再依赖构建目录或 `/tmp` 中的临时下载；仍可通过 `PARKING_LVGL_SOURCE_DIR` 指向其他 v9.6.0 源码。交叉构建会从 LubanCat SDK 自带的 ARM64 `libdrm-dev/libdrm2` 包提取头文件和链接库，不会错误链接宿主 x86_64 `libdrm`。

截至 2026-09-21 已完成：

- `plate_pipeline.c` RK3576 交叉编译；
- `plate_pipeline.c` 从 `parking_lib` 拆入独立的 `plate_pipeline_lib`；
- 两个迁移后的测试交叉编译并链接；
- 第一版正式 `parking` 主程序交叉编译并链接；
- `plate_event_queue.c` 已加入 `plate_pipeline_lib`，单个事件槽已替换为有界 FIFO；
- `parking_controller.c` 已加入独立 `parking_business_lib`，并由正式 `parking` 链接；
- `parking_plate_policy.c` 已加入 `parking_business_lib`，对应纯 C 策略测试在宿主运行通过并完成 RK3576 交叉编译；
- `parking_store.c` 已加入独立 `parking_storage_lib`，纯 C 临时数据库测试在宿主运行通过并完成 RK3576 交叉编译；
- `parking_store.c`、`test_parking_store.c` 和更新后的 `main.c` 已使用 RK3576 工具链通过 `-Wall -Wextra -Wpedantic -Werror` 编译检查；
- `parking_fee.c` 已加入 `parking_business_lib`，`test_parking_fee` 覆盖默认/自定义规则、边界、非法参数和极端值；
- `test_parking_store` 已覆盖 30 分钟免费、超 1 微秒收 50 分、2000 分封顶、重复检查不重算、规则变化不改待付金额和重启后金额保留；
- `main.c` 已接入 `parking_controller` 和 SQLite 执行器，支持 `--lane-mode entry|exit` 与 `--database PATH`；
- 产物确认为 ARM aarch64；
- `plate_event_queue.c` 和 `plate_pipeline.c` 额外使用 `-Wall -Wextra -Wpedantic -Werror` 编译通过；
- `parking_controller.c` 使用宿主和 RK3576 工具链的 `-Wall -Wextra -Wpedantic -Werror` 检查通过；
- `git diff --check` 通过；
- 用户完成板端多线程识别回归；
- 用户完成迁移后真实 Gate 回归；
- 用户完成第一版正式 `parking` 主程序板端回归，确认屏幕显示和稳定车牌日志成功；
- 用户确认包含稳定事件 FIFO 的正式 `parking` 版本在 RK3576 板端测试成功。此次确认属于正常运行回归，没有提供专门的满队列溢出日志或新增性能数字。
- 用户确认 `test_parking_plate_policy`、`parking --lane-mode entry` 和 `parking --lane-mode exit` 的上一版 dry-run 已在板端验证成功；未提供完整日志或新增性能数字。
- 2026-09-21 用户确认 `test_parking_fee`、扩展后的 `test_parking_store`，以及使用同一数据库完成入场、重启和出场计费的正式主程序链路均已在 RK3576 板端测试成功；未提供完整日志或新增性能数字。

运行正式主程序时板端需要：

```text
parking
models/yolo26s-plate-detect-rk3576.rknn
models/plate_rec_color-rk3576.rknn
fonts/platech.ttf
lib/librknnrt.so
```

新版本还要求板端系统动态库提供 `libsqlite3.so.0` 或 `libsqlite3.so`。当前 LubanCat GCC sysroot 没有 SQLite 开发包，因此 `parking_storage_lib` 在运行时通过 `dlopen` 加载板端 SQLite，不把宿主库错误链接进 ARM64 产物。数据库文件及其 `-wal`/`-shm` 同目录必须可写。

六个测试程序只在回归对应测试时部署，不是运行 `parking` 的必需文件：

```text
test_realtime_recognizer_threaded
test_realtime_plate_gate
test_parking_plate_policy
test_parking_fee
test_parking_store
test_lvgl_display
```

其中 `test_parking_plate_policy`、`test_parking_fee` 和 `test_parking_store` 不访问摄像头、NPU、Display 或 Gate，可直接在板端运行。`test_parking_store` 使用 `/tmp` 临时数据库并在结束时清理。`test_lvgl_display` 会独占 `/dev/dri/card0`，不能与 `parking`、fbdev 测试或其他显示程序同时运行；它不访问 Camera、NPU、SQLite 或 Gate。

识别目标通过 `$ORIGIN/lib` 使用部署目录里的 `librknnrt.so`。不要覆盖系统 `/lib/librknnrt.so`。

## 7. 当前 Git 工作树

保存本文前确认：

```text
branch: feature/lvgl
HEAD:   1bc265a “ui初始化显示成功”
```

写入本文后预期工作树：

```text
 M HANDOFF.md
 M CMakeLists.txt
 M lv_conf.h
 M src/main.c
?? include/parking_ui.h
?? src/ui/parking_ui.c
```

具体含义：

- `CMakeLists.txt`、`lv_conf.h`：正式目标默认构建 LVGL/DRM 和 canvas，并链接 `parking_ui_lib`；
- `include/parking_ui.h`、`src/ui/parking_ui.c`：正式停车 UI、实时 BGR 画面缩放和状态快照呈现；
- `src/main.c`：移除正式主程序的 `/dev/fb0` 路径，改由主线程驱动 `parking_ui`；
- `HANDOFF.md`：记录正式 UI 实现、验证结果和板端待验证边界。

`1bc265a` 已经提交了独立 LVGL/DRM 显示测试、zpos 修复及其构建接入；不要把这些已提交内容误判为缺失。
`eede95c` 已经提交了正式 Gate、5 秒关闸超时、板端业务回归记录和项目开发约定。
`5235b04` 已经提交了计费模块、SQLite 金额冻结及对应测试。
`390aa44` 已经提交了上一版 SQLite 存储模块及正式主程序集成。
`ae032da` 已经提交了稳定事件 FIFO 及管线集成；不要把它误写成当前未提交改动。

以上全部是用户资产。不要 reset、checkout、clean、恢复旧版或删除未跟踪文件。不要擅自提交；如需 commit，应先让用户确认提交范围和提交信息。

绝对不要执行：

```text
git reset --hard
git checkout -- .
git clean -fd
rm -rf build-rk3576
rm -rf build-rk3576-release
```

新会话读完本文后应执行：

```bash
git status --short
git diff -- CMakeLists.txt HANDOFF.md include/parking_controller.h \
  include/parking_plate_policy.h include/parking_store.h \
  src/business/parking_controller.c src/business/parking_plate_policy.c \
  src/storage/parking_store.c src/main.c \
  tests/test_parking_plate_policy.c tests/test_parking_store.c
```

并直接阅读当前正式接口、主程序和相关构建配置：

```bash
sed -n '1,260p' include/plate_pipeline.h
sed -n '1,260p' src/pipeline/plate_event_queue.h
sed -n '1,320p' src/pipeline/plate_event_queue.c
sed -n '1,760p' src/pipeline/plate_pipeline.c
sed -n '1,320p' include/parking_controller.h
sed -n '1,260p' include/parking_plate_policy.h
sed -n '1,260p' include/parking_store.h
sed -n '1,260p' include/parking_fee.h
sed -n '1,760p' src/business/parking_controller.c
sed -n '1,360p' src/business/parking_plate_policy.c
sed -n '1,360p' src/business/parking_fee.c
sed -n '1,1200p' src/storage/parking_store.c
sed -n '1,900p' src/main.c
sed -n '1,260p' CMakeLists.txt
```

## 8. 当前卡点与未完成事项

当前没有已知编译卡点或正在诊断的崩溃。真正未完成的是：

1. 尚未专门验证稳定事件 FIFO 的满队列溢出、丢弃统计和失败停止路径；用户不要求为本次 FIFO 新增独立单元测试；
2. 第一版单次业务内车牌去重和歧义策略及入口/出口 dry-run 已在板端回归成功，但没有收到完整真实识别波动日志；当前只有检测框置信度，没有 OCR 字符置信度，不能实现字符级置信度仲裁；
3. SQLite schema v1、事务、停车记录、聚合车位统计、按时长计费和正式主程序集成已经完成板端正常链路验证，但尚未专门验证断电瞬间恢复、数据库损坏、磁盘写满或 SQLite 锁竞争等故障路径；
4. 计费模块已经实现，但规则目前只在进程启动时使用默认值，尚无配置文件、后台下发或管理界面；支付仍是 dry-run。`parking_spaces` 当前只有全场总数/占用数，不是具体 A01/A02 车位分配；
5. SQLite 未完成 operation 会跨重启保留并在同一车牌再次检查时复用，不会擅自推断车辆是否已经通过；当前尚无人工列出/取消长期遗留 operation 的管理接口，因此启动日志会显示未完成入场数量；
6. 正式 `parking` 主程序入口、免费出场、收费出场和异常拒绝不开闸已提供完整板端日志并验证成功；Gate 故障路径尚未逐项验证；
7. `parking` 主程序尚未单独确认 Ctrl+C 后的完整板端清理日志；
8. 尚未实现摄像头断开重连、运行时重启和完整错误恢复；
9. 管线对象 stop 后不能再次 start；
10. 管线尚未提供查询 Camera 实际协商宽高/FPS/格式的接口；
11. 正式 `parking` 主程序的具体 FPS、延迟、丢帧和温度没有记录；
12. 当前 RKNN 推理无限速，芯片发热明显，但用户决定暂缓优化；
13. 自动关闸无物理限位、地感、防砸或车辆通过反馈；
14. 独立 LVGL DRM/KMS 测试已完成板端显示和 Ctrl+C 恢复验证；正式 `parking_ui` 也已在板端显示成功，但业务状态逐项变化与退出清理日志尚未单独确认。MQTT、正式支付和 OTA 尚未实现。

## 9. 下一步计划

### 9.1 稳定事件传递已升级

稳定事件已经从单个 `pending_event` 升级为私有的有界环形 FIFO：

```text
默认容量 32，可通过 event_queue_capacity 配置
    -> push/take 由独立 mutex 保护
    -> 按 FIFO 顺序消费
    -> 满队列拒绝当前新事件并累计 dropped_events
    -> 满队列让 plate_pipeline 失败停止，不静默继续运行
    -> stop 时关闭队列，拒绝新事件，已有事件仍可取完
```

图像帧仍保持容量为 1 的最新帧槽，不要把图像队列改成 FIFO 或无界队列。内存 FIFO 只解决短时消费积压，不等于 SQLite 持久化或断电恢复。正常板端路径已验证，专门的满队列路径尚未验证。

### 9.2 停车业务状态机核心已实现

新增文件：

```text
include/parking_controller.h
src/business/parking_controller.c
```

`parking_controller` 是纯 C、单线程驱动的单车道业务状态机，不直接调用 Camera、Recognizer、Gate、SQLite、Display 或网络。它支持 `ENTRY/EXIT` 两种模式，只有 `IDLE + Gate CLOSED` 时允许切换模式。当前状态包括：

```text
INITIALIZING
IDLE
CHECKING
WAIT_PAYMENT
OPENING
WAIT_PASSAGE
CLOSING
COMPLETING
REJECT_COOLDOWN
ERROR
```

控制器接收稳定车牌、业务检查结果、支付结果、Gate 状态、车辆通过、提交结果和复位事件，输出检查入/出场、请求支付、开/关闸、完成入/出场、拒绝和报告错误等命令。`parking_controller_tick()` 使用单调时钟处理检查、支付、Gate、通行、提交和拒绝冷却超时；`WAIT_PASSAGE` 会结合 `plate_pipeline_last_plate_seen_us()`，连续默认 5 秒没有看到车牌后输出关闸命令，并把来源标为 `PARKING_PASSAGE_VISION_TIMEOUT`。

状态机核心已加入独立 `parking_business_lib`，通过宿主严格告警检查、RK3576 严格告警检查和交叉编译。入口和出口全 dry-run 版本已经在板端回归成功。当前正式 `main.c` 继续由主线程取空稳定事件 FIFO 并驱动控制器，检查和完成提交使用 SQLite 执行器；支付仍立即模拟成功，Gate 默认模拟，但 `--enable-gate` 会使用真实 PWM 并等待异步状态完成。

当前 dry-run 链路为：

```text
稳定识别车牌
    -> parking_controller
    -> SQLite 入场/出场检查并创建持久化 PENDING operation
    -> 出口模式按入场/出场时间计费，非零费用模拟支付
    -> 模拟 Gate OPENED
    -> 连续 5 秒没有看到车牌
    -> VISION_TIMEOUT + 模拟 Gate CLOSED
    -> SQLite 事务完成入场/出场提交
```

此前全 dry-run 链路已在板端成功；2026-09-21 用户又确认 SQLite 计费版本的入场、重启和出场链路板端成功。该次验证中的 SQLite 检查、冻结金额和提交是真实持久化，支付与 Gate 是模拟。2026-09-22 用户确认当前可选真实 Gate 已能在板端由正式主程序控制；支付仍为模拟，不能称为正式支付。

### 9.3 车牌事件策略第一版已实现

新增文件：

```text
include/parking_plate_policy.h
src/business/parking_plate_policy.c
tests/test_parking_plate_policy.c
```

策略位于稳定事件 FIFO 与状态机业务处理之间，由 `parking_controller` 调用：

```text
非法 UTF-8、非 7/8 字符或不符合模型车牌字符集
    -> INVALID_PLATE
业务进行中与活动车牌完全相同
    -> IGNORE_DUPLICATE，不重复触发业务命令
业务进行中与活动车牌的 UTF-8 编辑距离为 1
    -> AMBIGUOUS_PLATE，拒绝且不自动合并
业务进行中出现差异更大的合法车牌
    -> LANE_BUSY
IDLE + 合法车牌
    -> ACCEPT
```

`detect_confidence` 是检测框置信度而不是 OCR 字符置信度。策略只校验它是 `[0,1]` 内的有限值，低阈值仍由 `plate_pipeline.confidence_threshold = 0.30` 统一过滤，不能宣称已经实现字符级置信度判断。宿主纯 C 测试和 RK3576 板端回归均已通过。

### 9.4 SQLite schema v1 与存储层已实现

新增文件：

```text
include/parking_store.h
src/storage/parking_store.c
tests/test_parking_store.c
```

schema v1 包含：

```text
schema_version       固定单行 schema 版本
parking_events       入/出场 PENDING、COMMITTED、REJECTED 事件
parking_sessions     在场/已离场停车会话、费用和 operation ID
parking_spaces       全场总车位、已占用车位和更新时间
```

关键语义：

- 所有检查和完成操作使用 `BEGIN IMMEDIATE` 事务；启用 WAL、`synchronous=FULL` 和 busy timeout；
- 活跃会话按车牌建立部分唯一索引，未完成操作按“事件类型 + 车牌”建立部分唯一索引；
- 入场检查创建 `PENDING` 操作并计入容量预留，避免并发批准超过总车位；
- 重启后同一车牌再次检查会返回原持久化 `operation_id`；
- 完成接口对已 `COMMITTED` 的同一 operation 可重复调用；
- 入场完成才创建活动会话并增加占用数，出场完成才关闭会话并释放车位；
- 业务拒绝也写入 `parking_events` 审计；
- 数据库时间使用 Unix epoch 微秒，不能使用状态机的单调时钟时间；
- 首次出场检查读取活动会话的入场时间，在同一个写事务内计算并写入 `PENDING` 事件；
- 重复出场检查不再次调用计费器，规则变化或进程重启后仍返回原 operation ID 和冻结金额；
- 当前 `parking_spaces` 是聚合统计，不提供具体车位分配。

宿主严格告警编译、临时数据库功能测试、ASan/UBSan（关闭当前受限环境不支持的 leak 检查）、RK3576 严格告警编译和 ARM64 链接均已通过。LubanCat GCC sysroot 没有 SQLite 开发包，因此实现通过 `dlopen`/`dlsym` 使用板端系统 SQLite ABI；ARM64 产物不会错误依赖宿主 SQLite。

板端回归命令如下；2026-09-21 用户已确认这一组测试和同数据库业务链路运行成功：

```bash
./test_parking_fee
./test_parking_store
./parking --lane-mode entry --database parking.db
# 入场完成后退出并保留同一个 parking.db
./parking --lane-mode exit --database parking.db
```

两个测试成功应分别输出：

```text
parking_fee: all 9 tests passed
parking_store: all tests passed
```

入口模式应看到 SQLite 检查通过、持久化 operation ID、模拟开关闸以及 `SQLite 入场完成`。退出进程后数据库必须保留；出口模式使用同一车牌和同一数据库，应查到活动会话，并按真实停车时长显示 0～2000 分。免费时直接模拟开闸，非零费用先模拟支付，再由 Gate 流程完成并输出 `SQLite 出场完成`。随后再次运行出口模式，同一车牌应被 `NO_ACTIVE_SESSION` 拒绝。入口、免费出场和收费出场的真实 Gate 正常链路均已有完整板端日志，但不能据此声称异常断电或并发故障路径也已验证。

### 9.5 停车计费模块已实现

新增文件：

```text
include/parking_fee.h
src/business/parking_fee.c
tests/test_parking_fee.c
```

默认收费规则为 30 分钟免费，超出部分每 10 分钟 50 分，不足一个周期向上取整，单次封顶 2000 分。时间使用微秒，金额使用分，全程整数计算。规则对象由模块复制保存；出场早于入场会返回错误。宿主严格告警、9 组单元测试、ASan/UBSan 和 RK3576 交叉编译均已通过；2026-09-21 用户确认板端运行成功。

### 9.6 LVGL DRM/KMS 最小显示测试已实现

新增文件：

```text
lv_conf.h
cmake/Findlibdrm.cmake
include/parking_lvgl_demo.h
src/ui/parking_lvgl_demo.c
tests/test_lvgl_display.c
```

正式 `parking` 现在固定使用 LVGL v9.6.0；`PARKING_BUILD_LVGL_TEST` 只控制是否额外构建独立测试。DRM 后端使用 XRGB8888 与 dumb buffer，不依赖 GBM/EGL。演示界面和正式界面都通过 Tiny-TTF 从已有 `fonts/platech.ttf` 加载中文字体。

`test_lvgl_display` 默认打开 `/dev/dri/card0`，connector ID 为 `-1` 自动选择，也支持 `--device`、`--connector` 和 `--font` 参数。板端已确认 `/dev/dri/card0` 的 DSI-1、1024x600、XR24 dumb buffer 和 plane 提交成功；当前最高 zpos 补丁解决了 fbcon 覆盖，用户已确认中文界面显示以及 Ctrl+C 后 fbcon 恢复。

板端运行前必须停止所有会访问 `/dev/fb0` 或 `/dev/dri/card0` 的程序，然后在包含 `fonts/platech.ttf` 的部署目录运行：

```bash
./test_lvgl_display
```

### 9.7 正式 parking_ui 已实现

新增 `include/parking_ui.h` 和 `src/ui/parking_ui.c`。模块在主线程拥有 LVGL/DRM 生命周期，通过 `parking_ui_present_bgr()` 把识别管线输出的 BGR888 画面保持比例缩放到 640x360 canvas，通过 `parking_ui_set_status()` 接收业务快照，不直接访问 Camera、SQLite、Gate 或业务状态机。

正式 `main.c` 已移除 `/dev/fb0` 的 `display_t`，改为显示实时识别画面以及入口/出口模式、车位统计、最近车牌与颜色、检测置信度、冻结费用、业务状态、真实/模拟 Gate 状态和拒绝原因。SQLite 检查与完成提交后会同步刷新车位统计；`parking_ui_process()` 与原业务循环在同一主线程运行。

宿主车牌策略、9 组计费测试和 SQLite 存储测试全部通过；`parking_ui.c` 与 `main.c` 已通过 `-Wall -Wextra -Wpedantic -Werror`，正式 `parking` 和原硬件测试已完成 RK3576 交叉编译。2026-10-11 用户确认正式 UI 已在板端显示成功；尚未收到业务状态逐项变化、长时间刷新或退出清理的完整日志。

### 9.8 后续功能

建议顺序：

```text
独立 LVGL DRM/KMS 显示已验证
    -> 正式 parking_ui 已在板端显示成功
    -> MQTT/JSON
    -> 正式支付
    -> OTA
```

正式主程序默认保持 dry-run。根据用户 2026-09-22 的明确要求，真实 PWM 通过 `--enable-gate` 启用后直接运行，不再要求人工输入 `ENABLE`；仍不能加入自动测试或默认启动路径。`test_realtime_plate_gate` 的原有人工确认保持不变。

正式 UI 使用 LVGL；当前 `display.c` 保留为测试/诊断后端。LVGL DRM/KMS 工作后不要让两个显示后端同时运行。

### 9.9 最后再处理性能与散热

整个业务完成后，再考虑 `recognition_interval_ms`、温度监测、NPU 占用和散热优化。不能在没有用户新指示时改变当前无限速推理和 3 帧确认的响应特性。

## 10. 绝对不要再踩的坑

### 10.1 不要改坏已验证模块

Camera、Image Convert、Recognizer、Gate、PWM、`latest_frame_slot`、`plate_confirmation` 和当前 `plate_pipeline` 都已有真机成功基准。新增业务应调用公开接口，不要为了方便把线程、帧槽或识别逻辑再次复制到测试或 `main.c`。

### 10.2 保持 C 测试和依赖边界

用户明确偏好测试程序使用 C。此前曾未经确认把摄像头测试改成 C++、引入额外 OpenCV imgcodecs 并扩大公共修改范围，实验程序出现过段错误；该版本已废弃，根因未确认。不要再次把 C 测试改成 C++。Recognizer 内部可继续使用 C++，外部走 C ABI。

### 10.3 严守 Camera 和 BGR 内存生命周期

正确顺序：

```text
camera_get_frame
    -> 转换/复制为独立 BGR
    -> camera_release_frame
    -> 识别/绘框/显示
    -> pipeline_frame_release
```

禁止在 `camera_release_frame()` 后访问 `CameraFrame.data`；禁止将它放进跨线程队列；禁止在 `display_present()` 前释放 BGR。

### 10.4 不要改变最新帧策略

两个帧槽容量均为 1。识别慢时覆盖并释放旧帧，避免延迟和内存无限增长。不要换成无界队列。`latest_frame_slot_take()` 的 `TIMEOUT` 是正常情况，不是错误；不要因为 100 ms 暂时无帧就停止管线。

### 10.5 不要破坏停止顺序

不能在线程仍使用 Camera/Recognizer 时销毁硬件对象。正确顺序是：

```text
设置停止标志
    -> 关闭帧槽和事件 FIFO，并唤醒等待帧的线程
    -> join 采集线程和识别线程
    -> camera_stop/camera_close
    -> destroy recognizer/slots/event queue/mutex
```

`plate_pipeline` 已封装这个顺序。外部不要绕过它直接销毁内部资源。信号处理函数只改 `sig_atomic_t`，不要在信号上下文中调用 pthread、日志、Camera、RKNN、Gate 或内存释放函数。

### 10.6 不要假设管线可重启

当前 `plate_pipeline_start()` 只能成功一次。`stop()` 会永久关闭帧槽。需要重启时 destroy 后重新 create；不要在同一对象上 stop 后再次 start。

### 10.7 不要破坏稳定事件 FIFO 的失败语义

当前稳定事件使用默认容量 32 的有界 FIFO，不再由新事件覆盖旧事件。满队列时拒绝当前新事件、累计 `dropped_events` 并让管线失败停止。不要改回单个 `pending_event`，不要改成无界队列，也不要在未定义业务恢复策略的情况下改为静默丢最旧或丢最新。FIFO 不是持久化存储，接入数据库后仍需事务、幂等更新和崩溃恢复。

### 10.8 不要误判显示技术

当前 `display.c` 写 `/dev/fb0`，是 fbdev 兼容层，不是应用直接使用原生 DRM/KMS。MIPI DSI 底层由 DRM 管理不等于当前应用是 DRM 后端。接入 LVGL 后不能同时让 fbdev 测试后端和 LVGL 写同一个显示设备。

### 10.9 不要释放 Recognizer 错误字符串

`plate_recognizer_last_error()` 返回内部只读指针，不能 `free()`。`plate_pipeline_copy_last_error()` 才是对外复制错误信息的安全接口。

### 10.10 不要用错工具链或覆盖 RKNN Runtime

必须使用 `cmake/toolchain-rk3576.cmake` 指向的 LubanCat SDK GCC 10.3 + sysroot，不要使用宿主 `/usr/bin/aarch64-linux-gnu-gcc/g++`。通过 `$ORIGIN/lib` 使用项目 `lib/librknnrt.so`，不要覆盖系统 `/lib/librknnrt.so`。

### 10.11 绝不能恢复 PWM 错误初始化顺序

RK3576 PWM 刚 export 时实测：

```text
enable=0
period=0
duty_cycle=0
polarity=inversed
```

此时再次写 `enable=0` 会返回 `EINVAL`。当前成功顺序是先建立合法 `period/duty_cycle/polarity`，真正动作时再 `enable=1`。不要恢复成无条件 `enable=0 -> period -> duty_cycle`。若接管已有 PWM，先读取真实 sysfs 状态再分支。

### 10.12 永远不要自动运行真实 Gate

`gate_create()` 会立即关闸。`test_gate` 和真实 Gate 联动都不能加入 CTest、CI、开机脚本或普通全量测试。

`test_realtime_plate_gate` 必须保持默认 dry-run；只有明确传入 `--enable-gate` 并人工输入 `ENABLE` 才能访问真实 PWM。真实测试必须有人看守、周围无人无车且机构无遮挡。

### 10.13 不要错误处理硬件

- 当前板是 `pwmchip2/pwm0`，不要猜其他编号；
- 不要用 `system("echo ...")` 控制 PWM；
- 不要无条件 unexport 其他进程导出的 PWM；
- 不要删除 `start_motion()` 中实际开始输出所需的 enable；
- 舵机必须独立稳压供电并与开发板共地，不能用 GPIO/3.3V 供电；
- 软件计时不是机械限位，也不是防砸反馈。

### 10.14 不要破坏 dirty worktree

当前所有未提交修改都是用户资产。不要 reset、checkout、clean，不要删除构建目录；以后如果出现未跟踪文件，也不能因为普通 `git diff` 看不到它们就误以为它们不重要。

### 10.15 不要把规划或旧数据写成当前事实

正式 `plate_pipeline`、独立 `plate_pipeline_lib`、稳定事件有界 FIFO、两个迁移后的硬件测试和第一版 `parking` 主程序已经交叉编译。用户已确认多线程测试、真实 Gate 测试、正式主程序的屏幕显示和稳定车牌日志、FIFO 正常路径、车牌策略测试、SQLite schema v1、按时长计费，以及同数据库入场—重启—出场链路均在板端成功。可以陈述这些正常路径事实，但不能把 FIFO 满队列、断电恢复、数据库损坏或并发故障路径写成已经验证，也不能编造正式主程序未提供的 FPS、延迟、温度和丢帧数字。正式主程序 Gate、正式支付、LVGL、MQTT 和 OTA 尚未实现。

### 10.16 不要擅自给 RKNN 限速

用户知道芯片发热并明确选择暂缓处理。不要“顺手”加入 sleep、跳帧或 `recognition_interval_ms`，也不要改变连续 3 帧确认行为。

### 10.17 不要在同一补丁中对同一路径同时 Delete/Add

`apply_patch` 不能在同一个补丁中同时 Delete 和 Add 同一路径。整文件重建时应使用 Update，或拆成两个连续补丁；删除后必须立即新增，并检查文件存在和 `git diff --check`。

### 10.18 保持主循环返回值判断正确

`plate_pipeline_take_frame()` 的返回值判断必须写成：

```c
if (result == PLATE_PIPELINE_TIMEOUT)
    continue;
if (result == PLATE_PIPELINE_CLOSED)
    break;
```

不要在 `if` 后误加分号，也不要把 `==` 写成 `=`。此前这两个错误曾导致 `display_present()` 永远不可达，并让成功取出的 BGR 帧因未调用 `pipeline_frame_release()` 而泄漏。

### 10.19 不要把相似车牌自动当成同一辆车

车牌字符串是 UTF-8，距离必须按 Unicode 码点计算，不能按字节计算。业务进行中与活动车牌完全一致才可作为重复事件忽略；编辑距离为 1 的合法字符串只能标记为 `AMBIGUOUS_PLATE`，不能自动纠正、覆盖或合并。`detect_confidence` 是检测框置信度，不是 OCR 字符置信度，不能据此声称已经实现字符级仲裁。当前策略覆盖单次状态机业务内去重；跨业务的活跃车牌唯一性、未完成 operation 复用和重复提交幂等已经由 SQLite v1 约束与事务处理，正常持久化链路已经完成板端验证。

## 11. 新会话开场清单

用户下次第一句话只会说：

```text
先读HANDOFF.md
```

新会话必须：

1. 完整阅读本文；
2. 执行 `git status --short`，不清理任何修改；
3. 阅读 `include/plate_pipeline.h`、`src/pipeline/plate_event_queue.h/.c`、`src/pipeline/plate_pipeline.c`、`include/parking_controller.h`、`src/business/parking_controller.c`、`include/parking_plate_policy.h`、`src/business/parking_plate_policy.c`、`include/parking_fee.h`、`src/business/parking_fee.c`、`include/parking_store.h`、`src/storage/parking_store.c`、`src/main.c` 和 `CMakeLists.txt`；
4. 阅读两个迁移后的测试，理解 Display 和 Gate 仍属于主线程/业务层；
5. 承认当前真实状态：正式管线、多线程识别、受控真实 Gate 测试、第一版正式 `parking` 主程序、FIFO 正常路径、入口/出口全 dry-run、车牌策略、SQLite v1、按时长计费和同数据库入场—重启—出场链路都已经板端成功；正式主程序入口、免费出场、收费出场和异常拒绝不开闸也已有完整板端日志并验证成功；满队列溢出、断电恢复和数据库故障路径尚未专门验证；
6. 不要把 `src/main.c` 恢复成占位程序，也不要重新复制 Camera、Recognizer 或管线线程逻辑；
7. 不要重新从摄像头、MIPI、RKNN 或 PWM 是否存在开始排查，除非硬件环境发生变化；
8. 默认下一步是板端验证正式 `parking_ui`，先保持 Gate dry-run；通过后再由用户决定是否回归 `--enable-gate` 真实 PWM；
9. 修改前说明文件范围；
10. 不运行真实 Gate 自动测试，不把真实 Gate 默认接入主程序；
11. 不擅自处理 RKNN 发热或改变推理频率；
12. 不编造正式 `parking` 主程序未提供的 FPS、延迟、丢帧或温度数据。

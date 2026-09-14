# Parking 项目交接文档

> 最后更新：2026-09-14（Asia/Shanghai）
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

本阶段任务是把已经真机跑通、但重复写在两个测试文件中的多线程识别管线抽成正式 `plate_pipeline` 模块，同时让两个测试改用公开接口，确保 Display 和 Gate 仍留在主线程/业务层。

本阶段已经完成，并由用户在 RK3576 板端确认：

1. `plate_pipeline` 已实现并接入构建；
2. `test_realtime_recognizer_threaded` 已迁移到 `plate_pipeline`，板端运行成功；
3. `test_realtime_plate_gate` 已迁移到 `plate_pipeline`；
4. 迁移后的真实 Gate 模式已在板端运行成功；
5. 稳定车牌仍能触发真实开闸，连续 10 秒没有有效车牌识别仍能自动关闸。

当前没有已知编译或运行卡点。下一阶段应先整理 CMake 依赖并把正式管线接入 `src/main.c`，之后再实现停车业务状态机和 SQLite；不要继续重写已经通过真机验证的 Camera、Recognizer、Gate 和管线线程逻辑。

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
迁移后的 test_realtime_recognizer_threaded   板端运行成功
迁移后的 test_realtime_plate_gate            真实 Gate 模式板端运行成功
稳定车牌 -> 真实 Gate 开闸                    已验证成功
连续 10 秒无有效车牌 -> 自动关闸              已验证成功
src/main.c 业务集成                          尚未开始，仍是占位程序
LVGL / 原生 DRM/KMS                          尚未接入
停车业务、SQLite、MQTT、计费、支付、OTA       尚未实现
```

用户没有提供本次正式 `plate_pipeline` 版本的具体 FPS、转换耗时、识别耗时、显示耗时、端到端延迟、丢帧数或芯片温度日志。不得编造这些数据；如需做性能对比，应请用户提供板端输出。

Gate 仍无物理限位、地感、防砸或车辆通过信号。当前自动关闸只依据“连续 10 秒没有识别到有效车牌”的软件计时，因此不能把当前原型当成可无人看守运行的安全系统。

用户观察到连续 RKNN 推理时芯片很烫。当前识别线程无限速，一帧完成后立即处理最新帧；最新帧槽只防止积压，不降低 NPU 占用。用户明确决定暂不限速，等整个项目完成后再处理；未经请求不要擅自加入 sleep 或改变识别频率。

## 3. 当前正式识别管线

### 3.1 文件

```text
include/plate_pipeline.h
src/pipeline/plate_pipeline.c
```

这两个文件目前还是未跟踪文件，属于用户资产，绝不能被 `git clean` 删除。

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
    -> 稳定车牌事件 + last_plate_seen_us
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

配置包括 Camera 参数、两个模型路径、字体路径、置信度阈值、确认帧数、漏检阈值、冷却时间、Camera 超时和内部帧等待时间。实现会把路径深拷贝到内部 `PATH_MAX` 数组，不依赖调用者临时字符串。

### 3.4 生命周期和线程边界

- `plate_pipeline_create()` 创建帧槽、确认器和 Recognizer，并打开 Camera，但不启动流和线程；
- `plate_pipeline_start()` 启动 Camera，先创建识别线程，再创建采集线程；
- Camera 只由采集线程调用；
- Recognizer 和确认器只由识别线程调用；
- `plate_pipeline_stop()` 设置停止标志、关闭帧槽、join 两个线程，再停止 Camera；
- `plate_pipeline_destroy()` 可重复调用 stop，然后关闭 Camera、销毁 Recognizer/帧槽/mutex 并释放对象；
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

`plate_pipeline_take_event()` 当前是非阻塞接口，返回 `1/0/-1` 表示有事件、无事件、错误。当前实现只有一个 `pending_event`，新事件可能覆盖尚未消费的旧事件。现阶段主循环每 100 ms 消费且确认事件频率低，真机验证已通过；正式停车记录不能默默丢事件，接入 SQLite/多车业务前应升级为有界 FIFO 事件队列，并明确满队列策略。

`last_plate_seen_us` 在每一帧出现任意有效最佳车牌时刷新，不受稳定确认和同车牌冷却限制，因此同一车牌持续在画面中会持续延后自动关闸。

### 3.6 当前统计与错误

统计包括：

```text
captured_frames
recognized_frames
confirmed_events
total_convert_us
total_recognize_us
capture_dropped_frames
output_dropped_frames
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
  --target test_realtime_recognizer_threaded \
           test_realtime_plate_gate \
  -j"$(nproc)"
```

本会话完成过：

- `plate_pipeline.c` RK3576 交叉编译；
- 两个迁移后的测试交叉编译并链接；
- 产物确认为 ARM aarch64；
- `plate_pipeline.c` 和两个测试额外使用 `-Wall -Wextra -Wpedantic -Werror` 编译通过；
- `git diff --check` 通过；
- 用户完成板端多线程识别回归；
- 用户完成迁移后真实 Gate 回归。

板端部署仍需要：

```text
test_realtime_recognizer_threaded
test_realtime_plate_gate
models/yolo26s-plate-detect-rk3576.rknn
models/plate_rec_color-rk3576.rknn
fonts/platech.ttf
lib/librknnrt.so
```

识别目标通过 `$ORIGIN/lib` 使用部署目录里的 `librknnrt.so`。不要覆盖系统 `/lib/librknnrt.so`。

## 7. 当前 Git 工作树

保存本文前确认：

```text
branch: feature/camera_v2
HEAD:   417c205 “测试代码使用新的模块接口”
```

写入本文后预期工作树：

```text
 M CMakeLists.txt
 M HANDOFF.md
 M tests/test_realtime_plate_gate.c
 M tests/test_realtime_recognizer_threaded.c
?? include/plate_pipeline.h
?? src/pipeline/plate_pipeline.c
```

具体含义：

- `CMakeLists.txt`：将 `src/pipeline/plate_pipeline.c` 加入 `parking_lib`；
- `include/plate_pipeline.h`：新的正式公开接口，尚未跟踪；
- `src/pipeline/plate_pipeline.c`：新的正式实现，尚未跟踪；
- 两个测试：删除重复线程/帧槽/Camera/Recognizer 管理，改用正式管线；
- `HANDOFF.md`：本交接更新。

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
git diff -- CMakeLists.txt HANDOFF.md \
  tests/test_realtime_recognizer_threaded.c \
  tests/test_realtime_plate_gate.c
```

注意：普通 `git diff` 不显示两个未跟踪的新文件，必须直接阅读：

```bash
sed -n '1,260p' include/plate_pipeline.h
sed -n '1,760p' src/pipeline/plate_pipeline.c
```

## 8. 当前卡点与未完成事项

当前没有已知编译卡点或正在诊断的崩溃。真正未完成的是：

1. `src/main.c` 仍是启动占位程序，尚未使用 `plate_pipeline`；
2. 当前 CMake 把高层 `plate_pipeline.c` 直接放进 `parking_lib`，但 `parking_lib` 没有公开声明其 `plate_recognizer_lib` 和 `Threads::Threads` 依赖；现有两个测试因自己链接了这些库而构建成功，接入 `main.c` 前应整理依赖；
3. `plate_pipeline` 的稳定事件只有单个 pending slot，正式业务可能覆盖未消费事件；
4. 尚未实现摄像头断开重连、运行时重启和完整错误恢复；
5. 管线对象 stop 后不能再次 start；
6. 测试日志现在明确打印“请求参数”，因为管线尚未提供查询 Camera 实际协商宽高/FPS/格式的接口；
7. 多线程正式版本的具体 FPS、延迟、丢帧和温度没有记录；
8. 当前 RKNN 推理无限速，芯片发热明显，但用户决定暂缓优化；
9. 自动关闸无物理限位、地感、防砸或车辆通过反馈；
10. 尚未实现入/出场业务状态机、SQLite、车位管理、LVGL、MQTT、计费、支付和 OTA。

## 9. 下一步计划

### 9.1 先整理构建依赖

推荐将 `plate_pipeline.c` 从底层 `parking_lib` 移出，在 `plate_recognizer_lib` 定义后增加独立的 `plate_pipeline_lib`：

```cmake
add_library(plate_pipeline_lib STATIC
    src/pipeline/plate_pipeline.c
)

target_include_directories(plate_pipeline_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(plate_pipeline_lib
    PUBLIC
        parking_lib
        plate_recognizer_lib
        Threads::Threads
)
```

两个多线程测试和后续 `parking` 主程序再链接 `plate_pipeline_lib`。这样依赖方向是：

```text
plate_pipeline_lib -> parking_lib + plate_recognizer_lib + Threads
```

修改后重新交叉编译两个测试；不要运行真实 Gate 作为普通构建验证。

### 9.2 接入 `src/main.c`

第一版主程序只做已验证能力的组合，不急着加入 SQLite/LVGL：

```text
安装信号处理
    -> 创建 Display
    -> 配置并创建 plate_pipeline
    -> 可选创建 Gate（默认必须保持安全，不应无确认自动驱动）
    -> 启动 plate_pipeline
    -> 主循环：取事件、更新 Gate、取绘制帧、显示
    -> 信号退出：stop pipeline、destroy pipeline、destroy display/gate
```

可以参考迁移后的 `test_realtime_plate_gate.c`，但不要简单复制一份新的重复管线。Display 和 Gate 继续在主线程，Camera/Recognizer 不再从主程序直接访问。

主程序真实 Gate 是否允许通过命令行显式启用、是否继续要求输入 `ENABLE`，属于安全策略；在用户没有新指示前，保持默认不驱动真实 PWM。

### 9.3 再做停车业务

建议顺序：

```text
入场/出场状态机
    -> 稳定事件 FIFO 与按车牌去重策略
    -> SQLite 停车记录和车位
    -> LVGL UI
    -> MQTT/JSON
    -> 计费与支付
    -> OTA
```

正式 UI 使用 LVGL；当前 `display.c` 保留为测试/诊断后端。LVGL DRM/KMS 工作后不要让两个显示后端同时运行。

### 9.4 最后再处理性能与散热

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
    -> 关闭帧槽并唤醒线程
    -> join 采集线程和识别线程
    -> camera_stop/camera_close
    -> destroy recognizer/slots/mutex
```

`plate_pipeline` 已封装这个顺序。外部不要绕过它直接销毁内部资源。信号处理函数只改 `sig_atomic_t`，不要在信号上下文中调用 pthread、日志、Camera、RKNN、Gate 或内存释放函数。

### 10.6 不要假设管线可重启

当前 `plate_pipeline_start()` 只能成功一次。`stop()` 会永久关闭帧槽。需要重启时 destroy 后重新 create；不要在同一对象上 stop 后再次 start。

### 10.7 不要忽略稳定事件可能被覆盖

当前只有一个 `pending_event`。测试循环消费足够快，因此真机已成功，但这不等于正式停车记录绝不会丢事件。接入数据库前必须设计有界 FIFO、溢出统计和明确的满队列处理策略。

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

当前修改和未跟踪文件都是用户资产。不要 reset、checkout、clean，不要删除构建目录，也不要因为普通 `git diff` 看不到未跟踪文件就误以为它们不重要。

### 10.15 不要把规划或旧数据写成当前事实

正式 `plate_pipeline` 和两个迁移后的测试已经交叉编译；用户也确认多线程测试和真实 Gate 模式在板端成功。可以陈述这些事实，但不能编造未提供的具体 FPS、延迟、温度和丢帧数字。入/出场业务、SQLite、LVGL、MQTT、计费、支付、OTA 仍未实现。

### 10.16 不要擅自给 RKNN 限速

用户知道芯片发热并明确选择暂缓处理。不要“顺手”加入 sleep、跳帧或 `recognition_interval_ms`，也不要改变连续 3 帧确认行为。

### 10.17 不要在同一补丁中对同一路径同时 Delete/Add

`apply_patch` 不能在同一个补丁中同时 Delete 和 Add 同一路径。整文件重建时应使用 Update，或拆成两个连续补丁；删除后必须立即新增，并检查文件存在和 `git diff --check`。

## 11. 新会话开场清单

用户下次第一句话只会说：

```text
先读HANDOFF.md
```

新会话必须：

1. 完整阅读本文；
2. 执行 `git status --short`，不清理任何修改；
3. 阅读未跟踪的 `include/plate_pipeline.h` 和 `src/pipeline/plate_pipeline.c`；
4. 阅读两个迁移后的测试和 `CMakeLists.txt`；
5. 承认当前真实状态：正式管线、多线程识别和真实 Gate 已经板端成功；
6. 不要重新从摄像头、MIPI、RKNN 或 PWM 是否存在开始排查，除非硬件环境发生变化；
7. 默认下一步是先整理 `plate_pipeline` 的 CMake 依赖，再接入 `src/main.c`；
8. 修改前说明文件范围；
9. 不运行真实 Gate 自动测试；
10. 不擅自处理 RKNN 发热或改变推理频率。

# Parking 项目交接文档

> 最后更新：2026-09-07（Asia/Shanghai）
>
> 新会话第一步：完整阅读本文，然后执行 `git status --short`。
>
> 本文记录当前真实状态；`Parking.md` 是长期规划，不代表其中功能已经实现。

## 1. 项目与当前任务

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
    -> LVGL 本地界面、支付、OTA 等
```

目前已打通以下链路：

1. RKNN 静态图片车牌识别；
2. Linux PWM sysfs 到真实舵机开关闸；
3. USB 摄像头实时采集、图像转换并显示到 MIPI 屏幕；
4. Camera -> BGR -> RKNN -> Draw -> Display 单线程实时车牌识别；
5. 连续 3 次确认、漏检容忍和同车牌 10 秒冷却去重；
6. 采集/转换、RKNN 识别/绘框、fbdev 显示的多线程最新帧流水线；
7. 稳定车牌事件驱动真实 Gate 开闸，连续 10 秒无车牌识别后自动关闸。

上述链路均已在 RK3576 真机验证成功。当前联动仍是独立测试程序，但第一阶段的基础模块抽取已完成：`pipeline_frame`、容量为 1 的 `latest_frame_slot` 与 `plate_confirmation` 已放入 `include/` 和 `src/pipeline/`，两个多线程测试已改为复用它们。

本次重构后的 `test_realtime_recognizer_threaded` 和 `test_realtime_plate_gate` 已成功交叉编译为 ARM aarch64，但尚未在 RK3576 板端回归。旧版测试的真机成功事实仍然有效，但不能把它写成“重构后版本已真机验证”。下一步先做板端回归，再抽取正式 `plate_pipeline`；不要直接改坏已验证的摄像头、单线程识别和 Gate 基础模块。

## 2. 当前结论速览

```text
RK3576 交叉编译                         已打通
RKNN 静态图片车牌识别                   已在 RK3576 真机验证
Gate 状态机 + PWM sysfs                已实现
RK3576 PWM + 舵机真实开关闸             已验证成功
USB 摄像头 V4L2 mmap 采集              已验证成功
MJPG/YUYV -> BGR888                    已实现
BGR888 -> /dev/fb0 -> MIPI DSI         已实现并完成显示测试
摄像头实时车牌识别                      已在 RK3576 真机验证
连续多帧确认与相同车牌冷却去重          已在 RK3576 真机验证
采集/识别/显示多线程与最新帧槽          已在 RK3576 真机验证
稳定车牌事件 -> Gate 受控联动             已在 RK3576 真机验证
连续 10 秒无车牌识别 -> 自动关闸          已在 RK3576 真机验证
基础模块 pipeline_frame/latest_frame_slot    已抽取并交叉编译
基础模块 plate_confirmation                 已抽取并交叉编译
两个多线程测试复用新模块             交叉编译通过，待板端回归
正式 plate_pipeline 管线                    尚未抽取
LVGL / 原生 DRM/KMS 界面                尚未接入
停车业务、SQLite、MQTT、计费、支付、OTA  尚未实现
src/main.c 业务集成                     尚未开始，仍是启动占位程序
```

当前没有已知编译卡点，最近的验证缺口是模块化后两个多线程测试尚未做板端回归。Gate 尚无物理限位、地感、防砸或车辆通过信号；当前自动关闸只依据“连续 10 秒没有识别到有效车牌”的软件计时，因此不能把当前原型当成可无人看守运行的安全系统。

用户还观察到当前连续 RKNN 推理时芯片很烫。原因与现有识别线程无限速、一帧完成后立即推理下一帧一致；最新帧槽只防止积压，不降低 NPU 占用。用户明确决定暂不限速，等整个项目完成后再处理；未经请求不要擅自改变当前识别频率。

## 3. 已确认的真实硬件信息

### 3.1 USB 摄像头

摄像头由 `uvcvideo` 驱动：

```text
/dev/video0  视频采集
/dev/video1  同一 UVC 设备的另一个节点，不要默认它是主视频节点
/dev/media0
```

`/dev/video0` 已确认支持：

```text
MJPG: 1920x1080@30, 1280x720@30, 640x480@30 等
YUYV: 1920x1080@5, 1280x720@10, 640x480@30 等
```

已用以下命令成功抓取单张 1280x720 MJPG：

```bash
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=1280,height=720,pixelformat=MJPG \
  --stream-mmap \
  --stream-count=1 \
  --stream-to=test.jpg
```

当前实时显示推荐/已使用参数：

```text
/dev/video0 1280x720 30 FPS MJPG
```

对比无 JPEG 解码路径时可用 `640x480 30 FPS YUYV`。

### 3.2 MIPI 屏幕

板端确认：

```text
/dev/fb0                         存在
/dev/dri/card0                  Rockchip display subsystem
/sys/class/drm/card0-DSI-1      connected
/sys/class/drm/card0-DP-1       disconnected
/dev/dri/card1                  NPU DRM 节点，不是显示屏
```

当前 `display.c` 使用：

```text
open/ioctl/mmap /dev/fb0
    -> DRM 驱动提供的 fbdev 兼容层
    -> MIPI DSI 屏幕
```

它不是原生 DRM/KMS。fbdev 用于学习、硬件验证和诊断；正式 UI 计划使用 LVGL，优先考虑 LVGL DRM/KMS 后端。不要让当前 `display.c` 与 LVGL 同时接管或写同一个显示设备。

### 3.3 Gate/PWM

已确认路径：

```text
/sys/class/pwm/pwmchip2/pwm0
```

真机已确认 `pwmchip2`、`npwm=1`、`channel=0`、export、`gate_create()`、`gate_open()`、`gate_close()` 均成功。

当前 `tests/test_gate.c` 参数：

```text
pwmchip_path     = /sys/class/pwm/pwmchip2
channel          = 0
period_ns        = 20000000
open_pulse_ns    = 2400000
close_pulse_ns   = 1400000
movement_time_ms = 800
hold_after_move  = 0
```

这些数值只对当前舵机和机械结构有效，更换硬件后必须重新校准。

## 4. 当前模块

### 4.1 Camera：V4L2 采集

```text
include/camera.h
src/camera/camera.c
```

```c
int camera_open(Camera *camera, const CameraConfig *config);
int camera_start(Camera *camera);
int camera_get_frame(Camera *camera, CameraFrame *frame, int timeout_ms);
int camera_release_frame(Camera *camera, CameraFrame *frame);
int camera_stop(Camera *camera);
void camera_close(Camera *camera);
```

实现了 V4L2 单平面 `QUERYCAP/S_FMT/S_PARM/REQBUFS/mmap/QBUF/STREAMON/poll/DQBUF` 流程。

关键内存语义：

- `CameraFrame.data` 是指向 V4L2 mmap buffer 的借用指针；
- `camera_release_frame()` 后绝不能继续访问它；
- 推理前先转换/复制到自有缓冲区，再归还 V4L2 buffer；
- 跨线程时不能把 `CameraFrame.data` 裸指针直接放进队列。

### 4.2 Image Convert：统一 BGR888

```text
include/image_convert.h
src/image/image_convert.c
```

```c
int image_convert_to_bgr(const CameraFrame *source,
                         image_bgr_frame_t *destination);
void image_bgr_frame_release(image_bgr_frame_t *frame);
```

支持：

```text
MJPG -> stb_image JPEG 解码 -> RGB -> 原地交换成 BGR888
YUYV -> 纯 C 颜色转换 -> BGR888
```

输出 `image_bgr_frame_t.data` 是独立分配的内存，使用后必须调用 `image_bgr_frame_release()`。统一 BGR888 是为了直接对接 RKNN 识别、绘制和显示。

### 4.3 Display：fbdev 测试后端

```text
include/display.h
src/display/display.c
```

```c
display_t *display_create(const display_config_t *config);
int display_present(display_t *display, const display_frame_t *frame);
unsigned int display_width(const display_t *display);
unsigned int display_height(const display_t *display);
void display_destroy(display_t *display);
```

`display_t` 是不透明结构体，真实字段只在 `display.c` 内。当前支持：

- 打开并 mmap `/dev/fb0`；
- 查询分辨率、stride、bpp、RGB 位域；
- 输入 RGB888/BGR888；
- 输出 16/24/32 bpp framebuffer；
- 保持宽高比、居中、黑边；
- 最近邻缩放和颜色查找表。

限制：CPU 逐像素处理、无双缓冲/垂直同步，可能撕裂。它是测试/学习模块，不是最终 LVGL UI。

### 4.4 Camera 实时显示测试

```text
tests/test_camera.c
CMake 目标：test_camera
```

这是纯 C 测试，没有使用 OpenCV。流程：

```text
camera_get_frame()
    -> image_convert_to_bgr()
    -> camera_release_frame()
    -> display_present(BGR888)
    -> image_bgr_frame_release()
```

转换必须在 `camera_release_frame()` 前完成；显示必须在 `image_bgr_frame_release()` 前完成。

运行：

```bash
./test_camera [camera] [width] [height] [fps] [MJPG|YUYV] [framebuffer]

./test_camera /dev/video0 1280 720 30 MJPG /dev/fb0
```

支持 SIGINT/SIGTERM 清理，并打印包含采集、解码、转换、显示在内的端到端 FPS。

### 4.5 RKNN 车牌识别

```text
include/plate_recognizer.h
src/recognizer/
tests/test_recognizer.cpp
models/yolo26s-plate-detect-rk3576.rknn
models/plate_rec_color-rk3576.rknn
fonts/platech.ttf
lib/librknnrt.so
```

静态图片识别已在 RK3576 真机成功，曾识别出 `川AA16052，绿色车牌`。

识别器内部是 C++/OpenCV/RKNN，但头文件暴露 C ABI，因此新的实时测试可以写成 `.c`。

注意：`plate_recognizer_last_error()` 实际返回内部 `std::string::c_str()`，调用者不能 `free()`。当前头文件“必须由调用者释放”的注释是错误的，以后应只修正注释。

`test_recognizer` 通过 `$ORIGIN/lib` 使用部署目录里的 `librknnrt.so`，不要覆盖系统 `/lib/librknnrt.so`。

### 4.6 Camera 单线程实时车牌识别测试

```text
tests/test_realtime_recognizer.c
CMake 目标：test_realtime_recognizer
```

该测试保持纯 C 源码，通过识别器现有 C ABI 调用内部 C++/OpenCV/RKNN 实现。处理顺序：

```text
camera_get_frame()
    -> image_convert_to_bgr()
    -> camera_release_frame()
    -> plate_recognizer_process()
    -> 连续多帧确认/冷却去重
    -> 确认后打印车牌号/颜色/置信度
    -> plate_recognizer_draw()
    -> display_present(BGR888)
    -> image_bgr_frame_release()
```

2026-09-03 已在 RK3576 真机验证成功：摄像头实时画面显示到 MIPI 屏幕，对不同省份车牌照片能绘制识别框并在终端输出车牌号、颜色和置信度。已成功识别包括 `贵B8P372`、`赣B8P372`、`川B8P372`，蓝色车牌颜色置信度约 `0.92～0.98`，检测置信度约 `0.89～0.91`。

实测性能（1280x720@30 MJPG 输入，单线程每帧识别）：

```text
实际处理 FPS      约 6.2～7.3
平均 MJPG/BGR 转换 约 22.8～28.3 ms
平均 RKNN 识别     约 100.4～104.9 ms
平均整帧处理       约 136.1～161.0 ms
```

由于当前是单线程每帧推理，显示速度被转换、RKNN 推理和 fbdev 显示共同限制；这是已知性能基线，不是当前故障。

2026-09-04 已在该测试中加入简单结果稳定状态机：每帧选择检测置信度最高的有效车牌，同一车牌稳定出现 3 次后才输出一次“确认车牌”事件，最多容忍 2 帧漏检，连续漏检 3 帧后重置候选，同一已确认车牌使用 10 秒冷却。识别框仍逐帧绘制，逐帧车牌日志改为只输出确认事件。该功能已在 RK3576 真机验证成功。

当前冷却实现只保存一个 `last_confirmed_plate`：同一车牌持续留在画面中时，`candidate_confirmed` 会使它只确认一次；候选被重置后在 10 秒内再次出现会被冷却拦截。这是测试级简化实现，未来多车业务需要使用按车牌管理的事件/冷却表。

### 4.7 Camera 多线程实时车牌识别测试

```text
tests/test_realtime_recognizer_threaded.c
CMake 目标：test_realtime_recognizer_threaded
```

该文件仍是纯 C，通过 pthread 实现三段流水线：

```text
采集/转换线程
    -> 容量 1 的 captured_frames 最新帧槽
    -> RKNN 识别/确认/绘框线程
    -> 容量 1 的 annotated_frames 最新帧槽
    -> 主线程 display_present()
```

帧槽由已抽取的 `latest_frame_slot` 模块提供，内部使用 `pthread_mutex_t + pthread_cond_t`。生产者发现槽中已有未消费帧时，会先释放旧 BGR 内存再放入最新帧，不会无限积压。消费者通过结构体移交取得 BGR 所有权，处理完后负责释放。车牌确认使用已抽取的 `plate_confirmation` 模块。

线程边界：

- Camera 只由采集线程调用 `get/release`；
- `plate_recognizer_t` 只由识别线程使用；
- `display_t` 只由主线程使用；
- SIGINT/SIGTERM 只设置退出标志，调用者通过 `latest_frame_slot_take(..., 100)` 每 100 ms 超时检查一次；
- 退出时先关闭帧槽并 `pthread_join()` 两个工作线程，再停止摄像头、销毁识别器与显示器。

每秒输出：

```text
FPS，平均转换/识别/显示耗时，端到端延迟，采集槽/显示槽丢帧数
```

2026-09-05 用户确认该多线程版本已在 RK3576 真机测试成功。本会话没有收到多线程版的具体 FPS/延迟/丢帧日志，后续不得编造具体数据；如需做性能对比，请用户提供一段真机输出。

2026-09-07 已将该文件改为复用 `pipeline_frame`、`latest_frame_slot` 和 `plate_confirmation`，删除文件内的重复实现；`test_realtime_recognizer_threaded` 交叉编译成功。此重构后版本尚未板端回归，因此 2026-09-05 的真机成功事实只能用作重构前基准。

### 4.8 Camera 实时车牌识别与 Gate 联动测试

```text
tests/test_realtime_plate_gate.c
CMake 目标：test_realtime_plate_gate
```

该纯 C 测试复用多线程最新帧流水线，并将稳定车牌事件与 Gate 受控联动。默认运行是 dry-run，不访问 PWM：

```bash
./test_realtime_plate_gate
```

真实 Gate 必须使用显式参数，并在启动时阅读警告、人工输入 `ENABLE`；注意 `gate_create()` 会立即执行一次关闸初始化：

```bash
./test_realtime_plate_gate --enable-gate
```

联动语义：

- 同一车牌连续识别 3 帧后才产生稳定确认事件并请求开闸；
- Gate 只有处于 `CLOSED` 时才接受开闸请求，正在运动或已经打开时忽略重复请求；
- 主线程周期调用 `gate_update()`，开/关动作保持非阻塞；
- 开闸后，每一帧的任意有效车牌识别都会刷新 `last_plate_seen_us`；
- 连续 10 秒没有识别到有效车牌时调用 `gate_close()`；
- 关闸倒计时依据逐帧有效识别，而不是受 10 秒冷却限制的稳定确认事件，所以同一车牌持续在画面中会持续刷新倒计时；
- dry-run 同样打印模拟开闸和模拟关闸，便于先验证业务逻辑；
- 退出时先停止并 join 采集/识别线程，最后销毁 Camera、Recognizer、Display 和 Gate。

真实 Gate 参数与 `tests/test_gate.c` 一致：

```text
pwmchip_path     = /sys/class/pwm/pwmchip2
channel          = 0
period_ns        = 20000000
open_pulse_ns    = 2400000
close_pulse_ns   = 1400000
movement_time_ms = 800
hold_after_move  = 0
```

2026-09-06 用户确认已在 RK3576 开发板真机测试成功：稳定识别车牌后真实 Gate 能开闸，连续 10 秒没有再次识别到车牌后能自动关闸。

2026-09-07 已将该文件改为复用 `pipeline_frame`、`latest_frame_slot` 和 `plate_confirmation`，删除文件内的重复实现；`test_realtime_plate_gate` 交叉编译成功。重构后尚未运行默认 dry-run，更没有再次启用真实 Gate；2026-09-06 的真机联动成功是重构前基准。

### 4.9 已抽取的管线基础模块

```text
include/pipeline_frame.h
src/pipeline/pipeline_frame.c
include/latest_frame_slot.h
src/pipeline/latest_frame_slot.c
include/plate_confirmation.h
src/pipeline/plate_confirmation.c
```

`pipeline_frame_t` 持有独立 BGR 图像及转换/识别耗时字段。`pipeline_frame_release()` 会调用 `image_bgr_frame_release()` 并清空整个结构体，可用于帧槽覆盖、消费者处理完成与退出清理。

`latest_frame_slot_t` 是不透明类型，主要接口：

```c
latest_frame_slot_t *latest_frame_slot_create(void);
int latest_frame_slot_push(latest_frame_slot_t *slot,
                           pipeline_frame_t *frame);
int latest_frame_slot_take(latest_frame_slot_t *slot,
                           pipeline_frame_t *frame,
                           int timeout_ms);
void latest_frame_slot_close(latest_frame_slot_t *slot);
uint64_t latest_frame_slot_dropped(latest_frame_slot_t *slot);
void latest_frame_slot_destroy(latest_frame_slot_t *slot);
```

`push()` 成功后所有权转给帧槽并清空输入帧；覆盖时先释放旧帧。`take()` 成功后所有权转给消费者，可返回 `OK/TIMEOUT/CLOSED/ERROR`。曾在实现过程中把 `ETIMEDOUT` 误当成错误，现已修正为 `LATEST_FRAME_SLOT_TIMEOUT`。

`plate_confirmation` 通过配置控制确认帧数、漏检重置阈值和同车牌冷却时间。当前两个测试仍配置为 3 次识别、3 帧漏检重置和 10 秒冷却。

上述模块已加入 `parking_lib`。用户决定跳过独立纯软件单元测试，所以目前验证范围是“交叉编译通过”，不能宣称已完成独立边界测试或重构后真机回归。

### 4.10 Gate

```text
include/gate.h
include/pwm_sysfs.h
src/gate/gate.c
src/gate/pwm_sysfs.c
tests/test_gate.c
```

Gate 命令非阻塞，必须周期调用 `gate_update()`。没有物理限位，`movement_time_ms` 到期只是软件推测完成。

不影响真机原型验证的待收尾项：

- `test_gate.c` 仍使用 `printf()`；
- 无启动前人工确认；
- 无 SIGINT/SIGTERM 清理；
- `wait_gate_finished()` 无软件超时；
- 重复 open/close 的返回语义不一致；
- 无限位、防砸、地感、电流/堵转检测。

### 4.11 主程序

`src/main.c` 仍基本只有：

```c
puts("Parking system started.");
```

尚未集成 Gate、Camera、Recognizer、LVGL 或停车业务。

## 5. 构建与部署

工具链：

```text
cmake/toolchain-rk3576.cmake
```

必须使用 LubanCat SDK GCC 10.3 和配套 sysroot。可靠构建目录：

```text
build-rk3576-release
```

```bash
cd /home/dyj/project/Parking

cmake -S . -B build-rk3576-release \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-rk3576.cmake" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-rk3576-release \
  --target test_camera \
           test_realtime_recognizer \
           test_realtime_recognizer_threaded \
           test_realtime_plate_gate \
  -j"$(nproc)"

file build-rk3576-release/test_camera \
     build-rk3576-release/test_realtime_recognizer \
     build-rk3576-release/test_realtime_recognizer_threaded \
     build-rk3576-release/test_realtime_plate_gate
```

四个目标均已交叉编译为 ARM aarch64。2026-09-07 抽取模块并迁移使用方后，`test_realtime_recognizer_threaded` 与 `test_realtime_plate_gate` 均再次交叉编译成功，`git diff --check` 通过。三个识别目标都通过 `$ORIGIN/lib` 查找部署目录中的 `librknnrt.so`。两个多线程目标使用 CMake `find_package(Threads REQUIRED)` 和 `Threads::Threads`。真实硬件测试不要加入 CTest 或 CI 自动运行。

板端部署目录需要同时包含：

```text
test_realtime_recognizer_threaded
test_realtime_plate_gate
models/yolo26s-plate-detect-rk3576.rknn
models/plate_rec_color-rk3576.rknn
fonts/platech.ttf
lib/librknnrt.so
```

从该目录运行：

```bash
./test_realtime_plate_gate
# 明确启用真实 PWM 时：
./test_realtime_plate_gate --enable-gate
```

## 6. 当前 Git 工作树

当前分支和 HEAD：

```text
branch: feature/camera_v2
HEAD:   4e2c27b 新增车牌识别流程模块
```

`pipeline_frame`、`latest_frame_slot`、`plate_confirmation` 及其 CMake 接入已在当前 HEAD 中。本次会话继续把两个多线程测试迁移到新模块。

保存本文时实际观察到的工作树是：

```text
M  HANDOFF.md
M  tests/test_realtime_plate_gate.c
M  tests/test_realtime_recognizer_threaded.c
```

两个测试文件的未提交修改都是本次模块复用迁移；`HANDOFF.md` 是本次交接更新。这些全部视为用户资产。

绝对不要执行：

```text
git reset --hard
git checkout -- .
git clean -fd
rm -rf build-rk3576
rm -rf build-rk3576-release
```

新会话先执行：

```bash
git status --short
git diff -- HANDOFF.md \
  tests/test_realtime_recognizer_threaded.c \
  tests/test_realtime_plate_gate.c
```

如新会话看到其他修改，视为用户在之后产生的资产，不得清理。

## 7. 当前卡点与未完成事项

当前没有已知编译卡点，也没有正在诊断的崩溃。此前实验性 C++/OpenCV 摄像头程序出现过段错误，但该版本已废弃，不能把它当成当前问题。

当前真正未完成：

1. 模块化后的 `test_realtime_recognizer_threaded` 尚未在 RK3576 板端回归；
2. 模块化后的 `test_realtime_plate_gate` 尚未运行默认 dry-run 回归，也尚未再次做真实 Gate 回归；
3. 完整的 Camera -> Recognizer -> Event 识别管线与 Gate 联动仍在测试文件中，尚未抽取为正式 `plate_pipeline` 模块；
4. 10 秒定时关闸没有限位、地感或防砸反馈，只适合有人看守的当前原型；
5. 多线程版具体 FPS、延迟、丢帧数和芯片温度尚未记入文档；
6. 当前 RKNN 推理无限速，芯片很烫；用户决定整个项目完成后再加 `recognition_interval_ms` 等限速/散热优化；
7. 尚未实现摄像头断开重连和完整错误统计；
8. 尚未接入 LVGL；
9. 尚未实现入/出场状态机、SQLite、车位、MQTT、计费、支付和 OTA；
10. `src/main.c` 仍是启动占位程序。

## 8. 下一步计划

### 8.1 先回归模块化后的两个测试

先将新交叉编译产物和现有模型/字体/RKNN Runtime 部署到 RK3576，依次运行：

```bash
./test_realtime_recognizer_threaded
./test_realtime_plate_gate
```

第二个必须先不带参数，仅运行 dry-run。验证内容：采集、转换、RKNN 识别、绘框、显示、3 次确认、10 秒冷却、模拟开闸以及 10 秒无识别模拟关闸。同时保存一段 FPS/转换/识别/显示/延迟/丢帧日志和芯片温度。

只有 dry-run 正常后，且用户明确要复测真实机构时，才能在有人看守、无人无车阻挡的条件下使用：

```bash
./test_realtime_plate_gate --enable-gate
```

### 8.2 抽取正式 `plate_pipeline`

板端回归成功后，再从两个测试中抽取 Camera -> BGR -> Recognizer -> Draw -> Event 的线程管线，建议文件：

```text
include/plate_pipeline.h
src/pipeline/plate_pipeline.c
```

定义清晰的创建、启动、停止、取最新绘制帧、取车牌事件、查询 `last_plate_seen_us`、错误和统计接口。Camera 只由采集线程使用，Recognizer 只由识别线程使用；Display 与 Gate 先留在主线程/业务层，不要塞进识别管线。

用户已决定暂不处理过热，因此此阶段保持当前无限速推理行为。可在配置设计中预留 `recognition_interval_ms`，但不要在没有用户新指示时启用限速。完成正式管线后再接入 `src/main.c`。

### 8.3 停车业务与 UI

随后建议顺序：入/出场状态机 -> SQLite/车位 -> LVGL -> MQTT/JSON -> 计费/支付 -> OTA。正式 MIPI UI 使用 LVGL；`display.c` 保留为 fbdev 测试/诊断后端。LVGL DRM/KMS 工作后不要让两个显示后端同时运行。

## 9. 绝对不要再踩的坑

### 9.1 尊重 C 语言和修改范围

用户明确偏好测试程序使用 C。此前曾未经确认把 `test_camera.c` 改为 C++、引入 OpenCV imgcodecs 并改公共日志/摄像头代码，板端实验程序出现段错误；该做法已撤销，段错误根因未确认。

以后不要擅自把 C 测试改 C++，不要为了方便扩大公共依赖；修改前说明文件范围。识别器内部可继续是 C++，通过现有 C ABI 给 C 测试调用。

### 9.2 不要误判显示技术

当前 `display.c` 是 `/dev/fb0` fbdev，不是直接 DRM/KMS。MIPI DSI 底层由 DRM 管理，不代表应用直接使用 DRM。接 LVGL 后不能同时运行两个显示后端。

### 9.3 严守图像内存生命周期

正确：

```text
get -> 转换/复制 BGR -> release CameraFrame
    -> 识别/绘制/显示 -> release BGR
```

禁止在 `camera_release_frame()` 后使用 `CameraFrame.data`。`display_frame.data` 只是别名，没有复制 BGR，禁止在显示前释放 BGR。

### 9.4 不要无限积压帧

多线程只使用容量 1～2 的最新帧队列。识别慢时丢旧帧，不能让延迟和内存持续增长。当前多线程测试的两个槽都是容量 1；新帧覆盖旧帧时必须先 `image_bgr_frame_release()` 旧帧。

`CameraFrame.data` 永远不能跨线程入队。队列中只能放置已转换、拥有独立内存的 BGR 帧。现在必须使用 `latest_frame_slot` 的所有权语义：`push()` 成功后输入帧已清空，`take()` 成功后由消费者调用 `pipeline_frame_release()`。不要重新在测试文件中复制一套静态帧槽实现。

### 9.5 不要释放识别器错误字符串

`plate_recognizer_last_error()` 返回内部指针，调用者只读，不得 `free()`；头文件现有释放说明有误。

### 9.6 不要用错工具链或 CMake 缓存

正确工具链路径：

```text
cmake/toolchain-rk3576.cmake
```

不要使用宿主 `/usr/bin/aarch64-linux-gnu-gcc/g++`，必须用 LubanCat SDK GCC 10.3 + sysroot。当前可靠构建目录是 `build-rk3576-release`；工具链变化时用新目录或确认后 fresh 配置，不要粗暴清理用户产物。

### 9.7 不要覆盖 RKNN Runtime

通过 `$ORIGIN/lib` 使用项目 `lib/librknnrt.so`，不要覆盖系统 `/lib/librknnrt.so`。

### 9.8 绝不能恢复 PWM 错误初始化顺序

RK3576 PWM 刚 export 时实测：

```text
enable=0
period=0
duty_cycle=0
polarity=inversed
```

此时再次写 `enable=0` 会返回 `EINVAL`。当前成功顺序是先建立合法 `period/duty_cycle/polarity`，真正动作时再 `enable=1`。不要恢复成无条件 `enable=0 -> period -> duty_cycle`。若接管已有 PWM，先读取真实 sysfs 状态再分支。

### 9.9 不要自动运行真实 Gate 测试

`gate_create()` 会立即关闸。不要把 `test_gate` 放入 CTest、CI、开机脚本或普通全量自动测试。

识别/Gate 联动测试必须保持默认 dry-run，必须由明确参数和人工输入 `ENABLE` 才能启用真实 PWM。当前真实模式会在连续 10 秒没有有效车牌识别后自动关闸；没有传感器防砸，禁止放入 CTest、CI、开机脚本或无人看守环境。

### 9.10 不要错误处理 PWM 硬件

- 不要猜其他板子的 `pwmchipN`；本板当前才是 `pwmchip2/pwm0`；
- 不要用 `system("echo ...")` 控制 PWM；
- 不要无条件 unexport 其他进程导出的 PWM；
- 不要删除 `start_motion()` 中真正开始输出所需的 enable；
- 不要用 GPIO/3.3V 给舵机供电，舵机独立稳压供电并与开发板共地；
- 不要把软件计时状态当机械限位或安全反馈。

### 9.11 不要破坏 dirty worktree

源码修改和未跟踪文件属于用户资产。不要 reset、checkout、clean 或随意删除构建目录。

### 9.12 不要把规划写成已完成

摄像头实时显示、单线程识别、多帧确认/去重、多线程最新帧识别、稳定车牌开闸和 10 秒无识别自动关闸均已在重构前版本真机验证。`pipeline_frame`、`latest_frame_slot`、`plate_confirmation` 已抽取并被两个测试复用，但重构后尚未板端回归。完整正式 `plate_pipeline`、入出场业务、LVGL、SQLite、MQTT、计费、支付、OTA 尚未完成。

### 9.13 不要在同一个补丁中对同一路径同时 Delete/Add

`apply_patch` 不允许在同一个补丁中同时执行：

```text
Delete File: HANDOFF.md
Add File: HANDOFF.md
```

这会在校验阶段报错：

```text
invalid patch: multiple operations target .../HANDOFF.md
```

整文件重写时优先使用一次 `Update File`。如果内容过大、确实需要重建文件，则拆成两个补丁：先删除、再立即新增；删除后必须确认新增成功，避免文件暂时缺失。完成后检查：

```bash
test -f HANDOFF.md
git diff --check -- HANDOFF.md
```

若补丁在校验阶段失败，应先确认原文件没有发生变化，再选择正确的补丁方式重试。

### 9.14 多线程退出时不要先销毁硬件对象

不要在采集线程仍可能调用 Camera、识别线程仍可能调用 RKNN 时就执行 `camera_stop()`、`camera_close()` 或 `plate_recognizer_destroy()`。正确顺序是设置停止状态、关闭帧槽/唤醒等待者、join 工作线程，最后销毁 Camera、Recognizer、Display 和同步原语。

信号处理函数只做 `running = 0`，不要在信号处理函数里调用 pthread 条件变量、日志、RKNN、Camera 或内存释放函数。

### 9.15 不要把帧槽正常超时当成管线错误

`latest_frame_slot_take()` 有四种返回状态：`LATEST_FRAME_SLOT_OK`、`LATEST_FRAME_SLOT_TIMEOUT`、`LATEST_FRAME_SLOT_CLOSED`、`LATEST_FRAME_SLOT_ERROR`。100 ms 内没有帧是正常 `TIMEOUT`，调用者应 `continue` 后重试，不能停止管线。实现时曾漏掉 `ETIMEDOUT` 分支导致误报错，已修正，不要回退。

### 9.16 暂时不要擅自改变 RKNN 推理频率

当前识别线程完成一帧后立即处理最新帧，会让 NPU 近乎连续工作，用户已观察到芯片很烫。建议的后续方案是配置 `recognition_interval_ms`，例如从 250 ms（约 4 次/秒）开始评估，但用户已决定项目整体完成后再处理。下次会话不要为了“顺手优化”就加 sleep 或改变 3 帧确认响应。

## 10. 新会话开场检查清单

用户新会话第一句话会是：

```text
先读HANDOFF.md
```

读完后：

1. 执行 `git status --short`，不要清理工作树；
2. 阅读 `CMakeLists.txt`、`include/pipeline_frame.h`、`src/pipeline/pipeline_frame.c`、`include/latest_frame_slot.h`、`src/pipeline/latest_frame_slot.c`、`include/plate_confirmation.h`、`src/pipeline/plate_confirmation.c`；
3. 阅读 `tests/test_realtime_recognizer_threaded.c`、`tests/test_realtime_plate_gate.c`，继续识别/Gate 工作前再读 `include/plate_recognizer.h`、`include/gate.h`、`src/gate/gate.c` 和 `tests/test_gate.c`；
4. 承认当前真机事实：Gate 开关成功、静态图片识别成功、USB 摄像头实时显示到 MIPI 成功、单线程实时识别成功、多帧确认/去重成功、多线程最新帧识别成功、稳定车牌开闸成功、连续 10 秒无车牌识别自动关闸成功；同时明确这些是模块复用迁移前的真机基准；
5. 不要重新从摄像头/MIPI/PWM 是否存在开始排查，除非硬件变了；
6. 默认下一步是先在板端回归模块化后的两个多线程测试，Gate 联动先只运行默认 dry-run；通过后再抽取正式 `plate_pipeline`；
7. 不要为了方便改坏 `test_camera.c`、`test_realtime_recognizer.c`、`test_realtime_recognizer_threaded.c` 或 `test_realtime_plate_gate.c`，也不要重新复制已抽取的帧槽/确认逻辑；
8. 当前 RKNN 无限速会导致芯片很烫，用户明确暂缓处理，不要擅自加限速；
9. 修改前先向用户说明文件范围，避免擅自扩大修改。

# Parking 项目交接文档

> 最后更新：2026-09-04（Asia/Shanghai）
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

目前已打通三条基础链路和一条实时集成链路：

1. RKNN 静态图片车牌识别；
2. Linux PWM sysfs 到真实舵机开关闸；
3. USB 摄像头实时采集、图像转换并显示到 MIPI 屏幕；
4. Camera -> BGR -> RKNN -> Draw -> Display 单线程实时车牌识别。

当前单线程实时识别已在 RK3576 真机验证成功；连续多帧确认和相同车牌冷却去重已实现并通过交叉编译，尚待 RK3576 真机验证。真机确认成功后再实现最新帧队列和多线程。

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
连续多帧确认与相同车牌冷却去重          已实现，待 RK3576 真机验证
采集/识别多线程与最新帧队列              尚未实现
LVGL / 原生 DRM/KMS 界面                尚未接入
停车业务、SQLite、MQTT、计费、支付、OTA  尚未实现
src/main.c 业务集成                     尚未开始，仍是启动占位程序
```

当前没有卡在摄像头、MIPI 屏幕、PWM 或 RKNN 识别上。当前要先验证车牌确认与去重的真机行为，然后再进入多线程低延迟处理。

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

2026-09-04 已在该测试中加入简单结果稳定状态机：每帧选择检测置信度最高的有效车牌，同一车牌稳定出现 3 次后才输出一次“确认车牌”事件，最多容忍 2 帧漏检，连续漏检 3 帧后重置候选，同一已确认车牌使用 10 秒冷却。识别框仍逐帧绘制，逐帧车牌日志改为只输出确认事件。已通过 RK3576 交叉编译，尚未真机验证。

### 4.7 Gate

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

### 4.8 主程序

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
  --target test_camera test_realtime_recognizer \
  -j"$(nproc)"

file build-rk3576-release/test_camera \
     build-rk3576-release/test_realtime_recognizer
```

`test_camera` 和 `test_realtime_recognizer` 均已交叉编译为 ARM aarch64。`test_realtime_recognizer` 通过 `$ORIGIN/lib` 查找部署目录中的 `librknnrt.so`。真实硬件测试不要加入 CTest 或 CI 自动运行。

## 6. 当前 Git 工作树

保存本文后，预期 dirty 状态至少包括：

```text
M  HANDOFF.md
M  CMakeLists.txt
M  include/camera.h
M  tests/test_camera.c
?? include/display.h
?? include/image_convert.h
?? src/display/
?? src/image/
?? tests/test_realtime_recognizer.c
```

这些均视为用户资产。绝对不要执行：

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
git diff -- CMakeLists.txt include/camera.h tests/test_camera.c HANDOFF.md
```

未跟踪的 display/image 文件和 `tests/test_realtime_recognizer.c` 不会出现在普通 `git diff` 中，必须直接阅读。

## 7. 当前卡点与未完成事项

当前没有硬件卡点，也没有正在诊断的崩溃。此前实验性 C++/OpenCV 摄像头程序出现过段错误，但该版本已废弃，不能把它当成当前问题。

当前真正未完成：

1. 连续多帧确认和相同车牌冷却去重尚待 RK3576 真机验证；
2. 尚未决定每帧识别还是跳帧识别；
3. 尚未实现采集/识别线程和容量 1～2 的最新帧队列；
4. 尚未实现摄像头断开重连和完整错误统计；
5. 尚未接入 LVGL；
6. 尚未实现停车业务、数据库和网络层。

## 8. 下一步计划

### 8.1 结果稳定与去重（待真机验证）

已实现同一车牌 3 次确认、最多容忍 2 帧漏检和同车牌 10 秒冷却。下一步在 RK3576 上验证：持续展示同一车牌只产生一次确认事件；短暂遮挡不应误触发；切换新车牌能产生新事件；同车牌移出后在冷却期内重新出现不重复确认。

### 8.2 性能与线程

结果确认成功后再实现：

```text
采集/转换线程
    -> 容量 1（最多 2）的最新帧有界队列
    -> 识别线程
    -> 最新识别结果
    -> UI/显示线程
```

识别跟不上时丢旧帧，绝不能无限积压。随后增加连续多帧确认、相同车牌冷却、阈值/错误统计和摄像头断开重连。

### 8.3 LVGL 与业务

正式 MIPI UI 使用 LVGL；`display.c` 保留为 fbdev 测试/诊断后端。LVGL DRM/KMS 工作后不要让两个显示后端同时运行。

再依次实现 SQLite、入出场状态机、车位、Gate 联动、计费、MQTT/JSON、后台、支付与 OTA。

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

多线程只使用容量 1～2 的最新帧队列。识别慢时丢旧帧，不能让延迟和内存持续增长。

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

多线程、LVGL、SQLite、MQTT、停车计费、后台、支付、OTA 尚未完成。单线程摄像头实时识别已完成真机验证；连续多帧确认和相同车牌去重已实现但尚待真机验证。

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

## 10. 新会话开场检查清单

用户新会话第一句话会是：

```text
先读HANDOFF.md
```

读完后：

1. 执行 `git status --short`，不要清理工作树；
2. 阅读 `CMakeLists.txt`、`tests/test_camera.c`、`tests/test_realtime_recognizer.c`、`include/image_convert.h`、`src/image/image_convert.c`、`include/display.h`、`src/display/display.c`；
3. 继续识别工作前再读 `include/plate_recognizer.h` 和 `tests/test_recognizer.cpp`；
4. 承认当前真机事实：Gate 开关成功、静态图片识别成功、USB 摄像头实时显示到 MIPI 成功、单线程实时车牌识别闭环成功；
5. 不要重新从摄像头/MIPI/PWM 是否存在开始排查，除非硬件变了；
6. 默认下一步是在 RK3576 真机验证连续多帧确认和相同车牌冷却去重，验证成功后再进入最新帧队列和多线程；
7. 修改前先向用户说明文件范围，避免擅自扩大修改。

# Parking 项目交接文档

> 最后更新：2026-08-30（Asia/Shanghai）
>
> 新会话第一步：完整阅读本文，然后执行 `git status --short`。
>
> 本文是当前状态的权威交接；`Parking.md` 是长期规划，不代表功能已经实现。

## 1. 我们正在做什么

项目目录：

```text
/home/dyj/project/Parking
```

这是一个基于 LubanCat RK3576 的嵌入式 Linux 智慧停车场项目，长期目标包括：

```text
V4L2 摄像头采集
    -> RK3576 NPU / RKNN 车牌识别
    -> 入场/出场业务状态机
    -> PWM 舵机道闸
    -> SQLite 停车记录与车位管理
    -> MQTT/JSON 与 Qt 后台通信
    -> 支付、显示、OTA 等
```

本阶段的具体任务是先完成并验证闸门模块：

```text
gate API
    -> Linux PWM sysfs 后端
    -> RK3576 交叉编译
    -> 开发板真实 PWM + 舵机开关闸测试
```

这一阶段已经完成最关键的真机闭环：`gate_create()`、`gate_open()`、`gate_close()` 已在 RK3576 上实际测试成功。

## 2. 当前结论速览

```text
RK3576 交叉编译                 已打通
RKNN 图片车牌识别               已在 RK3576 实机验证
V4L2 摄像头基础模块             已编码，尚需恢复测试和端到端接入
Gate 状态机                     已实现
Linux PWM sysfs 后端            已实现
RK3576 PWM + 舵机真实开关闸      已验证成功
摄像头实时识别                  尚未串联
停车业务、SQLite、MQTT、Qt      尚未实现
src/main.c 业务集成              尚未开始，当前仍是启动占位程序
```

当前没有卡在 PWM 驱动或 Gate 基础控制上。下一阶段应先收尾 Gate 测试程序的安全性和日志，再开始摄像头到识别器的端到端集成。

## 3. Gate 模块当前实现

相关文件：

```text
include/gate.h
include/pwm_sysfs.h
src/gate/gate.c
src/gate/pwm_sysfs.c
tests/test_gate.c
CMakeLists.txt
```

### 3.1 Gate 对外 API

```c
gate_t *gate_create(const gate_config_t *config);
int gate_open(gate_t *gate);
int gate_close(gate_t *gate);
int gate_update(gate_t *gate);
gate_state_t gate_get_state(const gate_t *gate);
const char *gate_last_error(const gate_t *gate);
void gate_destroy(gate_t *gate);
```

状态：

```text
GATE_STATE_CLOSED
GATE_STATE_OPENING
GATE_STATE_OPEN
GATE_STATE_CLOSING
GATE_STATE_ERROR
```

### 3.2 Gate 关键语义

- `gate_open()`、`gate_close()` 是非阻塞命令，只启动 PWM 动作。
- 调用者必须周期性调用 `gate_update()`。
- `gate_update()` 使用 `CLOCK_MONOTONIC` 和 `movement_time_ms` 推进状态。
- `movement_time_ms` 到期后，软件把 `OPENING/CLOSING` 改为 `OPEN/CLOSED`。
- 当前没有物理限位反馈，状态只是时间推测，不代表机械结构一定到位。
- `hold_after_move=0` 时完成动作后停止 PWM；为 `1` 时继续输出以保持舵机位置。
- `gate_create()` 会立即发起一次关闸，成功返回后的初始状态是 `CLOSING`。
- `gate_destroy()` 会停止 PWM、关闭 PWM 实例并释放内存。

### 3.3 当前已知 API 不一致

- 已关闭或正在关闭时，重复 `gate_close()` 返回 `GATE_OK`。
- 已打开或正在打开时，重复 `gate_open()` 返回 `GATE_ERROR_INVALID_STATE`。

暂未统一，不要在没有和用户确认语义的情况下随意修改。

### 3.4 当前真实硬件测试程序

当前 CMake 目标名是：

```text
test_gate
```

源码是：

```text
tests/test_gate.c
```

它链接 `parking_lib`，因此使用的是真实：

```text
src/gate/gate.c
src/gate/pwm_sysfs.c
src/log/log.c
```

没有链接 Mock PWM，也没有注册到 CTest，避免自动执行时驱动真实闸门。

当前测试程序提供交互命令：

```text
open
close
status
quit
```

当前测试配置硬编码在 `tests/test_gate.c`：

```text
pwmchip_path     = /sys/class/pwm/pwmchip2
channel          = 0
period_ns        = 20000000
open_pulse_ns    = 2000000
close_pulse_ns   = 1000000
movement_time_ms = 800
hold_after_move  = 0
```

这些参数已经用于本次真机成功测试，但以后更换舵机、PWM 引脚或板端设备树后必须重新确认，不能当成所有硬件的通用参数。

当前测试程序仍然使用 `printf()` 输出 `[INFO]/[ERROR]` 字样。虽然 `parking_lib` 已包含 `log.c`，但 `tests/test_gate.c` 尚未包含 `log.h` 或调用 `LOG_INFO/LOG_ERROR`。用户此前明确提出希望使用 `log.c`，这是下一步需要完成的小收尾。

## 4. RK3576 真机 PWM 验证结果

已确认的板端 PWM 路径：

```text
/sys/class/pwm/pwmchip2/pwm0
```

已确认：

```text
pwmchip2 存在
npwm = 1
channel = 0
pwm0 可以正常 export
设备树和 PWM 引脚配置正确
gate_create() 真机成功
gate_open() 真机成功
gate_close() 真机成功
```

PWM 刚 export 后，RK3576 上的真实状态是：

```text
enable      = 0
period      = 0
duty_cycle  = 0
polarity    = inversed
```

## 5. 本次最重要的根因与修复

### 5.1 原来的错误初始化顺序

原 `pwm_sysfs_configure()` 使用：

```text
enable = 0
duty_cycle = 0
polarity = normal
period = 20000000
duty_cycle = 1000000
```

问题出在第一步。RK3576 PWM 刚 export 时 `period == 0`，此时即使 `enable` 本来已经是 `0`，再次写入：

```bash
echo 0 > /sys/class/pwm/pwmchip2/pwm0/enable
```

Rockchip PWM 驱动仍返回：

```text
EINVAL
Invalid argument / 无效的参数
```

调用链因此变成：

```text
写 enable=0 失败
    -> pwm_sysfs_configure() 失败
    -> gate_create() 失败
```

### 5.2 当前真机验证通过的初始化顺序

当前 `pwm_sysfs_configure()` 使用：

```text
period = 20000000
duty_cycle = initial_duty
polarity = normal
```

即先建立合法 PWM state，不再对刚 export 且 `period=0` 的 PWM 重复写 `enable=0`。

真正开始输出时，再由 `pwm_sysfs_enable()` 执行：

```text
enable = 1
```

这个顺序已经通过 RK3576 真机验证。

### 5.3 绝不能回退的兼容约束

不要把 `pwm_sysfs_configure()` 恢复成无条件的：

```text
先 enable=0
再配置 period
```

除非未来实现了对当前 `enable/period/duty_cycle` 的读取，并明确区分：

1. 刚 export、`period=0`、已禁用的初始状态；
2. 已经配置但禁用的状态；
3. 已经配置且正在运行的状态。

如果以后需要支持接管一个预先存在或正在运行的 PWM 通道，应先读取真实 sysfs 状态再分支处理，不能用一个固定顺序覆盖所有情况。

## 6. 交叉编译状态与正确命令

工具链文件：

```text
cmake/toolchain-rk3576.cmake
```

使用 LubanCat SDK GCC 10.3 和对应 sysroot。不要使用宿主机的新版 aarch64 编译器。

本次验证成功的独立 Release 构建目录是：

```text
build-rk3576-release
```

正确命令：

```bash
cd /home/dyj/project/Parking

cmake -S . -B build-rk3576-release \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-rk3576.cmake" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-rk3576-release \
  --target test_gate \
  -j"$(nproc)"

file build-rk3576-release/test_gate
```

已验证产物：

```text
ELF 64-bit LSB executable, ARM aarch64
动态加载器 /lib/ld-linux-aarch64.so.1
动态依赖仅 libc.so.6
```

产物位置：

```text
build-rk3576-release/test_gate
```

部署示例：

```bash
scp build-rk3576-release/test_gate \
  root@172.20.10.14:/root/test/parking/test_gate
```

板端运行前必须再次确认源码里硬编码的 PWM 路径和舵机脉宽：

```bash
cd /root/test/parking
chmod +x test_gate
./test_gate
```

## 7. CMake 路径和缓存踩坑

错误命令曾写成：

```bash
-DCMAKE_TOOLCHAIN_FILE=toolchain-rk3576.cmake
```

项目根目录下没有这个文件，正确相对路径是：

```bash
-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rk3576.cmake
```

更稳妥的是使用绝对路径：

```bash
-DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-rk3576.cmake"
```

错误配置曾污染原 `build-rk3576` 缓存，并造成该构建目录里大量生成文件变化/删除。不要把它当成当前可靠构建目录。优先继续使用已验证的：

```text
build-rk3576-release
```

如果必须恢复 `build-rk3576`，先确认其中没有用户需要保留的产物，再使用 CMake 的 fresh 配置；不要直接对整个工作树执行 Git 恢复命令。

## 8. 当前 Git 工作树状态

工作树是 dirty 的，包含用户自己的源码修改和构建目录变化。至少包括：

```text
M  CMakeLists.txt
M  include/gate.h
M  src/gate/gate.c
M  src/gate/pwm_sysfs.c
?? tests/test_gate.c
?? build-rk3576-release/
```

此外，失败的 CMake 配置导致 `build-rk3576/` 下大量已跟踪生成文件显示修改或删除。

这些改动都应视为用户资产。新会话绝对不要执行：

```text
git reset --hard
git checkout -- .
git clean -fd
rm -rf build-rk3576
```

先执行：

```bash
git status --short
git diff -- CMakeLists.txt include/gate.h src/gate/gate.c src/gate/pwm_sysfs.c
```

只修改当前任务明确涉及的文件。

## 9. 当前还没有完成的事情

### 9.1 Gate 收尾

1. 把 `tests/test_gate.c` 的 `printf()` 日志迁移到 `log.h` 的 `LOG_INFO/LOG_WARN/LOG_ERROR`。
2. 给真实硬件测试增加启动前人工确认，因为 `gate_create()` 会立即关闸。
3. 增加 Ctrl+C/SIGTERM 的安全清理，确保退出时调用 `gate_destroy()`。
4. 给 `wait_gate_finished()` 增加软件超时，避免异常情况下无限等待。
5. 考虑把 PWM 路径、通道和脉宽改成命令行参数或配置文件，避免硬编码。
6. 如需自动单元测试，另建 Mock PWM 测试目标；不要把真实硬件测试加入 CTest。
7. 决定是否统一重复开闸和重复关闸命令的返回语义。
8. `gate_create()` 失败目前只返回 `NULL`，对象释放后调用者拿不到详细 PWM 错误；以后可考虑改进错误传递。

### 9.2 Gate 安全能力

当前没有：

```text
开到位限位开关
关到位限位开关
红外防砸
地感/车辆通过检测
电流或堵转检测
```

因此当前实现只适合原型验证，不能作为工业级安全判断。自动关闸条件应放在停车业务状态机中，不要写死到 Gate 底层模块。

### 9.3 摄像头与识别集成

`camera.c` 已实现 V4L2 mmap/poll/DQBUF/QBUF 基础流程，但尚未完成实时识别接入。

关键问题：

- 摄像头实际 FOURCC 必须在板端查询，不能假定。
- `tests/test_camera.c` 当前偏向 RGB565；识别器需要 BGR888。
- 根据实际格式实现 `RGB565/YUYV/MJPEG -> BGR888`。
- `CameraFrame.data` 指向 mmap 缓冲区，调用 `camera_release_frame()` 后不能继续使用。
- 若跨线程识别，必须先复制到自有缓冲区，再归还 V4L2 buffer。
- 推荐使用容量 1～2 的“最新帧”有界队列，避免识别跟不上时无限积压。

`CMakeLists.txt` 当前没有 `test_camera` 目标，需要恢复后再实机验证。

## 10. 其他已完成模块

### 10.1 日志模块

文件：

```text
include/log.h
src/log/log.c
```

支持：

```text
LOG_DEBUG
LOG_INFO
LOG_WARN
LOG_ERROR
日志级别
详细 file:line/function 信息
终端彩色输出
```

当前日志只输出到 `stderr`，没有文件日志初始化/关闭接口。需要保存时可由 shell 重定向或 `tee` 完成。

### 10.2 RKNN 车牌识别

图片识别已经在 RK3576 真机验证成功，曾识别出：

```text
川AA16052，绿色车牌
```

相关文件：

```text
src/recognizer/
include/plate_recognizer.h
tests/test_recognizer.cpp
models/yolo26s-plate-detect-rk3576.rknn
models/plate_rec_color-rk3576.rknn
lib/librknnrt.so
fonts/platech.ttf
```

`test_recognizer` 的 RPATH 使用 `$ORIGIN/lib`，板端应加载项目部署目录里的 `lib/librknnrt.so`，不要覆盖系统 `/lib/librknnrt.so`。

### 10.3 主程序

`src/main.c` 当前仍基本只有：

```c
puts("Parking system started.");
```

尚未集成 Gate、摄像头、识别或停车业务。

## 11. 已踩过的坑，绝对不要再踩

### 11.1 绝不能恢复 PWM 的错误初始化顺序

RK3576 刚 export、`period=0` 时，不要重复写 `enable=0`。这会返回 `EINVAL`。必须先建立合法 `period/duty_cycle`，再 enable。

### 11.2 不要把桌面 Linux 的“通用顺序”强行覆盖真机结论

本项目已经有 RK3576 实机证据。任何所谓“配置前总应先 disable”的重构，都必须兼容 `period=0` 的 Rockchip 初始状态，不能仅凭通用经验回退代码。

### 11.3 不要使用错误的 toolchain 路径

错误：

```text
toolchain-rk3576.cmake
```

正确：

```text
cmake/toolchain-rk3576.cmake
```

### 11.4 不要使用宿主 Ubuntu 的新版 aarch64 GCC

不要使用 `/usr/bin/aarch64-linux-gnu-gcc/g++`。它可能引入板端不存在的新版 glibc 符号。必须使用 LubanCat SDK GCC 10.3 + sysroot。

### 11.5 不要复用已污染的 CMake 缓存

工具链选择会缓存。路径错误或换工具链后，使用新的构建目录或明确执行 fresh 配置。当前可靠目录是 `build-rk3576-release`。

### 11.6 不要自动运行真实 Gate 测试

不要把 `test_gate` 注册到 CTest，也不要在 CI、开机脚本或普通全量测试中自动运行。`gate_create()` 会立即驱动关闸。

### 11.7 不要硬编码猜测其他板子的 pwmchip 编号

本板本次确认是 `pwmchip2/pwm0`，但 `pwmchipN` 会随设备树、风扇、背光和启用的控制器变化。更换环境后必须重新检查 `/sys/class/pwm` 和 `/sys/kernel/debug/pwm`。

### 11.8 不要用 GPIO/3.3V 给舵机供电

PWM 引脚只连接信号。舵机必须使用满足额定电压和堵转电流的独立稳压电源，并与开发板共地。

### 11.9 不要假设所有舵机都安全支持 1～2 ms

当前参数已在当前硬件上成功，但换舵机后必须依据手册和实测重新校准。第一次动作应解除机械负载，避免撞限位、堵转、过流和烧毁。

### 11.10 不要把软件状态当成物理限位

`movement_time_ms` 到期只表示软件认为动作完成，不代表舵机未卡住或闸杆真实到位。

### 11.11 不要用 `system("echo ...")` 控制 PWM

`pwm_sysfs.c` 已使用 `open/write/close` 并保存错误。不要退回 shell 拼接方式。

### 11.12 不要无条件 unexport 别人导出的 PWM

`exported_by_us` 用于记录所有权。`pwm_sysfs_close()` 只应 unexport 本实例导出的通道。

### 11.13 不要删除 `start_motion()` 中的 PWM enable

`pwm_sysfs_configure()` 只建立配置，不开始输出；真正动作依赖 `pwm_sysfs_enable()`。

### 11.14 不要破坏 dirty worktree

现有未提交源码和未跟踪测试属于用户。不要 reset、checkout、clean 或随意删除构建目录。

### 11.15 不要盲目替换板端 RKNN Runtime

识别程序应通过 `$ORIGIN/lib` 使用项目部署的 `librknnrt.so`。不要覆盖系统 `/lib/librknnrt.so`。

### 11.16 不要把 `Parking.md` 的规划写成已完成功能

SQLite、MQTT、Qt、计费、车位业务、多线程识别和 OTA 仍未实现。

## 12. 下一步推荐顺序

### 第一阶段：Gate 收尾

1. 先检查 dirty worktree，保护用户真机验证过的 `pwm_sysfs.c`。
2. 把 `tests/test_gate.c` 改用 `log.c`，保留交互命令。
3. 增加启动确认、信号清理和等待超时。
4. 把硬件参数改为命令行或配置文件输入。
5. 重新交叉编译 `test_gate` 并做一次板端回归。
6. 记录舵机具体型号、电源、40Pin 物理引脚和最终校准值；当前交接中这些信息仍不完整。

### 第二阶段：摄像头到识别器

1. 恢复 `test_camera` CMake 目标。
2. 在板端查询 USB 摄像头支持的 FOURCC。
3. 完成实际需要的像素格式到 BGR888 转换。
4. 先做单线程 `camera -> recognizer` 端到端测试。
5. 稳定后再做采集线程、最新帧有界队列和识别线程。

### 第三阶段：停车业务

在硬件输入输出和实时识别稳定之后，再实现：

```text
SQLite schema
车辆入场/出场状态机
车位状态
计费
MQTT/JSON
Qt 后台
显示与支付
OTA
```

## 13. 新会话开场检查清单

新会话中，用户只会说：

```text
先读HANDOFF.md
```

读完后应按顺序：

1. 执行 `git status --short`，不要修改或清理用户文件。
2. 阅读当前 `CMakeLists.txt`、`tests/test_gate.c`、`src/gate/gate.c` 和 `src/gate/pwm_sysfs.c`。
3. 明确承认真机状态：RK3576 的 `pwmchip2/pwm0` 和 Gate 开关闸已经验证成功。
4. 明确保留关键约束：刚 export 且 `period=0` 时不能先写 `enable=0`。
5. 若用户没有指定新任务，建议从“Gate 测试改用 log.c + 安全收尾”继续。
6. 不要重新从“PWM 是否存在、设备树是否启用”开始排查，除非硬件环境发生变化。

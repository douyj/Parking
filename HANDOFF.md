# Parking 项目交接文档

## 1. 项目目标

当前在做一个基于 RK3576（LubanCat）的嵌入式 Linux 智慧停车场管理系统。总体规划见 `Parking.md`。

最终目标不只是车牌识别，而是完成：

```text
V4L2 摄像头采集
    ↓
RK3576 NPU / RKNN 车牌识别
    ↓
入场/出场业务逻辑
    ↓
SQLite 车辆、车位和停车记录
    ↓
MQTT + JSON 与 Qt 后台通信
    ↓
PWM 舵机道闸、GPIO 传感器、LVGL、支付、OTA 等
```

## 2. 当前工程与环境

- 工程目录：`/home/dyj/project/Parking`
- 开发板 IP：`172.20.10.14`
- 开发板部署目录：`/root/test/parking`
- 交叉构建目录：`build-rk3576`
- 交叉编译器：LubanCat SDK 自带 GCC 10.3
- OpenCV：RKNN Toolkit 中的 aarch64 OpenCV 3.4.5，静态库
- RKNN Runtime：项目的 `lib/librknnrt.so`
- RKNN 模型：
  - `models/yolo26s-plate-detect-rk3576.rknn`
  - `models/plate_rec_color-rk3576.rknn`

## 3. 已完成的工作

### 3.1 RK3576 交叉编译已打通

`cmake/toolchain-rk3576.cmake` 已切换到 LubanCat SDK GCC 10.3 工具链及其 sysroot。

正确构建命令：

```bash
cd /home/dyj/project/Parking

cmake --fresh -S . -B build-rk3576 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rk3576.cmake \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-rk3576 -j4
```

新产物的 glibc 符号需求最高为 `GLIBC_2.29`，已在板端成功运行。

### 3.2 V4L2 摄像头基础模块已编码

相关文件：

- `include/camera.h`
- `src/camera/camera.c`
- `tests/test_camera.c`

已实现的 API：

- `camera_open`
- `camera_start`
- `camera_get_frame`
- `camera_release_frame`
- `camera_stop`
- `camera_close`

内部包含 V4L2 capability/format/buffer 配置、mmap、poll、DQBUF/QBUF 及资源释放。

`tests/test_camera.c` 中有 RGB565 摄像头帧写入 framebuffer 的测试源码，但当前 `CMakeLists.txt` **没有定义 `test_camera` 目标**，下一步需要补回并重新实机验证。

### 3.3 日志模块已有基础实现

相关文件：

- `include/log.h`
- `src/log/log.c`

支持日志级别、彩色输出和详细信息开关。

### 3.4 RKNN 车牌识别已完成并实机验证

相关文件：

- `include/plate_recognizer.h`
- `include/rknn_api.h`
- `src/recognizer/plate_recognizer.cpp`
- `tests/test_recognizer.cpp`
- `third_party/stb_image.h`
- `third_party/stb_image_write.h`
- `third_party/stb_truetype.h`

已实现：

- RKNN 车牌检测模型加载和 NPU 推理
- 车牌框及四个角点解析
- 透视矫正
- 单层/双层车牌处理
- 车牌字符识别
- 车牌颜色识别
- 中文字体绘制、边框和置信度标注
- 标注结果 JPEG 保存

开发板已成功执行：

```bash
cd /root/test/parking
./test_recognizer tsl.jpg
```

已验证的实际输出：

```text
检测到 1 个车牌
[0] 车牌=川AA16052, 颜色=绿色, 检测置信度=0.92, 颜色置信度=0.44
结果已保存到 recognition_result.jpg
```

### 3.5 RKNN 部署库路径已修正

`test_recognizer` 的 RPATH 已包含：

```text
$ORIGIN/lib
```

板端已确认加载：

```text
librknnrt.so => /root/test/parking/./lib/librknnrt.so
```

而不是系统的 `/lib/librknnrt.so`。

### 3.6 VS Code 红色波浪线配置已补充

`.vscode/settings.json` 已让 Microsoft C/C++ 和 clangd 读取：

```text
build-rk3576/compile_commands.json
```

其中已包含 aarch64 OpenCV 头文件和 LubanCat 交叉编译器路径。

如仍显示旧诊断，在 VS Code 执行：

```text
Developer: Reload Window
C/C++: Reset IntelliSense Database
```

### 3.7 舵机闸门 Gate API 与 Linux PWM sysfs 后端已实现

用户在等待 USB 摄像头到货期间，先实现了供后续停车业务调用的舵机闸门模块。

相关文件：

- `include/gate.h`
- `include/pwm_sysfs.h`
- `src/gate/gate.c`
- `src/gate/pwm_sysfs.c`
- `CMakeLists.txt`

`gate.h` 当前采用精简状态：

```text
GATE_STATE_CLOSED
GATE_STATE_OPENING
GATE_STATE_OPEN
GATE_STATE_CLOSING
GATE_STATE_ERROR
```

对业务层提供的 API：

- `gate_create`
- `gate_open`
- `gate_close`
- `gate_update`
- `gate_get_state`
- `gate_last_error`
- `gate_destroy`

关键语义：

- `gate_open()` 和 `gate_close()` 是非阻塞命令，不在内部 `sleep()`；
- 业务主循环需要周期性调用 `gate_update()`；
- `gate_update()` 使用 `CLOCK_MONOTONIC` 和 `movement_time_ms` 将 `OPENING/CLOSING` 更新为 `OPEN/CLOSED`；
- 当前没有限位开关，因此最终状态只是按动作时间推测，并非传感器确认；
- `hold_after_move=0` 时到位后停止 PWM，`1` 时继续输出以保持舵机位置；
- `gate_create()` 当前会立即发出一次关闸动作，返回后的初始状态为 `CLOSING`。

`pwm_sysfs` 后端已实现：

- 检查 `pwmchip` 目录和 `npwm` 通道范围；
- 通道不存在时写 `export` 并等待 `pwmN` 目录生成；
- 配置前禁用 PWM、清零旧 duty、设置 `normal` 极性、`period` 和初始 `duty_cycle`；
- 独立的 `set_duty_cycle`、`enable`、`disable`；
- 保存详细错误信息；
- 只对本实例自己导出的通道执行 `unexport`，预先已导出的通道不会被擅自取消导出。

为了让 gate 与新后端正确互通，还修正了 `gate.c`：

- `_POSIX_C_SOURCE` 已移到系统头文件之前；
- `start_motion()` 在设置脉宽后会调用 `pwm_sysfs_enable()`；
- `gate_create()` 不再因为初始状态已是 `CLOSED` 而跳过首次关闸动作。

验证结果：

- 严格 C99 `-Wall -Wextra -Wpedantic -Werror` 编译检查通过；
- 用 `/tmp` 下的伪 sysfs 目录验证了配置、启用、修改脉宽、禁用和关闭；
- 用伪 sysfs 验证了 `gate_create -> 关闸完成 -> 开闸完成` 状态流；
- RK3576 交叉构建通过，`parking` 和 `test_recognizer` 均成功生成。

注意：上述 PWM 测试只是接口级模拟，尚未在 RK3576 真实 PWM 引脚和真实舵机上验证。

## 4. 当前真实进度

不要将 `Parking.md` 中的规划当成已实现功能。当前状态是：

```text
V4L2 摄像头基础能力    已编码，需恢复 CMake 测试目标并复测
RKNN 图片车牌识别      已完成、已在 RK3576 实机验证
摄像头 + 实时识别     尚未串联
Gate 业务 API          已实现基础版本
PWM sysfs 后端         已实现并通过模拟测试，尚未板端实测
舵机开关闸             尚未接线、供电和校准
停车业务系统           尚未开始实现
```

`src/main.c` 目前只有：

```c
puts("Parking system started.");
```

它尚未调用摄像头或识别模块。

## 5. 当前卡点

当前不是卡在 RKNN 模型，模型已证明可用。当前的主要集成点是：

### 5.1 摄像头帧格式和识别输入不一致

`tests/test_camera.c` 当前按 RGB565 采集和显示；`plate_recognizer_process()` 要求 BGR888（每像素 3 字节）。

所以不能直接将 V4L2 mmap 缓冲区传给识别器。需要增加转换层：

```text
RGB565 -> BGR888
```

如果改用摄像头 MJPEG，则是：

```text
MJPEG -> JPEG 解码 -> BGR888
```

需根据开发板实际摄像头支持格式决定，不要盲目假定。

### 5.2 mmap 帧生命周期

`CameraFrame.data` 指向 V4L2 mmap 缓冲区，用完必须调用 `camera_release_frame()` 归还 QBUF。

如果要将帧交给另一个识别线程，不能归还缓冲区后继续使用原指针。应该：

1. 将需要识别的帧复制到自有缓冲区；
2. 立即 `camera_release_frame()`；
3. 将自有缓冲区送入有界队列。

### 5.3 Gate 当前卡在真实硬件验证，不是卡在软件编译

PWM/gate 软件已经能够编译和通过伪 sysfs 测试，但真实板端还需要确认：

1. 用户使用的准确 LubanCat RK3576 板型；
2. 选择哪个 40Pin PWM 引脚；
3. 通过 `fire-config` 或 `/boot/uEnv/` 设备树插件启用对应 PWM 并重启；
4. 板端实际分配的 `/sys/class/pwm/pwmchipN`；
5. 舵机具体型号、额定电压、堵转电流和允许脉宽范围；
6. 开闸与关闸的实际校准脉宽；
7. `hold_after_move` 是否需要保持为 1。

LubanCat RK3576 官方文档：

```text
https://doc.embedfire.com/linux/rk3576/quick_start/zh/latest/doc/40pin/pwm/pwm.html
```

文档中 LubanCat-3 系列 40Pin PWM 引脚为：

```text
Pin 12: PWM2_CH6_M3（与风扇 PWM 冲突，优先不要用）
Pin 32: PWM2_CH7_M3
Pin 33: PWM1_CH0_M3
Pin 35: PWM2_CH3_M3
```

不要把文档示例中的 `pwmchip1` 当作固定编号。启用的 PWM/背光/风扇不同，内核分配的编号会改变，必须在目标板查询。

### 5.4 当前 gate 状态并不等于物理限位状态

当前只有舵机 PWM 输出，没有限位开关、红外防砸或地感输入。因此：

```text
movement_time_ms 到时 -> 软件认为 OPEN/CLOSED
```

这只适合原型验证。以后真实业务应增加车辆通过检测、限位输入和防砸逻辑，自动关闸条件属于停车业务状态机，不应写死在 `gate` 模块里。

## 6. 下一步计划

用户当前选择在 USB 摄像头到货前先完成舵机闸门模块。当前最近的里程碑是：

> 在 RK3576 上启用一个不冲突的硬件 PWM，用独立电源驱动舵机，完成安全的小范围脉宽测试，并通过 `gate_open/gate_close` 实际控制闸门。

推荐按如下顺序：

1. 新会话先检查 `git status --short` 和当前 `gate.h/gate.c/pwm_sysfs.h/pwm_sysfs.c`，不要覆盖用户代码。
2. 决定是否统一重复命令语义：当前 `gate_close()` 对重复关闭返回 `GATE_OK`，而 `gate_open()` 对已打开/正在打开返回 `GATE_ERROR_INVALID_STATE`，两者不一致。
3. 新增 `tests/test_gate.c` 和 CMake 测试目标，提供命令行开闸/关闸测试；不要直接把真实测试写进 `src/main.c`。
4. 在板端通过 `fire-config` 启用 Pin 32、33 或 35 对应的 PWM，重启后执行：

   ```bash
   ls -l /sys/class/pwm/
   cat /sys/kernel/debug/pwm
   ```

5. 根据 platform 设备地址和官方文档确认实际 `pwmchipN`，再填写 `gate_config_t.pwmchip_path`。
6. 舵机使用独立稳压电源，先断开机械闸杆，从中位脉宽开始做小范围测试，再逐步校准 `open_pulse_ns` 和 `close_pulse_ns`。
7. 验证 `gate_create()` 的首次关闸动作、`gate_update()` 状态变化、`hold_after_move=0/1` 以及程序退出时 PWM 释放行为。
8. 舵机链路稳定后，可增加 mock gate 测试和板端 `test_gate`，再把 Gate API 接入未来停车业务状态机。

USB 摄像头到货后再恢复原摄像头里程碑：

1. 在 `CMakeLists.txt` 恢复 `test_camera` 目标；
2. 查询摄像头实际支持的 FOURCC；
3. 实现 `RGB565/YUYV/MJPEG -> BGR888` 中实际需要的转换；
4. 新增 `test_camera_recognizer`，每秒抽取一帧做单线程端到端识别；
5. 稳定后再做摄像头线程 + 容量 1～2 的有界最新帧队列 + 识别线程；
6. 然后才实现 SQLite、入出场状态机、计费、MQTT、Qt 和 OTA。

## 7. 开发板测试部署文件

车牌图片测试所需的完整目录结构：

```text
/root/test/parking/
├── test_recognizer
├── tsl.jpg
├── lib/
│   └── librknnrt.so
├── models/
│   ├── yolo26s-plate-detect-rk3576.rknn
│   └── plate_rec_color-rk3576.rknn
└── fonts/
    └── platech.ttf
```

从 Ubuntu 部署：

```bash
cd /home/dyj/project/Parking

ssh root@172.20.10.14 \
  'mkdir -p /root/test/parking/lib /root/test/parking/models /root/test/parking/fonts'

scp build-rk3576/test_recognizer \
  root@172.20.10.14:/root/test/parking/

scp lib/librknnrt.so \
  root@172.20.10.14:/root/test/parking/lib/

scp models/yolo26s-plate-detect-rk3576.rknn \
  models/plate_rec_color-rk3576.rknn \
  root@172.20.10.14:/root/test/parking/models/

scp fonts/platech.ttf \
  root@172.20.10.14:/root/test/parking/fonts/

scp tsl.jpg root@172.20.10.14:/root/test/parking/
```

## 8. 已踩过的坑，绝对不要再踩

### 8.1 不要用宿主 Ubuntu GCC 15 的 aarch64 编译器

错误用法：

```text
/usr/bin/aarch64-linux-gnu-gcc
/usr/bin/aarch64-linux-gnu-g++
```

这会链接宿主 Ubuntu glibc 2.43，导致板端报：

```text
GLIBC_2.38 not found
GLIBC_2.43 not found
```

必须使用 `cmake/toolchain-rk3576.cmake` 中的 LubanCat SDK GCC 10.3 + sysroot。

### 8.2 修改 toolchain 后必须清理 CMake 旧缓存

CMake 编译器选择会持久化在构建目录。修改 toolchain 后必须用：

```bash
cmake --fresh -S . -B build-rk3576 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rk3576.cmake \
  -DCMAKE_BUILD_TYPE=Release
```

否则可能继续使用旧的 `/usr/bin/cc` 或旧配置。

### 8.3 toolchain 文件曾经是空文件

当时 CMake 虽然记录了 `CMAKE_TOOLCHAIN_FILE`，但编译器仍是 `/usr/bin/cc`。原因是 `cmake/toolchain-rk3576.cmake` 内容为空。遇到类似问题先检查文件内容和 `CMakeCache.txt`，不要只看命令行参数。

### 8.4 不要让板端默认加载 `/lib/librknnrt.so`

曾经 `ldd` 显示：

```text
librknnrt.so => /lib/librknnrt.so
```

当时程序发生段错误。强制换库测试时还曾出现进程卡住、`Ctrl+C` 不能退出的情况，可能是 NPU 驱动调用进入不可中断状态。

现在必须保持部署：

```text
test_recognizer
lib/librknnrt.so
```

并用下面命令确认：

```bash
ldd ./test_recognizer | grep rknn
```

应指向 `/root/test/parking/lib/librknnrt.so`。不要盲目覆盖板端 `/lib/librknnrt.so`，以免破坏系统其他 RKNN 程序。

### 8.5 不要假设测试程序接收模型参数

当前 `test_recognizer` 用法是：

```bash
./test_recognizer <测试图片> [输出图片]
```

模型和字体路径是程序内置的：

```text
models/yolo26s-plate-detect-rk3576.rknn
models/plate_rec_color-rk3576.rknn
fonts/platech.ttf
```

之前错误地用了“两个模型 + 图片”三个参数，程序只会打印用法。

### 8.6 不要再用 Parking 测试入口的 OpenCV imgcodecs 版本

最初 Parking 的 `test_recognizer` 用 `cv::imread/cv::imwrite`，而原先已经在板端验证的 `/home/dyj/test_projects/plate_rknn` 使用 stb 读写图片。

已对比确认两个工程的：

- `plate_recognizer.cpp`
- `rknn_api.h`
- 两个 `.rknn` 模型
- `librknnrt.so`

内容/哈希完全一致。实际差异之一是测试入口。现在 `tests/test_recognizer.cpp` 已改回 stb 读写，并已在板端成功运行。不要无理由改回 `imgcodecs`。

### 8.7 不要宣称未实现的 Parking.md 功能

`Parking.md` 是规划文档，不是已完成清单。当前只新增了 Gate API 和 PWM sysfs 基础后端；真实舵机控制尚未板端验证。SQLite、MQTT、Qt、计费、车位业务、识别多线程和 OTA 仍未实现。

### 8.8 不要破坏用户已有的 dirty worktree

当前 Git 工作树有较多用户改动、删除项和未跟踪文件，包括旧 `build/` 产物删除、`Parking.md`、模型、库、字体和识别源码等。

不要执行：

```text
git reset --hard
git checkout -- .
```

也不要随意删除未跟踪文件。修改前先用 `git status --short` 了解状态，只处理当前任务相关文件。

### 8.9 不要用 `system("echo ...")` 控制 PWM

当前 `pwm_sysfs.c` 已使用 `open/write/close` 操作 sysfs，并记录具体错误。不要改成拼接 shell 命令，否则会引入命令注入、转义、返回值和错误处理问题。

### 8.10 不要硬编码 `pwmchip1`

`pwmchipN` 是内核按已启用 PWM 控制器动态分配的编号。风扇、屏幕背光和设备树插件都会影响编号。必须在板端通过 `/sys/class/pwm`、`/sys/kernel/debug/pwm` 和 platform 设备地址确认映射。

### 8.11 不要用 RK3576 GPIO/3.3V 给舵机供电

PWM 引脚只连接舵机信号线。舵机必须使用符合其额定电压和堵转电流的独立稳压电源，并把舵机电源 GND 与 RK3576 GND 共地。两个独立电源的正极不要随意并联。

常见小型舵机原型可从独立 `5V/2A` 稳压电源起步，但最终必须以具体舵机数据手册和堵转电流为准。大扭矩舵机可能需要明显更大的电流。

### 8.12 不要假设所有舵机都是固定 1～2 ms

`period_ns=20000000`、`1.0ms/1.5ms/2.0ms` 只是常见参考，不是所有舵机的保证范围。第一次上电应断开闸杆或解除机械负载，从中位附近开始小范围调整，避免撞限位、堵转、过流和烧毁舵机。

### 8.13 `pwm_sysfs_configure()` 不会自动使能 PWM

这是有意的接口设计：

```text
pwm_sysfs_configure() -> 配置完成但 enable=0
pwm_sysfs_enable()    -> 真正开始输出
```

`gate.c:start_motion()` 当前已调用 `pwm_sysfs_enable()`，不要再删除，否则 `gate_open/gate_close` 只会改 duty 而不会产生输出。

### 8.14 不要随意 unexport 预先存在的 PWM 通道

当前 `pwm_sysfs_t.exported_by_us` 会记录通道是否由本实例导出。`pwm_sysfs_close()` 只取消导出自己创建的通道。不要改成无条件 `unexport`，否则可能破坏其他程序或系统设备使用的 PWM。

### 8.15 不要把时间推测状态当成真实安全反馈

当前 gate 没有限位开关和防砸传感器。`OPEN/CLOSED` 只是动作时间到期后的软件状态，不能用于工业级安全判断。舵机停止 PWM 后也可能失去保持力，是否使用 `hold_after_move=0` 必须结合机械结构实测。

### 8.16 `gate_create()` 当前会立即驱动关闸

这与 `gate.h` 注释一致，但实机第一次运行时可能造成突然动作。接舵机测试前先解除闸杆机械负载、确认中位和关闸脉宽安全，再执行测试程序。

## 9. 新会话开始时的建议操作

1. 完整读取本文档和 `Parking.md`。
2. 执行 `git status --short`，保护用户现有改动。
3. 检查 `include/gate.h`、`include/pwm_sysfs.h`、`src/gate/gate.c`、`src/gate/pwm_sysfs.c` 和 `CMakeLists.txt`。
4. 先确认用户是否已经拿到舵机、独立电源以及准确型号；没有硬件时先补 `test_gate`/mock 测试，不要宣称真实 PWM 已验证。
5. 有硬件时从“启用设备树 PWM -> 确认实际 pwmchip -> 断开机械负载校准舵机”开始。
6. 摄像头到货后再继续“摄像头单帧 -> BGR888 -> RKNN 识别”。
7. 不要提前跳到 Qt、MQTT 或 OTA。

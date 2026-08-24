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
GPIO 道闸、LVGL、支付、OTA 等
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

## 4. 当前真实进度

不要将 `Parking.md` 中的规划当成已实现功能。当前状态是：

```text
V4L2 摄像头基础能力    已编码，需恢复 CMake 测试目标并复测
RKNN 图片车牌识别      已完成、已在 RK3576 实机验证
摄像头 + 实时识别     尚未串联
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

## 6. 下一步计划

下一个里程碑应该是：

> RK3576 从 USB 摄像头采集画面，每秒选取一帧转换为 BGR888，调用 NPU 识别车牌，在终端输出结果并保存标注抓拍图片。

推荐按如下顺序：

1. 在 `CMakeLists.txt` 恢复 `test_camera` 目标。
2. 在开发板查询摄像头实际支持的 FOURCC，复测 V4L2 采集。
3. 实现独立的帧格式转换层，例如 `frame_converter.cpp`。
4. 新增 `test_camera_recognizer`，先做单线程端到端闭环。
5. 测量采集、转换、检测和识别耗时。
6. 闭环稳定后，再改为摄像头线程 + 有界队列 + 识别线程。
7. 然后实现 SQLite：车辆表、停车记录表、车位表。
8. 再实现入场/出场状态机、计费和剩余车位。
9. 业务闭环完成后，再接 MQTT + JSON、Qt 后台、GPIO 道闸和 OTA。

不要现在就跳去做 Qt、MQTT 或 OTA；先把摄像头到 NPU 识别的最小端到端链路打通。

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

`Parking.md` 是规划文档，不是已完成清单。SQLite、MQTT、Qt、GPIO、计费、车位、多线程和 OTA 目前均未实现。

### 8.8 不要破坏用户已有的 dirty worktree

当前 Git 工作树有较多用户改动、删除项和未跟踪文件，包括旧 `build/` 产物删除、`Parking.md`、模型、库、字体和识别源码等。

不要执行：

```text
git reset --hard
git checkout -- .
```

也不要随意删除未跟踪文件。修改前先用 `git status --short` 了解状态，只处理当前任务相关文件。

## 9. 新会话开始时的建议操作

1. 完整读取本文档和 `Parking.md`。
2. 执行 `git status --short`，保护用户现有改动。
3. 检查 `CMakeLists.txt`、`camera.h/camera.c` 和 `test_camera.c`。
4. 从“恢复 `test_camera` 构建目标 + 板端确认摄像头 FOURCC”开始。
5. 以“摄像头单帧 -> BGR888 -> RKNN 识别”为下一个实现目标。

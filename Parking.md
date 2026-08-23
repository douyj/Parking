# 基于 RK3576 的智慧停车场管理系统

## 1. 项目定位

**项目名称：** 基于 RK3576 的智慧停车场管理系统  
**项目类型：** 嵌入式 Linux 应用层综合项目  
**目标岗位：** 嵌入式 Linux 应用开发工程师

本项目以 RK3576 为核心控制器，模拟真实地下停车场业务场景，实现车辆入场/出场识别、剩余车位统计、车位状态管理、停车计费、月卡车辆管理、道闸控制、Qt 后台管理以及应用级 OTA 升级等功能。

项目重点不是单纯“识别车牌”，而是把 V4L2、Linux 多线程/IPC、MQTT、SQLite、HTTP、Qt、LVGL、GPIO、RS485、OTA 等知识组合成完整的嵌入式 Linux 工程系统。

---

## 2. 系统总体架构

```text
                         智慧停车场管理系统

                ┌─────────────────────────┐
                │       Qt 管理后台        │
                │                         │
                │  车辆管理 / 车位管理    │
                │  停车记录 / 设备状态    │
                │  月卡 / 支付 / OTA       │
                └────────────┬────────────┘
                             │
                           MQTT
                             │
                   ┌─────────▼─────────┐
                   │   MQTT Broker     │
                   │ Mosquitto / EMQX  │
                   └─────────┬─────────┘
                             │
                           MQTT
                             │
                             ▼
┌──────────────────────────────────────────────────────────┐
│                        RK3576                            │
│                                                          │
│  V4L2       OpenCV/车牌识别      SQLite      LVGL        │
│   │               │                │           │          │
│ 摄像头采集      车辆识别         业务数据      本地界面   │
│                                                          │
│  pthread / IPC / MQTT / HTTP / OTA / GPIO / RS485       │
└───────────────┬───────────────────────────────┬──────────┘
                │                               │
              GPIO                            RS485
                │                               │
             道闸/蜂鸣器                   STM32车位节点
                                                │
                                      红外/超声波车位传感器
```

---

### 2.1 模块解耦、接口抽象与可替换后端

为了便于后续从 **RK3576** 迁移到 **RK3588 / 后续更高性能 Rockchip 平台**，项目在应用层采用“**业务逻辑与具体硬件实现解耦**”的设计。业务层只依赖统一接口，不直接依赖 V4L2、GPIO、某一种车牌识别库或某一种网络实现。

```text
                     ┌──────────────────────┐
                     │    Parking Service   │
                     │    停车业务逻辑层     │
                     └──────────┬───────────┘
                                │
                  只调用统一抽象接口 / API
                                │
        ┌───────────────┬───────┼───────────────┬───────────────┐
        ▼               ▼       ▼               ▼               ▼
   Camera API       Plate API  Gate API      Network API      Storage API
        │               │       │               │               │
   ┌────┴────┐      ┌───┴────┐  │         ┌─────┴─────┐         │
   │         │      │        │  │         │           │         │
V4L2 USB   MIPI CSI  CPU识别  NPU识别    MQTT       TCP      SQLite
RK3576    RK3576    OpenCV   RKNN      Broker      JSON
```

推荐将工程拆分为：

```text
include/
├── camera.h
├── plate_recognizer.h
├── gate.h
├── network_client.h
└── storage.h

src/
├── app/
│   └── parking_service.c/.cpp       # 业务逻辑，不关心底层平台
│
├── camera/
│   ├── camera_v4l2.c               # RK3576 / USB 摄像头通用实现
│   └── camera_rk3576.c             # RK3576 MIPI CSI / RKMPP 实现
│
├── recognition/
│   ├── plate_cpu.cpp               # OpenCV / CPU 后端
│   └── plate_rknn.cpp              # RK3576 NPU / RKNN 后端
│
├── network/
│   ├── mqtt_client.c
│   └── tcp_client.c                # 可保留为备用后端
│
└── platform/
    └── rk3576/
        └── gpio_gate.c
```

统一接口示例：

```c
typedef struct {
    int  (*open)(const char *device);
    int  (*capture)(unsigned char **data, int *len);
    void (*close)(void);
} CameraOps;

typedef struct {
    int (*open_gate)(void);
    int (*close_gate)(void);
} GateOps;
```

业务代码只做：

```c
camera->capture(&frame, &len);
recognizer->recognize(frame, len, &result);
storage->save_record(&record);
network->publish_event(&event);
gate->open_gate();
```

这样在 RK3576 上既可以使用 **V4L2 + CPU/OpenCV**，也可以直接采用 **MIPI CSI / RKMPP / RKNN NPU** 等硬件加速实现；后续若更换 RK3588 或其他 Rockchip 平台，可继续复用统一接口与业务层，而停车计费、车辆入场/出场、SQLite、MQTT、Qt 后台协议等上层业务基本不需要重写。

这种设计重点体现：

- **模块解耦**：采集、识别、业务、网络、数据库、设备控制彼此独立。
- **接口抽象**：业务层面向 Camera / Recognizer / Gate / Network 等统一接口编程。
- **可替换后端**：同一个接口可以挂接 RK3576 的 USB/V4L2、MIPI/RKMPP、RKNN NPU 实现，或 Mock 测试实现。
- **平台迁移能力**：更换 SoC 时优先替换 HAL/Adapter 层，而不是重写整个项目。
- **便于测试**：没有真实摄像头、道闸或开发板时，也可以用 camera_mock、gate_mock 在 PC 上验证业务逻辑。

---

## 3. 核心功能

### 3.1 车辆入场

业务流程：

```text
车辆到达入口
    ↓
红外/车辆到达信号
    ↓
V4L2 摄像头抓拍
    ↓
OpenCV / 车牌识别
    ↓
查询 SQLite
    ↓
判断：
    ├── 月卡车辆 → 直接放行
    └── 普通车辆 → 创建停车记录
    ↓
检查剩余车位
    ↓
道闸打开
    ↓
车辆进入
    ↓
更新剩余车位
    ↓
上传 Qt 后台
```

---

### 3.2 车辆出场

```text
车辆到达出口
    ↓
V4L2 抓拍
    ↓
识别车牌
    ↓
查询入场记录
    ↓
计算停车时长
    ↓
计算停车费用
    ↓
模拟支付
    ↓
支付成功
    ↓
更新出场时间
    ↓
释放车位
    ↓
抬杆放行
```

---

## 4. 车位管理

### 4.1 剩余车位统计

系统维护：

```text
总车位：100
已占用：73
剩余：27
```

计算逻辑：

```text
free_spaces = total_spaces - occupied_spaces
```

LVGL 本地界面可显示：

```text
┌────────────────────────────┐
│      智慧停车管理系统       │
├────────────────────────────┤
│ 总车位：       100         │
│ 已使用：        73         │
│ 剩余车位：      27         │
│                            │
│ 最近车辆：粤A·12345        │
│ 入场时间：15:32            │
└────────────────────────────┘
```

---

### 4.2 具体车位状态

不仅统计“还有多少个空位”，还可以显示具体哪个车位空闲。

示例：

```text
A区

A01   occupied   粤A12345
A02   free
A03   free
A04   occupied   粤B88888
```

数据库表：

```sql
CREATE TABLE parking_space (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    space_no TEXT UNIQUE NOT NULL,
    area TEXT,
    status INTEGER DEFAULT 0,
    plate_number TEXT
);
```

其中：

```text
status = 0：空闲
status = 1：占用
```

---

## 5. STM32 + RS485 车位节点（进阶功能）

为了提高项目工程含量，可以使用 STM32 作为车位检测节点。

```text
                       RK3576
                          │
                        RS485
                          │
             ┌────────────┴────────────┐
             │                         │
           STM32                     STM32
             │                         │
       ┌─────┼─────┐             ┌────┼────┐
      A01   A02   A03            B01  B02  B03
```

STM32 负责：

- 红外/超声波车位传感器采集
- GPIO
- 定时器
- 简单状态判断

RK3576 负责：

- Linux 应用层
- 串口 / RS485
- SQLite
- MQTT 网络通信
- UI
- 停车业务逻辑

通信协议可以选择：

- 自定义串口协议
- Modbus RTU

该设计能够体现 **MCU + MPU 异构系统设计**。

---

## 6. 摄像头模块

基于 Linux V4L2 实现 USB 摄像头采集。

推荐掌握完整流程：

```text
open("/dev/videoX")
    ↓
VIDIOC_QUERYCAP
    ↓
VIDIOC_S_FMT
    ↓
VIDIOC_REQBUFS
    ↓
VIDIOC_QUERYBUF
    ↓
mmap()
    ↓
VIDIOC_QBUF
    ↓
VIDIOC_STREAMON
    ↓
select()/poll()
    ↓
VIDIOC_DQBUF
    ↓
处理当前帧
    ↓
VIDIOC_QBUF
```

推荐格式：

```text
JPEG / MJPEG
640 × 480
```

相比直接处理高分辨率 YUYV，可以减少内存带宽和 CPU 压力。

摄像头模块应通过统一 `CameraOps` / `CameraBackend` 接口暴露给业务层。RK3576 可根据摄像头类型使用 `camera_v4l2` 或新增 `camera_rk3576`（MIPI CSI / RKMPP）后端，而不修改停车业务代码。

---

## 7. 车牌识别模块

可以使用：

- OpenCV
- HyperLPR
- ONNX Runtime（可选）
- 自己封装车牌识别接口

建议设计统一接口：

```cpp
struct PlateResult {
    std::string plate;
    float confidence;
};

PlateResult recognize_plate(const cv::Mat &image);
```

识别模块不要和 V4L2 采集代码强耦合。建议进一步抽象为 `PlateRecognizer` 接口：RK3576 阶段既可使用 OpenCV / CPU 实现，也可直接切换为 RKNN NPU 推理后端，上层业务流程保持不变。

推荐架构：

```text
V4L2线程
   ↓
帧队列
   ↓
识别线程
   ↓
业务线程
```

---

## 8. SQLite 数据库设计

### 8.1 车辆表

```sql
CREATE TABLE vehicle (
    plate TEXT PRIMARY KEY,
    type INTEGER,
    owner TEXT,
    create_time TEXT
);
```

车辆类型：

```text
0：普通车辆
1：月卡车辆
```

---

### 8.2 停车记录表

```sql
CREATE TABLE parking_record (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    plate TEXT NOT NULL,
    entry_time TEXT NOT NULL,
    exit_time TEXT,
    fee REAL DEFAULT 0,
    status INTEGER DEFAULT 0
);
```

状态：

```text
0：停车中
1：已离场
```

---

### 8.3 车位表

```sql
CREATE TABLE parking_space (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    space_no TEXT UNIQUE,
    area TEXT,
    status INTEGER,
    plate_number TEXT
);
```

---

## 9. 月卡车辆

车辆可以分为：

```text
NORMAL
VIP
BLACKLIST
```

业务逻辑：

```text
普通车辆
    ↓
停车计费

月卡车辆
    ↓
验证有效期
    ↓
自动放行
```

---

## 10. MQTT 通信

RK3576 与 Qt 后台通过 MQTT Broker 进行发布/订阅通信。Qt 后台与 RK3576 都作为 MQTT 客户端接入 Broker。

推荐 Topic：

```text
parking/device/gate01/status
parking/device/gate01/heartbeat
parking/device/gate01/vehicle/entry
parking/device/gate01/vehicle/exit
parking/device/gate01/cmd
parking/device/gate01/ota
parking/payment/{order_id}
```

### 10.1 车辆进入消息

```json
{
    "type": "vehicle_entry",
    "plate": "粤A12345",
    "time": "2026-08-17 15:30:10",
    "gate": "entry_01"
}
```

### 10.2 车位状态

```json
{
    "type": "parking_status",
    "total": 100,
    "occupied": 73,
    "free": 27
}
```

### 10.3 设备状态

```json
{
    "type": "device_status",
    "device_id": "parking_terminal_01",
    "camera": "online",
    "database": "normal",
    "version": "1.2.0"
}
```

建议支持：

- MQTT 发布/订阅
- JSON Payload
- Topic 规划
- QoS
- 心跳/在线状态
- 断线重连
- 遗嘱消息（LWT）
- 远程控制命令

---

## 11. Qt 管理后台

Qt 后台主要页面：

```text
首页 Dashboard
├── 总车位
├── 剩余车位
├── 今日入场车辆
├── 今日收入
└── 设备状态

车辆管理
├── 普通车辆
└── 月卡车辆

车位管理
├── A区
├── B区
└── 实时车位状态

停车记录
├── 入场时间
├── 出场时间
├── 停车时长
└── 停车费用

设备管理
├── 在线状态
├── 摄像头状态
├── 软件版本
└── OTA升级
```

---

## 12. HTTP + MQTT 模拟支付

普通车辆出场时：

```text
停车费用
    ↓
生成二维码
    ↓
手机扫码
    ↓
HTTP请求
    ↓
模拟支付服务器
    ↓
支付成功
    ↓
支付服务器通过 MQTT 发布支付结果
    ↓
RK3576收到结果
    ↓
开闸
```

示例：

```http
POST /api/payment
```

```json
{
    "order_id": "P202608170001",
    "plate": "粤A12345",
    "amount": 12.0
}
```

返回：

```json
{
    "status": "success"
}
```

HTTP 主要负责“请求/响应业务”和文件下载，MQTT 主要负责设备状态上报、事件通知、远程控制以及支付/OTA 等异步消息。

---

## 13. 多线程设计

建议线程划分：

```text
main thread
│
├── camera_thread
├── recognition_thread
├── network_thread
├── database_thread
├── parking_space_thread
├── ui_thread
└── ota_thread
```

线程之间采用：

- mutex
- condition variable
- message queue
- ring buffer
- eventfd（可选）

进行同步。

需要重点解决：

- 摄像头采集速度与识别速度不一致
- 网络发送阻塞
- 数据库并发访问
- 线程退出与资源释放
- 异常情况下的状态恢复

---

## 14. IPC 设计（可选进阶）

如果后期改成多进程架构：

```text
camera_process
      ↓
shared memory
      ↓
recognition_process
      ↓
message queue
      ↓
parking_service
      ↓
mqtt_service
```

可使用：

- 共享内存
- POSIX 信号量
- 消息队列
- socketpair

这样可以进一步体现 Linux 系统编程能力。

---

# 15. OTA 升级系统

## 15.1 OTA 升级什么？

本项目优先实现 **应用级 OTA**。MQTT 负责升级通知和版本信息下发，HTTP 负责实际升级包下载。主要升级：

1. RK3576 主应用程序
2. 配置文件
3. 车牌识别模型
4. LVGL 资源文件

例如：

```text
parking_v1.2.0.tar.gz
```

升级包：

```text
parking_v1.2.0/
├── bin/
│   └── parking_terminal
├── config/
│   └── config.json
├── model/
│   └── plate_recognition.onnx
├── resource/
│   └── ui_resource.bin
└── manifest.json
```

---

## 15.2 manifest.json

```json
{
    "version": "1.2.0",
    "app": "parking_terminal",
    "sha256": "xxxxxxxxxxxxxxxx",
    "min_version": "1.0.0"
}
```

---

## 15.3 OTA 流程

```text
Qt后台 / OTA服务器
        ↓
MQTT接收升级通知
        ↓
比较版本号
        ↓
HTTP下载升级包
        ↓
SHA256校验
        ↓
解压到临时目录
        ↓
备份旧版本
        ↓
安装新版本
        ↓
启动新程序
        ↓
健康检测
        ↓
成功 → 保留新版本
失败 → 自动回滚
```

---

## 15.4 应用级回滚

推荐目录：

```text
/app/
├── current -> version_1.2.0
├── version_1.1.0/
│   └── parking_terminal
└── version_1.2.0/
    └── parking_terminal
```

新版本启动失败：

```text
version_1.2.0
    ↓
健康检测失败
    ↓
current重新指向version_1.1.0
    ↓
恢复旧版本
```

可以使用 systemd 负责：

- 程序启动
- 异常重启
- OTA 后启动新版本
- 健康检测失败后的恢复

---

## 16. 后续进阶：系统级 A/B OTA

应用级 OTA 完成以后，再考虑：

```text
eMMC
├── boot_a
├── rootfs_a
├── boot_b
└── rootfs_b
```

流程：

```text
当前运行 rootfs_a
    ↓
升级 rootfs_b
    ↓
修改 U-Boot 启动参数
    ↓
重启
    ↓
启动 rootfs_b
    ↓
健康检测
    ├── 成功 → 确认升级
    └── 失败 → 回滚 rootfs_a
```

涉及：

- U-Boot
- bootargs
- eMMC 分区
- rootfs
- watchdog
- bootcount
- rollback

系统级 OTA 属于后期进阶内容，不建议作为项目第一阶段重点。

---

# 17. 推荐开发优先级

## 第一阶段：必须完成

- [ ] RK3576 基础 Linux 环境
- [ ] V4L2 摄像头抓拍
- [ ] GPIO 控制模拟道闸
- [ ] SQLite 车辆管理
- [ ] 剩余车位统计
- [ ] MQTT + JSON Payload
- [ ] Qt 管理后台
- [ ] 多线程架构

---

## 第二阶段：增强业务

- [ ] 车牌识别
- [ ] 月卡车辆
- [ ] 停车计费
- [ ] HTTP 模拟支付
- [ ] LVGL 本地界面
- [ ] MQTT 心跳/在线状态 + 断线重连

---

## 第三阶段：提高项目含金量

- [ ] STM32 车位节点
- [ ] RS485
- [ ] Modbus RTU
- [ ] 具体车位状态
- [ ] OTA版本检测
- [ ] HTTP下载升级包
- [ ] SHA256校验
- [ ] 自动回滚

---

## 第四阶段：可选高阶

- [ ] systemd 服务管理
- [ ] watchdog
- [ ] 日志上传
- [ ] A/B rootfs OTA
- [ ] U-Boot 回滚

---

# 18. 简历推荐写法

## 项目名称

**基于 RK3576 的智慧停车场管理系统**

## 项目描述

基于 RK3576 设计智慧停车场嵌入式终端，实现车辆识别、车位管理、停车计费、设备监控及远程升级，并通过 Qt 后台实现停车场设备与业务数据的统一管理。

## 简历核心描述

1. 基于 **V4L2** 完成 USB 摄像头图像采集，并结合 OpenCV/车牌识别模块实现车辆入场、出场识别及抓拍。

2. 基于 **SQLite3** 设计车辆、车位及停车记录数据库，实现剩余车位统计、月卡车辆管理及停车计费等业务。

3. 基于 **MQTT + JSON Payload** 实现 RK3576、MQTT Broker 与 Qt 管理后台之间的发布/订阅通信，完成车辆事件、设备状态实时上报以及远程控制，并通过多线程解耦摄像头采集、识别、网络与业务模块。

4. 基于 GPIO 实现道闸及报警设备控制，并可通过 **STM32 + RS485** 扩展车位检测节点，实现具体车位占用状态采集与管理。

5. 设计应用级 **OTA 升级机制**，通过 HTTP 下载升级包，完成版本检测、SHA256 完整性校验、新版本切换及异常自动回滚。

6. 采用 **模块解耦 + 接口抽象 + 可替换后端** 的工程设计，将摄像头、车牌识别、网络、设备控制与业务逻辑分层，实现 RK3576 平台下 V4L2、MIPI CSI、RKMPP 与 RKNN NPU 等可替换后端，并为后续迁移 RK3588 或其他 Rockchip 平台保留统一接口。

---

# 19. 面试重点

这个项目真正需要准备的不是“背项目描述”，而是能够解释下面这些问题。

### 软件架构 / 模块解耦

- 为什么业务层不能直接调用 V4L2 / GPIO
- 什么是接口抽象和依赖倒置
- Camera / Recognizer / Gate / Network 接口如何设计
- Mock 后端有什么作用
- 如何在 RK3576 的 USB/V4L2、MIPI/RKMPP 与 RKNN 后端之间切换，以及后续迁移到 RK3588
- 哪些模块可以复用，哪些模块必须重写
- CPU 推理后端如何替换为 RKNN NPU 后端
- 如何避免平台相关代码污染业务层

### V4L2

- V4L2 采集流程
- mmap 原理
- QBUF / DQBUF
- JPEG / YUYV 区别
- select / poll
- 摄像头异常恢复

### Linux 网络 / MQTT

- MQTT 发布/订阅模型
- Broker 的作用
- Topic 设计
- QoS 0/1/2
- Keep Alive / 心跳
- 遗嘱消息（LWT）
- 断线重连
- JSON Payload 设计
- HTTP 与 MQTT 的职责划分

### Linux 系统编程

- pthread
- mutex
- condition variable
- semaphore
- IPC
- 多线程资源释放
- producer-consumer

### SQLite

- sqlite3_prepare_v2
- sqlite3_bind_xxx
- sqlite3_step
- 事务
- 索引
- 多线程访问

### OTA

- 为什么需要 OTA
- 升级包格式
- 版本判断
- SHA256 的作用
- 下载过程中断怎么办
- 新版本无法启动怎么办
- 如何自动回滚
- 应用 OTA 和 A/B 系统 OTA 的区别

### RS485 / STM32

- UART
- RS485 半双工
- Modbus RTU
- CRC
- MCU 与 MPU 的职责划分

---

# 20. 项目核心价值

这个项目最终应该体现的能力不是：

```text
“我调用了一个车牌识别库。”
```

而是：

```text
我能够在嵌入式 Linux 平台上完成：

设备采集
    ↓
数据处理
    ↓
多线程调度
    ↓
业务逻辑
    ↓
数据库
    ↓
网络通信
    ↓
GUI
    ↓
设备控制
    ↓
远程运维/OTA
    ↓
模块抽象 / 可移植架构
```

最终形成完整的 **嵌入式 Linux 智慧停车终端系统**，并能够通过替换平台适配层和算法后端，从 RK3576 平滑迁移到 RK3588 / 后续更高性能 Rockchip 平台。

这才是项目最适合作为嵌入式 Linux 应用开发岗位简历项目的原因。

# Parking 项目学习路线

本文用于系统学习 Parking 项目，而不是继续增加功能。目标不是记住所有代码，而是能够独立解释
架构、追踪数据、修改模块、定位故障，并在嵌入式 Linux 面试中讲清设计取舍。

## 1. 最终学习目标

完成本路线后，应能够做到：

- 从摄像头采集开始，完整追踪一次车牌入场和收费出场；
- 说明各线程的职责、共享数据、同步方式和退出顺序；
- 解释最新帧槽、有界 FIFO、状态机、SQLite 事务和消息幂等的作用；
- 区分现场 SQLite、后台 SQLite、MQTT 内存队列和 HTTP 支付订单；
- 独立完成宿主测试、RK3576 交叉编译、部署和日志分析；
- 说明哪些正常路径已经真机验证，哪些故障路径尚未验证；
- 不看讲稿完成 5～10 分钟项目介绍，并回答关键技术追问。

## 2. 项目全局图

先记住三条主线，后续所有代码都能放回这三条链路中。

```text
图像链路
USB Camera -> V4L2 -> MJPG/YUYV 转 BGR -> RKNN -> 多帧确认 -> 稳定车牌事件

业务链路
稳定车牌 -> 车牌策略 -> 停车状态机 -> SQLite/计费/支付 -> Gate -> 完成提交

网络链路
设备状态/车辆事件 -> MQTT -> Mosquitto -> FastAPI -> 后台 SQLite -> SSE/Web
                                      ^
手机扫码 -> HTTP 支付确认 ----------------+
```

RK3576 负责实时采集、识别、现场业务、LVGL 和 Gate；VMware 负责 Broker、后台数据库、管理网页
和模拟支付页面。两个 SQLite 数据库用途不同，不能混为一个数据库。

## 3. 学习方法

每个阶段都按下面五个问题学习：

1. 模块解决什么问题？
2. 模块的公开接口和数据结构是什么？
3. 输入来自哪里，输出交给谁？
4. 正常路径和失败路径分别是什么？
5. 为什么这样设计，还有哪些替代方案？

建议为每个阶段输出三样东西：

- 一张自己画的流程图；
- 一页学习笔记；
- 一段不看代码的口头讲解。

不要一开始逐行阅读所有实现。先看头文件和调用关系，再进入实现细节。阅读期间默认使用 Gate
dry-run，不要为了学习代码执行真实 Gate 自动测试。

## 4. 总进度表

| 阶段 | 主题 | 学习产物 | 状态 |
|---|---|---|---|
| 1 | 全局架构与主程序 | 总体架构图、主程序生命周期图 | [ ] |
| 2 | 摄像头与识别管线 | 线程图、帧所有权说明 | [ ] |
| 3 | 停车业务状态机 | 入口/出口状态迁移图 | [ ] |
| 4 | SQLite 与计费 | 表关系图、事务和幂等说明 | [ ] |
| 5 | LVGL 与 Gate | UI 更新链路、Gate 时序图 | [ ] |
| 6 | MQTT 与协议 | Topic 图、重连和 QoS 1 说明 | [ ] |
| 7 | 后台与 HTTP 支付 | 后台数据流、支付时序图 | [ ] |
| 8 | 构建、部署与测试 | 独立构建部署记录、故障清单 | [ ] |
| 9 | 综合代码追踪与面试 | 完整链路讲解、模拟面试答案 | [ ] |

## 5. 阶段一：全局架构与主程序

### 5.1 学习目标

- 知道项目由哪些库和可执行程序组成；
- 理解 `parking` 从启动到退出的完整生命周期；
- 明确主线程、管线线程、MQTT 线程和支付线程的边界；
- 能从 `main.c` 找到每个业务动作对应的模块调用。

### 5.2 阅读顺序

```text
HANDOFF.md
CMakeLists.txt
include/plate_pipeline.h
include/parking_controller.h
include/parking_store.h
include/parking_ui.h
include/parking_mqtt_reporter.h
include/parking_payment.h
src/main.c
```

头文件只关注公开类型、配置、返回值和生命周期函数。阅读 `main.c` 时按下面顺序划分：

```text
命令行解析
-> 配置结构初始化
-> controller/fee/store/ui/gate/mqtt/payment 创建
-> pipeline 启动
-> 主循环
-> Ctrl+C 停止
-> 反向释放资源
```

### 5.3 必须回答的问题

- 为什么 `main.c` 不直接实现 Camera 和 Recognizer 线程？
- 哪些对象始终创建，哪些对象由命令行参数选择性创建？
- 为什么 MQTT 和支付失败不能阻塞图像主循环？
- 主循环每轮按什么顺序更新 Gate、支付、车牌事件、状态机、MQTT 和 UI？
- Ctrl+C 后为什么要先停止生产数据的模块，再释放消费者和资源？

### 5.4 实践任务

- [ ] 画出 `main()` 的创建和清理顺序；
- [ ] 找到 `--lane-mode`、`--enable-gate`、`--mqtt-host` 和
  `--payment-base-url` 的处理位置；
- [ ] 追踪一次 `PARKING_COMMAND_REQUEST_EXIT_CHECK` 由谁产生、由谁执行；
- [ ] 不看代码讲解一次程序从启动到进入主循环的过程。

### 5.5 完成标准

能够在白板上画出主要模块，并在 5 分钟内说明模块之间的调用关系，不需要描述 RKNN 或 SQLite
内部细节。

## 6. 阶段二：摄像头与识别管线

### 6.1 学习目标

- 理解 V4L2 mmap 采集流程和图像格式转换；
- 理解采集、识别和显示之间的数据传递；
- 掌握最新帧槽、有界事件 FIFO 和帧引用生命周期；
- 理解多帧确认、漏检容忍和冷却机制。

### 6.2 阅读顺序

```text
include/camera.h
src/camera/camera.c
include/image_convert.h
src/image/image_convert.c
include/pipeline_frame.h
src/pipeline/pipeline_frame.c
include/latest_frame_slot.h
src/pipeline/latest_frame_slot.c
include/plate_confirmation.h
src/pipeline/plate_confirmation.c
src/pipeline/plate_event_queue.h
src/pipeline/plate_event_queue.c
include/plate_recognizer.h
src/recognizer/
include/plate_pipeline.h
src/pipeline/plate_pipeline.c
```

### 6.3 必须回答的问题

- V4L2 的 `REQBUFS`、`QUERYBUF`、`QBUF`、`DQBUF` 和 `STREAMON` 分别做什么？
- 为什么 RKNN 前需要统一成 BGR888？
- 为什么实时画面采用容量为 1 的最新帧槽，而不采用普通 FIFO？
- 为什么稳定车牌事件必须使用有界 FIFO？
- 一帧图像在不同线程之间如何转移所有权，何时释放？
- 队列满时为什么选择显式失败，而不是静默丢弃后继续？
- 连续三帧确认如何降低偶发误识别？

### 6.4 实践任务

- [ ] 画出 Camera、Recognizer、主线程以及两个队列之间的关系；
- [ ] 从 `plate_pipeline_start()` 追踪到两个工作线程的入口；
- [ ] 找出最新帧被覆盖时旧帧如何释放；
- [ ] 找出稳定事件 FIFO 满时的错误传播路径；
- [ ] 使用一张纸说明“帧”和“稳定车牌事件”为什么采用不同队列策略。

### 6.5 完成标准

能够解释项目为什么不会让慢速识别造成摄像头帧无限积压，并能说明每一帧最终由谁释放。

## 7. 阶段三：停车业务状态机

### 7.1 学习目标

- 掌握入口和出口模式的状态迁移；
- 理解稳定车牌、业务检查、支付、Gate、通行和提交事件；
- 理解状态机核心为什么不直接访问硬件、数据库和网络；
- 掌握重复车牌、歧义车牌和车道忙碌策略。

### 7.2 阅读顺序

```text
include/parking_controller.h
src/business/parking_controller.c
include/parking_plate_policy.h
src/business/parking_plate_policy.c
tests/test_parking_plate_policy.c
src/main.c 中 execute_demo_commands() 及其调用链
```

### 7.3 必须回答的问题

- 入口和出口从 `IDLE` 开始分别经过哪些状态？
- 状态机的 event、command 和 state 有什么区别？
- 为什么先输出检查命令，再通过检查结果事件推进状态？
- 免费出口和收费出口在哪个位置分流？
- 支付成功为什么不能直接操作 Gate？
- `WAIT_PASSAGE` 为什么使用“连续一段时间没有看到车牌”请求关闸？
- 相同车牌、编辑距离为 1 的车牌和其他车辆分别怎样处理？
- 超时、拒绝和错误状态如何恢复？

### 7.4 实践任务

- [ ] 分别画出入口、免费出口和收费出口状态图；
- [ ] 追踪 `浙A66666` 从稳定事件到 SQLite 入场完成；
- [ ] 追踪无活动会话时为什么不会产生开闸命令；
- [ ] 为“支付超时”和“Gate 打不开”各写一条预期状态迁移；
- [ ] 不看代码解释为什么状态机适合纯 C 单元测试。

### 7.5 完成标准

能够根据一段运行日志反推出当前状态、上一个事件和下一个可能命令。

## 8. 阶段四：SQLite 与计费

### 8.1 学习目标

- 理解现场数据库的三个核心表；
- 掌握检查和完成分离、事务、唯一约束与幂等；
- 理解跨进程重启后未完成 operation 的复用；
- 掌握整数计费规则和金额冻结。

### 8.2 阅读顺序

```text
include/parking_store.h
src/storage/parking_store.c
tests/test_parking_store.c
include/parking_fee.h
src/business/parking_fee.c
tests/test_parking_fee.c
```

### 8.3 必须回答的问题

- `parking_events`、`parking_sessions` 和 `parking_spaces` 分别保存什么？
- 为什么入场检查通过后不能立刻增加已占用车位？
- operation ID 如何关联检查、会话和最终提交？
- 同一 operation 重复完成为什么不会重复修改车位？
- 进程在开闸后退出，重启时数据库会保留什么？
- 为什么出口金额在首次检查后冻结？
- 为什么费用全部使用整数分而不是浮点元？
- 30 分钟免费、每 10 分钟 50 分和 2000 分封顶如何处理边界？

### 8.4 实践任务

- [ ] 画出三个表的关系和关键唯一约束；
- [ ] 手工计算 29、30、31、40、41 分钟的费用；
- [ ] 从 `parking_store_check_exit()` 追踪到费用冻结；
- [ ] 从 `parking_store_complete_exit()` 追踪事务内的所有修改；
- [ ] 阅读测试并解释每个幂等场景在防止什么错误。

### 8.5 完成标准

能够解释“开闸成功但提交前掉电”时数据库中可能留下什么，以及系统为什么不能擅自推断车辆已经
通过。

## 9. 阶段五：LVGL 与 Gate

### 9.1 学习目标

- 理解 LVGL、DRM/KMS 和实时画面的组合方式；
- 理解 UI 主线程约束和支付二维码显示条件；
- 掌握 PWM sysfs、Gate 异步更新和 dry-run 边界。

### 9.2 阅读顺序

```text
include/parking_ui.h
src/ui/parking_ui.c
include/parking_lvgl_demo.h
src/ui/parking_lvgl_demo.c
tests/test_lvgl_display.c
include/pwm_sysfs.h
src/gate/pwm_sysfs.c
include/gate.h
src/gate/gate.c
tests/test_gate.c
```

### 9.3 必须回答的问题

- DRM/KMS、fbdev 和 LVGL 各自处在哪一层？
- 为什么 LVGL 对象只在主线程更新？
- BGR 摄像头帧怎样进入 LVGL canvas？
- 支付二维码根据哪个业务状态显示和隐藏？
- Gate 的 open/close 命令为什么不是瞬间完成？
- `gate_update()` 如何把硬件时间过程转成业务可观察状态？
- 为什么正式程序默认 Gate dry-run？
- 没有地感、限位和防砸传感器会带来什么安全边界？

### 9.4 实践任务

- [ ] 画出 Camera 帧到 LCD 的显示路径；
- [ ] 追踪一次 `REQUEST_OPEN_GATE` 到 `GATE_OPENED`；
- [ ] 找出真实 Gate 与 dry-run 的分支位置；
- [ ] 说明此前 DRM plane 层级冲突为什么会导致界面被覆盖；
- [ ] 说明二维码支付期间 UI、支付线程和主线程如何协作。

### 9.5 完成标准

能够说明 UI 为什么不会直接发起支付或控制 Gate，并能准确描述真实 Gate 测试的安全条件。

## 10. 阶段六：MQTT 与协议

### 10.1 学习目标

- 掌握 MQTT Topic、JSON v1 协议和消息类型；
- 理解网络工作线程、有界发布队列和非阻塞上报；
- 掌握 QoS 1、PUBACK、LWT、心跳、retained 和重连；
- 理解数据库业务成功与网络上报失败之间的边界。

### 10.2 阅读顺序

```text
docs/MQTT_PROTOCOL_V1.md
include/parking_protocol.h
src/network/parking_protocol.c
tests/test_parking_protocol.c
include/parking_mqtt.h
src/network/parking_mqtt_transport.h
src/network/parking_mqtt.c
src/network/parking_mqtt_mosquitto.c
tests/test_parking_mqtt.c
include/parking_mqtt_reporter.h
src/network/parking_mqtt_reporter.c
tests/test_parking_mqtt_reporter.c
```

### 10.3 必须回答的问题

- availability、heartbeat、status 和 entry/exit event 有什么区别？
- retained 消息解决什么问题？
- LWT 如何处理进程被 `SIGKILL` 或网络突然断开？
- QoS 1 为什么仍可能让订阅端收到重复消息？
- 发布队列为什么必须有容量上限？
- Broker 中断后队首消息如何保留并重发？
- 为什么 MQTT 失败不能回滚已经提交的 SQLite 业务？
- 当前内存发布队列与 SQLite outbox 有什么差别？
- 为什么交叉构建采用 `dlopen("libmosquitto.so.1")`？

### 10.4 实践任务

- [ ] 画出所有正式 Topic 和发布时机；
- [ ] 追踪一次 committed exit 从 SQLite 到 MQTT PUBACK；
- [ ] 追踪 Broker 断开、退避和重新连接流程；
- [ ] 解释后台为什么仍需要 `(site_id, device_id, message_id)` 去重；
- [ ] 根据测试说明正常离线与异常 LWT 离线的区别。

### 10.5 完成标准

能够清楚回答“已经用了 QoS 1，为什么还需要幂等”和“网络失败为什么不影响车辆放行”。

## 11. 阶段七：后台与 HTTP 模拟支付

### 11.1 学习目标

- 理解 FastAPI 后台如何消费 MQTT 并维护设备快照；
- 理解后台 SQLite 与 RK3576 SQLite 的职责区别；
- 掌握 SSE 页面更新和只读管理边界；
- 掌握订单创建、token、过期、确认和板端轮询流程。

### 11.2 阅读顺序

```text
backend/README.md
backend/app/config.py
backend/app/protocol.py
backend/app/database.py
backend/app/mqtt_subscriber.py
backend/app/events.py
backend/app/main.py
backend/static/index.html
backend/static/app.js
backend/tests/
include/parking_payment.h
src/network/parking_payment.c
tests/test_parking_payment_live.c
```

### 11.3 必须回答的问题

- 后台收到 MQTT 消息后经过哪些校验？
- heartbeat 为什么不逐条保存历史？
- SSE 与普通 HTTP 轮询相比解决什么问题？
- 为什么管理页面保持只读，不能远程控制 Gate？
- 支付 token 和 operation ID 各自解决什么问题？
- 重复创建同一 operation 的支付订单会怎样？
- 订单为什么需要过期时间？
- 手机确认后，RK3576 怎样发现状态变为 `paid`？
- 为什么支付客户端使用独立 pthread 和 socket 超时？
- 为什么当前 HTTP 方案只能运行在可信局域网？

### 11.4 实践任务

- [ ] 画出 MQTT 消息进入后台并刷新网页的完整流程；
- [ ] 画出 RK3576、FastAPI 和手机之间的支付时序图；
- [ ] 追踪 `POST /api/payments` 对数据库的读写；
- [ ] 追踪 `POST /pay/{token}/confirm` 后状态怎样回到板端；
- [ ] 阅读支付测试并解释幂等、过期和重复确认场景。

### 11.5 完成标准

能够从日志解释一次 50 分订单的创建、手机确认、`PAID` 轮询、开闸和 SQLite 出场提交。

## 12. 阶段八：构建、部署与测试

### 12.1 学习目标

- 理解 CMake target 和模块依赖；
- 区分宿主测试、交叉编译、板端自动测试和真机硬件测试；
- 能够独立构建、复制、运行和回退程序；
- 能够根据日志判断故障属于哪一层。

### 12.2 阅读顺序

```text
CMakeLists.txt
cmake/toolchain-rk3576.cmake
cmake/Findlibdrm.cmake
tests/
backend/requirements.txt
backend/requirements-dev.txt
backend/README.md
deploy/systemd/
HANDOFF.md 中的验证记录和限制
```

### 12.3 必须回答的问题

- 哪些模块可以在 x86 宿主测试，哪些必须在 RK3576 上运行？
- 交叉编译器、sysroot、目标头文件和目标动态库分别起什么作用？
- 编译成功、自动测试通过和真机验证成功有什么区别？
- 为什么真实 Gate 测试不能自动执行？
- 如何判断问题位于 Camera、Recognizer、业务状态机、SQLite、MQTT 还是后台？
- systemd、异常重启和在线备份为什么属于部署收尾，而不是核心业务？

### 12.4 实践任务

- [ ] 从干净终端独立完成一次 RK3576 `parking` 构建；
- [ ] 说明 `parking` 链接了哪些项目库；
- [ ] 独立把新二进制复制到板端并保留旧版本；
- [ ] 运行一组不访问硬件的板端测试；
- [ ] 为 Camera 打不开、数据库拒绝、Broker 断开和支付超时分别写出排查步骤；
- [ ] 最终功能确认后，再按计划验证 systemd 和每日备份。

### 12.5 完成标准

不依赖复制旧命令，能够说明每个构建和部署参数的含义，并能根据第一条错误日志选择正确模块排查。

## 13. 阶段九：综合代码追踪与面试准备

### 13.1 完整追踪任务

至少独立完成以下四次代码追踪：

- [ ] 正常入场：稳定车牌到 SQLite entry committed；
- [ ] 免费出场：出口检查到 Gate 关闭和 SQLite exit committed；
- [ ] 收费出场：计费、订单、手机确认、`PAID`、Gate 和提交；
- [ ] 异常路径：无活动会话或 Broker 中断，说明为什么不会错误开闸或回滚业务。

每次追踪都记录：

```text
起点事件：
调用过的公开接口：
发生状态变化的位置：
数据库修改：
线程切换：
最终日志：
失败时的行为：
```

### 13.2 必须准备的面试问题

- 为什么使用最新帧槽而不是图像 FIFO？
- 如何保证跨线程图像内存安全？
- 状态机相比大量 `if/else` 有什么优势？
- SQLite 如何实现幂等和跨重启恢复？
- QoS 1 为什么还要做数据库唯一约束？
- 支付网络线程如何把结果安全地交给主线程？
- DRM/KMS 与 framebuffer 有什么区别？
- 为什么采用动态加载 Mosquitto？
- 项目发生过最难定位的问题是什么，如何缩小范围？
- 如果继续产品化，最优先补哪些可靠性和安全功能？

### 13.3 一分钟项目介绍框架

```text
项目目标和硬件平台
-> 图像采集与 RKNN 识别
-> 多线程管线与稳定事件
-> 状态机、SQLite 和计费
-> LVGL 与真实 Gate
-> MQTT、后台和二维码支付
-> 真机验证结果
-> 当前原型边界
```

### 13.4 完成标准

- 能在 1 分钟、5 分钟和 10 分钟三个长度下介绍项目；
- 能不看代码画出线程图、状态图和支付时序图；
- 能区分“已经验证”“已经实现但未专项验证”和“明确不在范围”；
- 遇到追问时使用代码和测试说明依据，不编造性能数据。

## 14. 学习笔记模板

每学习一个模块，可以复制下面的模板：

```markdown
# 模块名称

## 解决的问题

## 公开接口

## 核心数据结构

## 输入与输出

## 所属线程

## 正常流程

## 失败流程

## 资源创建与释放

## 关键设计取舍

## 已验证范围

## 尚未验证范围

## 我还不能回答的问题
```

## 15. 学习纪律

- 先理解接口和数据流，再阅读函数内部实现；
- 每个结论都尽量找到代码、测试或真机日志依据；
- 不把“编译成功”说成“真机验证成功”；
- 不编造 FPS、延迟、准确率、温度或资源占用；
- 不为了学习随意重构已验证模块；
- 不自动执行真实 Gate 测试；
- 不把微信/支付宝、账号、HTTPS、OTA 当作当前项目缺失；
- systemd 和自动备份留到全部功能确认后的最终部署阶段。

完成这份路线的标志不是所有复选框都被机械勾选，而是能够把项目当作一个完整系统解释：数据从
哪里来、由谁处理、保存在哪里、如何跨线程和跨设备传递、失败后会发生什么，以及当前设计为什么
适合这个嵌入式 Linux 原型。

# Parking MQTT/JSON 协议 v1

本文冻结 RK3576 停车终端第一版 MQTT Topic 与上行 JSON Payload。当前阶段只实现协议编码，
不连接 Broker，不订阅命令，也不允许远程控制 Gate。

## 1. Topic

Topic 根路径固定为：

```text
parking/v1/{site_id}/{device_id}
```

`site_id` 和 `device_id` 长度为 1～64 字节，只允许 ASCII 字母、数字、点、下划线和连字符，
禁止 `/`、`+`、`#`，避免层级和通配符注入。

| 后缀 | 方向 | QoS | Retain | 用途 |
| --- | --- | ---: | --- | --- |
| `availability` | 设备 → 后台 | 1 | 是 | 上线、正常离线和 LWT 离线状态 |
| `heartbeat` | 设备 → 后台 | 0 | 否 | 周期心跳、启动时长和序号 |
| `status` | 设备 → 后台 | 1 | 是 | 设备、业务、Gate 和车位快照 |
| `events/entry` | 设备 → 后台 | 1 | 否 | SQLite 入场提交完成事件 |
| `events/exit` | 设备 → 后台 | 1 | 否 | SQLite 出场提交完成事件 |
| `commands/request` | 后台 → 设备 | 1 | 否 | 预留，v1 当前不订阅 |
| `commands/reply` | 设备 → 后台 | 1 | 否 | 预留，v1 当前不发布 |

完整示例：

```text
parking/v1/lot-a/gate01/events/entry
```

## 2. 公共字段

所有已实现的上行 JSON 都包含：

| 字段 | 类型 | 语义 |
| --- | --- | --- |
| `schema_version` | integer | 固定为 `1` |
| `type` | string | 消息类型 |
| `message_id` | string | 逻辑消息唯一 ID；QoS 重试必须复用原值 |
| `timestamp_unix_us` | integer | 创建消息时的 Unix epoch 微秒，不能使用单调时钟 |
| `site_id` | string | 停车场 ID，与 Topic 一致 |
| `device_id` | string | 终端 ID，与 Topic 一致 |
| `session_id` | string | 本次进程/MQTT 会话 ID，用于区分重启和旧 LWT |

金额统一使用整数分，字段名为 `amount_cents`。检测置信度是检测框置信度，不表示 OCR
字符置信度。JSON 字符串保持 UTF-8，控制字符、双引号和反斜杠必须转义。

## 3. Availability

上线、正常离线和 LWT 共用 retained 消息：

```json
{
  "schema_version": 1,
  "type": "availability",
  "message_id": "availability-session-7-online",
  "timestamp_unix_us": 1700000000000000,
  "site_id": "lot-a",
  "device_id": "gate01",
  "session_id": "session-7",
  "state": "online"
}
```

`state` 只能为 `online` 或 `offline`。LWT 在连接前生成，其 `timestamp_unix_us` 是 LWT
创建时间，不是 Broker 实际判定掉线的时间；后台应以消息到达时间判断实际掉线时刻。

## 4. Heartbeat

```json
{
  "schema_version": 1,
  "type": "heartbeat",
  "message_id": "heartbeat-session-7-9",
  "timestamp_unix_us": 1700000000000000,
  "site_id": "lot-a",
  "device_id": "gate01",
  "session_id": "session-7",
  "sequence": 9,
  "uptime_us": 123456789
}
```

`sequence` 在当前进程会话中单调递增；`uptime_us` 来自单调时钟，但只表示启动时长。

## 5. Device status

```json
{
  "schema_version": 1,
  "type": "device_status",
  "message_id": "status-session-7-12",
  "timestamp_unix_us": 1700000000000000,
  "site_id": "lot-a",
  "device_id": "gate01",
  "session_id": "session-7",
  "software_version": "1.0.0",
  "lane_mode": "exit",
  "business_state": "wait_payment",
  "gate": {
    "mode": "real",
    "state": "closed"
  },
  "components": {
    "camera": "ok",
    "recognizer": "ok",
    "database": "ok"
  },
  "spaces": {
    "total": 100,
    "occupied": 72,
    "pending_entries": 1,
    "available": 27
  }
}
```

枚举值：

- `lane_mode`：`entry`、`exit`；
- `business_state`：`initializing`、`idle`、`checking`、`wait_payment`、`opening`、
  `wait_passage`、`closing`、`completing`、`reject_cooldown`、`error`；
- `gate.mode`：`dry_run`、`real`；
- `gate.state`：`closed`、`opening`、`open`、`closing`、`error`；
- 组件状态：`ok`、`unavailable`、`error`。

车位字段必须满足：

```text
occupied + pending_entries + available == total
```

## 6. Vehicle event

车辆事件只在对应 SQLite operation 已提交后发布。发布失败时允许重试，同一 operation 的
`message_id` 必须保持不变，后台按 `message_id` 或 `operation_id + type` 幂等处理。

```json
{
  "schema_version": 1,
  "type": "vehicle_exit",
  "message_id": "exit-gate01-43",
  "timestamp_unix_us": 1700000000000000,
  "site_id": "lot-a",
  "device_id": "gate01",
  "session_id": "session-7",
  "operation_id": 43,
  "lane_mode": "exit",
  "plate_number": "浙A66666",
  "plate_color": "蓝色",
  "detect_confidence": 0.95,
  "amount_cents": 50,
  "passage_source": "vision_timeout",
  "result": "committed",
  "spaces": {
    "total": 100,
    "occupied": 71,
    "pending_entries": 0,
    "available": 29
  }
}
```

`type` 只能为 `vehicle_entry` 或 `vehicle_exit`；入场的 `amount_cents` 必须为 0。
`passage_source` 只能为 `sensor`、`manual` 或 `vision_timeout`。

## 7. 后续接入约束

- MQTT 网络线程负责重连、退避、LWT、心跳和发布队列，主业务线程不能等待网络 I/O；
- `availability`、`status` 和车辆事件使用 QoS 1，消费者必须按 ID 幂等；
- 第一版不接受远程开闸、关闸或修改业务状态的命令；
- MQTT 内存队列不能代替 SQLite。车辆事件可靠重发需要单独设计持久化 outbox；
- 协议不传输单调时钟绝对值，只传 Unix 时间或明确标注的启动时长。

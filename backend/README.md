# Parking 只读后台与 Web 管理页面

第一版后台运行在 VMware，订阅现有 MQTT/JSON v1 上行消息，把设备快照和已提交车辆事件保存到独立 SQLite，并提供同源只读 API、SSE 实时刷新和原生网页。

它不会订阅 `commands/request`，也没有开闸、删除记录、修改计费、支付或 OTA 接口。

## 1. 已确认的开发环境

```text
VMware / Broker / Web: 172.20.10.2
RK3576:                 172.20.10.14
MQTT:                   172.20.10.2:1883
```

Mosquitto 当前配置仅适合受信任开发网络。正式部署前应关闭匿名访问并配置账号或 TLS。

## 2. 创建 Python 环境

在 VMware 打开终端：

```bash
cd /home/dyj/project/Parking/backend
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements-dev.txt
```

以后重新打开终端时，只需要：

```bash
cd /home/dyj/project/Parking/backend
source .venv/bin/activate
```

## 3. 运行自动测试

```bash
cd /home/dyj/project/Parking/backend
source .venv/bin/activate
pytest -q
```

## 4. 启动后台

```bash
cd /home/dyj/project/Parking/backend
source .venv/bin/activate

export PARKING_MQTT_HOST=172.20.10.2
export PARKING_MQTT_PORT=1883
export PARKING_TIMEZONE=Asia/Shanghai

uvicorn app.main:app --host 127.0.0.1 --port 8000
```

浏览器打开：

```text
http://127.0.0.1:8000
```

当前页面包含车牌和车辆进出记录，只建议在 VMware 本机预览。若后续需要让平板或手机访问，
应先增加登录认证、限制可信网络，并按部署环境配置 HTTPS；不要直接绑定到不受信任网络或公网。

健康检查：

```bash
curl http://127.0.0.1:8000/api/health
```

运行数据默认写入：

```text
backend/data/parking_backend.db
```

## 5. 开发板联调

先在 VMware 启动后台，再在开发板的部署目录运行正式程序。下面默认保持 Gate dry-run：

```bash
./parking \
  --lane-mode entry \
  --database parking-web-test.db \
  --mqtt-host 172.20.10.2 \
  --mqtt-site parking-test \
  --mqtt-device rk3576
```

完成一次入场后，用同一数据库测试出场：

```bash
./parking \
  --lane-mode exit \
  --database parking-web-test.db \
  --mqtt-host 172.20.10.2 \
  --mqtt-site parking-test \
  --mqtt-device rk3576
```

不要在这一步添加 `--enable-gate`。网页只显示 SQLite 已经提交的 entry/exit 事件。

## 6. API

```text
GET /api/health
GET /api/overview
GET /api/devices
GET /api/events
GET /api/events?plate=浙A66666
GET /api/events?event_type=entry&date_from=2026-10-15&date_to=2026-10-15
GET /api/stream
```

`/api/stream` 使用 SSE 通知页面重新获取最新快照。heartbeat 只更新设备的最后在线时间和序号，不逐条保存历史记录。QoS 1 消息以 `(site_id, device_id, message_id)` 唯一约束去重。

后台以 MQTT 实际接收时间记录 offline 时刻，不把 LWT Payload 中接近启动时间的时间戳当作真实掉线时间。

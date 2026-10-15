# Parking 后台、网页与 HTTP 模拟支付

后台运行在 VMware，订阅 MQTT/JSON v1 上行消息，保存设备快照和已提交车辆事件，并提供
管理网页、SSE 实时刷新及局域网 HTTP 模拟支付。管理网页不提供远程开闸、记录删除、计费修改
或 OTA；支付确认只能完成当前出口订单，开发板仍由原有业务状态机决定是否开闸。

## 1. 开发环境

```text
VMware / Broker / Web: 172.20.10.2
RK3576:                 172.20.10.14
MQTT:                   172.20.10.2:1883
Web / 模拟支付:          http://172.20.10.2:8000
```

当前方案不配置账号和 HTTPS，只适合个人热点或可信局域网，不能暴露到公网、校园公共网络或其他
不受信任网络。

## 2. 创建 Python 环境

```bash
cd /home/dyj/project/Parking/backend
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements-dev.txt
```

## 3. 运行自动测试

```bash
cd /home/dyj/project/Parking/backend
source .venv/bin/activate
pytest -q
```

## 4. 手工启动与局域网访问

```bash
cd /home/dyj/project/Parking/backend
source .venv/bin/activate

export PARKING_MQTT_HOST=172.20.10.2
export PARKING_MQTT_PORT=1883
export PARKING_TIMEZONE=Asia/Shanghai
export PARKING_PUBLIC_BASE_URL=http://172.20.10.2:8000
export PARKING_PAYMENT_TTL_SECONDS=120

uvicorn app.main:app --host 172.20.10.2 --port 8000
```

VMware、手机和平板连接同一可信局域网后打开：

```text
http://172.20.10.2:8000
```

健康检查：

```bash
curl http://172.20.10.2:8000/api/health
```

后台数据库默认位于 `backend/data/parking_backend.db`。

## 5. 开发板二维码支付联调

交叉编译后，把新的 `parking` 复制到开发板部署目录。先使用已有测试数据库完成一次入场，再把
该会话的入场时间调早以产生非零费用，最后运行出口模式：

```bash
./parking \
  --lane-mode exit \
  --database parking-payment-test.db \
  --mqtt-host 172.20.10.2 \
  --mqtt-port 1883 \
  --mqtt-site parking-test \
  --mqtt-device rk3576 \
  --payment-base-url http://172.20.10.2:8000
```

识别到收费车辆后，RK3576 屏幕会用 LVGL 显示二维码。手机扫码并点击“确认模拟支付”后，
开发板的 HTTP 工作线程会轮询到 `paid`，业务状态机随后继续开闸。默认仍是 Gate dry-run，测试
二维码支付时不要添加 `--enable-gate`。

不传 `--payment-base-url` 时保留原来的立即成功 dry-run 支付，便于离线回归。

## 6. VMware systemd 自启与自动重启

确认 `/home/dyj/project/Parking/backend/.venv` 已安装依赖，然后执行：

```bash
sudo install -d -m 0755 /etc/parking
sudo install -m 0644 \
  /home/dyj/project/Parking/backend/.env.example \
  /etc/parking/backend.env
sudo install -m 0644 \
  /home/dyj/project/Parking/deploy/systemd/parking-backend.service \
  /etc/systemd/system/parking-backend.service
sudo systemctl daemon-reload
sudo systemctl enable --now parking-backend.service
sudo systemctl status parking-backend.service --no-pager
```

服务在异常退出时等待 3 秒自动重启。查看日志：

```bash
journalctl -u parking-backend.service -f
```

## 7. SQLite 每日备份

备份脚本使用 Python SQLite 在线备份 API，不直接复制正在使用的 WAL 数据库。先手工验证一次：

```bash
cd /home/dyj/project/Parking/backend
.venv/bin/python scripts/backup_database.py \
  --database data/parking_backend.db \
  --output backups \
  --retention-days 7
```

启用每天一次的 systemd timer：

```bash
sudo install -m 0644 \
  /home/dyj/project/Parking/deploy/systemd/parking-backend-backup.service \
  /etc/systemd/system/parking-backend-backup.service
sudo install -m 0644 \
  /home/dyj/project/Parking/deploy/systemd/parking-backend-backup.timer \
  /etc/systemd/system/parking-backend-backup.timer
sudo systemctl daemon-reload
sudo systemctl enable --now parking-backend-backup.timer
sudo systemctl start parking-backend-backup.service
systemctl list-timers parking-backend-backup.timer --no-pager
```

备份保存在 `backend/backups/`，默认保留 7 天。

## 8. RK3576 systemd 自启与自动重启

先确认 `/root/test/parking/parking`、模型和字体均已部署。在 VMware 执行：

```bash
scp /home/dyj/project/Parking/deploy/systemd/parking-edge.service \
  root@172.20.10.14:/root/test/parking/
```

然后在板端执行：

```bash
install -m 0644 \
  /root/test/parking/parking-edge.service \
  /etc/systemd/system/parking-edge.service
systemctl daemon-reload
systemctl enable --now parking-edge.service
systemctl status parking-edge.service --no-pager
```

仓库模板位于 `deploy/systemd/parking-edge.service`。模板是出口、Gate dry-run；如需真实 Gate，必须
在板端完成安全检查后人工修改服务参数，不能把 `--enable-gate` 加入自动化测试。

## 9. API

```text
GET  /api/health
GET  /api/overview
GET  /api/devices
GET  /api/events
GET  /api/stream
POST /api/payments
GET  /api/payments/{token}
POST /api/payments/{token}/pay
GET  /pay/{token}
POST /pay/{token}/confirm
```

车辆事件以 `(site_id, device_id, message_id)` 唯一约束处理 MQTT QoS 1 重复消息。支付订单以
`(site_id, device_id, operation_id)` 幂等，默认 120 秒过期。heartbeat 只更新设备最新状态，不
逐条保存历史。

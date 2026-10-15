from __future__ import annotations

import logging
import threading
import time
from typing import Any

import paho.mqtt.client as mqtt

from .config import Settings
from .database import Database
from .events import UpdateHub
from .protocol import ProtocolError, decode_message


LOGGER = logging.getLogger(__name__)


class MqttSubscriber:
    def __init__(
        self, settings: Settings, database: Database, update_hub: UpdateHub
    ) -> None:
        self._settings = settings
        self._database = database
        self._update_hub = update_hub
        self._lock = threading.Lock()
        self._connected = False
        self._last_error = ""
        self._invalid_messages = 0
        self._duplicate_messages = 0
        self._normalized_messages = 0

        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=settings.mqtt_client_id,
            protocol=mqtt.MQTTv311,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)
        if settings.mqtt_username is not None:
            self._client.username_pw_set(
                settings.mqtt_username, settings.mqtt_password
            )
        if settings.mqtt_ca_file is not None:
            self._client.tls_set(ca_certs=settings.mqtt_ca_file)

    def start(self) -> None:
        self._client.connect_async(
            self._settings.mqtt_host,
            self._settings.mqtt_port,
            keepalive=30,
        )
        self._client.loop_start()

    def stop(self) -> None:
        try:
            self._client.disconnect()
        finally:
            self._client.loop_stop()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "connected": self._connected,
                "last_error": self._last_error,
                "invalid_messages": self._invalid_messages,
                "duplicate_messages": self._duplicate_messages,
                "normalized_messages": self._normalized_messages,
                "topic": self._settings.mqtt_topic,
                "broker": f"{self._settings.mqtt_host}:{self._settings.mqtt_port}",
            }

    def _on_connect(
        self,
        client: mqtt.Client,
        userdata: Any,
        flags: mqtt.ConnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del userdata, flags, properties
        if reason_code.is_failure:
            with self._lock:
                self._connected = False
                self._last_error = f"Broker 拒绝连接: {reason_code}"
            LOGGER.error("MQTT Broker 拒绝连接: %s", reason_code)
            return

        result, _ = client.subscribe(self._settings.mqtt_topic, qos=1)
        if result != mqtt.MQTT_ERR_SUCCESS:
            with self._lock:
                self._connected = False
                self._last_error = f"订阅失败: {mqtt.error_string(result)}"
            LOGGER.error("订阅 MQTT Topic 失败: %s", mqtt.error_string(result))
            return

        with self._lock:
            self._connected = True
            self._last_error = ""
        LOGGER.info(
            "已连接 MQTT Broker %s:%d，订阅 %s",
            self._settings.mqtt_host,
            self._settings.mqtt_port,
            self._settings.mqtt_topic,
        )

    def _on_disconnect(
        self,
        client: mqtt.Client,
        userdata: Any,
        disconnect_flags: mqtt.DisconnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del client, userdata, disconnect_flags, properties
        with self._lock:
            self._connected = False
            if reason_code.is_failure:
                self._last_error = f"MQTT 连接中断: {reason_code}"
        if reason_code.is_failure:
            LOGGER.warning("MQTT 连接中断，将自动重连: %s", reason_code)

    def _on_message(
        self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage
    ) -> None:
        del client, userdata
        received_at_us = time.time_ns() // 1000
        try:
            decoded = decode_message(message.topic, bytes(message.payload))
            result = self._database.ingest(decoded, received_at_us)
        except ProtocolError as exc:
            with self._lock:
                self._invalid_messages += 1
                self._last_error = f"协议拒绝 {message.topic}: {exc}"
            LOGGER.warning("拒绝 MQTT 消息 %s: %s", message.topic, exc)
            return
        except Exception as exc:
            with self._lock:
                self._last_error = f"保存消息失败: {exc}"
            LOGGER.exception("保存 MQTT 消息失败")
            return

        with self._lock:
            if decoded.stripped_trailing_nul:
                self._normalized_messages += 1
            if result.duplicate:
                self._duplicate_messages += 1

        if result.inserted:
            self._update_hub.publish_from_thread(
                {
                    "kind": result.kind,
                    "site_id": result.site_id,
                    "device_id": result.device_id,
                    "received_at_us": received_at_us,
                }
            )

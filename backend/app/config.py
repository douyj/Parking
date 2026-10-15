from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


BACKEND_DIR = Path(__file__).resolve().parent.parent


def _env_int(name: str, default: int, minimum: int, maximum: int) -> int:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default

    try:
        value = int(raw)
    except ValueError as exc:
        raise ValueError(f"{name} 必须是整数") from exc
    if value < minimum or value > maximum:
        raise ValueError(f"{name} 必须在 {minimum}～{maximum} 之间")
    return value


@dataclass(frozen=True)
class Settings:
    database_path: Path
    mqtt_host: str
    mqtt_port: int
    mqtt_username: str | None
    mqtt_password: str | None
    mqtt_ca_file: str | None
    mqtt_topic: str
    mqtt_client_id: str
    timezone: str
    public_base_url: str | None = None
    payment_ttl_seconds: int = 120

    @classmethod
    def from_env(cls) -> "Settings":
        database_path = Path(
            os.getenv(
                "PARKING_BACKEND_DATABASE",
                str(BACKEND_DIR / "data" / "parking_backend.db"),
            )
        ).expanduser()
        mqtt_host = os.getenv("PARKING_MQTT_HOST", "127.0.0.1").strip()
        if not mqtt_host:
            raise ValueError("PARKING_MQTT_HOST 不能为空")

        username = os.getenv("PARKING_MQTT_USERNAME") or None
        password = os.getenv("PARKING_MQTT_PASSWORD") or None
        if password is not None and username is None:
            raise ValueError("设置 PARKING_MQTT_PASSWORD 时必须同时设置用户名")

        return cls(
            database_path=database_path,
            mqtt_host=mqtt_host,
            mqtt_port=_env_int("PARKING_MQTT_PORT", 1883, 1, 65535),
            mqtt_username=username,
            mqtt_password=password,
            mqtt_ca_file=os.getenv("PARKING_MQTT_CA_FILE") or None,
            mqtt_topic=os.getenv("PARKING_MQTT_TOPIC", "parking/v1/+/+/#"),
            mqtt_client_id=os.getenv(
                "PARKING_BACKEND_MQTT_CLIENT_ID", "parking-web-backend"
            ),
            timezone=os.getenv("PARKING_TIMEZONE", "Asia/Shanghai"),
            public_base_url=(
                os.getenv("PARKING_PUBLIC_BASE_URL", "").strip().rstrip("/")
                or None
            ),
            payment_ttl_seconds=_env_int(
                "PARKING_PAYMENT_TTL_SECONDS", 120, 30, 600
            ),
        )

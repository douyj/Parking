from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass
from typing import Any


SCHEMA_VERSION = 1
TOPIC_LEVEL = re.compile(r"^[A-Za-z0-9._-]{1,64}$")

TOPIC_TYPES = {
    "availability": "availability",
    "heartbeat": "heartbeat",
    "status": "device_status",
    "events/entry": "vehicle_entry",
    "events/exit": "vehicle_exit",
}

BUSINESS_STATES = {
    "initializing",
    "idle",
    "checking",
    "wait_payment",
    "opening",
    "wait_passage",
    "closing",
    "completing",
    "reject_cooldown",
    "error",
}
GATE_STATES = {"closed", "opening", "open", "closing", "error"}
COMPONENT_STATES = {"ok", "unavailable", "error"}
PASSAGE_SOURCES = {"sensor", "manual", "vision_timeout"}


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class DecodedMessage:
    kind: str
    site_id: str
    device_id: str
    payload: dict[str, Any]
    stripped_trailing_nul: bool = False


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _require_int(
    data: dict[str, Any], name: str, minimum: int = 0
) -> int:
    value = data.get(name)
    if not _is_int(value) or value < minimum:
        raise ProtocolError(f"字段 {name} 必须是大于等于 {minimum} 的整数")
    return value


def _require_string(
    data: dict[str, Any], name: str, maximum_bytes: int = 64
) -> str:
    value = data.get(name)
    if not isinstance(value, str) or not value:
        raise ProtocolError(f"字段 {name} 必须是非空字符串")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ProtocolError(f"字段 {name} 超过 {maximum_bytes} 字节")
    return value


def _require_choice(data: dict[str, Any], name: str, choices: set[str]) -> str:
    value = _require_string(data, name)
    if value not in choices:
        raise ProtocolError(f"字段 {name} 的值无效")
    return value


def _validate_spaces(data: Any) -> None:
    if not isinstance(data, dict):
        raise ProtocolError("字段 spaces 必须是对象")

    total = _require_int(data, "total")
    occupied = _require_int(data, "occupied")
    pending = _require_int(data, "pending_entries")
    available = _require_int(data, "available")
    if occupied + pending + available != total:
        raise ProtocolError("车位统计不满足 occupied + pending + available = total")


def _parse_topic(topic: str) -> tuple[str, str, str]:
    if not isinstance(topic, str):
        raise ProtocolError("Topic 必须是字符串")

    levels = topic.split("/")
    if len(levels) not in (5, 6) or levels[:2] != ["parking", "v1"]:
        raise ProtocolError("Topic 不符合 parking/v1/{site}/{device}/... 格式")

    site_id, device_id = levels[2], levels[3]
    if TOPIC_LEVEL.fullmatch(site_id) is None or TOPIC_LEVEL.fullmatch(device_id) is None:
        raise ProtocolError("Topic 中的 site_id 或 device_id 无效")

    suffix = "/".join(levels[4:])
    expected_type = TOPIC_TYPES.get(suffix)
    if expected_type is None:
        raise ProtocolError("当前后台不接受该 Topic")
    return site_id, device_id, expected_type


def _validate_common(
    data: dict[str, Any], site_id: str, device_id: str, expected_type: str
) -> None:
    if data.get("schema_version") != SCHEMA_VERSION:
        raise ProtocolError("仅支持 schema_version=1")
    if data.get("type") != expected_type:
        raise ProtocolError("Payload type 与 Topic 不一致")

    _require_string(data, "message_id")
    _require_string(data, "session_id")
    _require_int(data, "timestamp_unix_us", 1)
    if data.get("site_id") != site_id or data.get("device_id") != device_id:
        raise ProtocolError("Payload 中的 site_id/device_id 与 Topic 不一致")


def _validate_message(data: dict[str, Any], expected_type: str) -> None:
    if expected_type == "availability":
        _require_choice(data, "state", {"online", "offline"})
        return

    if expected_type == "heartbeat":
        _require_int(data, "sequence")
        _require_int(data, "uptime_us")
        return

    if expected_type == "device_status":
        _require_string(data, "software_version", 32)
        _require_choice(data, "lane_mode", {"entry", "exit"})
        _require_choice(data, "business_state", BUSINESS_STATES)

        gate = data.get("gate")
        components = data.get("components")
        if not isinstance(gate, dict) or not isinstance(components, dict):
            raise ProtocolError("gate 和 components 必须是对象")
        _require_choice(gate, "mode", {"dry_run", "real"})
        _require_choice(gate, "state", GATE_STATES)
        for component in ("camera", "recognizer", "database"):
            _require_choice(components, component, COMPONENT_STATES)
        _validate_spaces(data.get("spaces"))
        return

    _require_int(data, "operation_id", 1)
    _require_string(data, "plate_number", 31)
    _require_string(data, "plate_color", 31)
    _require_choice(data, "passage_source", PASSAGE_SOURCES)
    if data.get("result") != "committed":
        raise ProtocolError("车辆事件 result 必须是 committed")

    confidence = data.get("detect_confidence")
    if (
        not isinstance(confidence, (int, float))
        or isinstance(confidence, bool)
        or not math.isfinite(confidence)
        or confidence < 0.0
        or confidence > 1.0
    ):
        raise ProtocolError("detect_confidence 必须是 0～1 的有限数值")

    amount = _require_int(data, "amount_cents")
    expected_lane = "entry" if expected_type == "vehicle_entry" else "exit"
    if data.get("lane_mode") != expected_lane:
        raise ProtocolError("车辆事件 lane_mode 与 Topic 不一致")
    if expected_type == "vehicle_entry" and amount != 0:
        raise ProtocolError("入场事件 amount_cents 必须为 0")
    _validate_spaces(data.get("spaces"))


def decode_message(topic: str, payload: bytes) -> DecodedMessage:
    site_id, device_id, expected_type = _parse_topic(topic)
    if not isinstance(payload, bytes) or not payload:
        raise ProtocolError("Payload 必须是非空字节串")

    normalized = payload.rstrip(b"\x00")
    stripped_trailing_nul = len(normalized) != len(payload)
    if not normalized or b"\x00" in normalized:
        raise ProtocolError("Payload 含有非法 NUL 字节")

    try:
        text = normalized.decode("utf-8", errors="strict")
        data = json.loads(text)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError("Payload 不是有效 UTF-8 JSON") from exc
    if not isinstance(data, dict):
        raise ProtocolError("Payload 顶层必须是对象")

    _validate_common(data, site_id, device_id, expected_type)
    _validate_message(data, expected_type)
    return DecodedMessage(
        kind=expected_type,
        site_id=site_id,
        device_id=device_id,
        payload=data,
        stripped_trailing_nul=stripped_trailing_nul,
    )

from __future__ import annotations

import json
from typing import Any


def base_payload(message_type: str, message_id: str) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "type": message_type,
        "message_id": message_id,
        "timestamp_unix_us": 1_700_000_000_000_000,
        "site_id": "lot-a",
        "device_id": "gate01",
        "session_id": "session-7",
    }


def spaces() -> dict[str, int]:
    return {
        "total": 100,
        "occupied": 20,
        "pending_entries": 1,
        "available": 79,
    }


def status_payload(message_id: str = "status-1") -> dict[str, Any]:
    payload = base_payload("device_status", message_id)
    payload.update(
        {
            "software_version": "1.0.0",
            "lane_mode": "entry",
            "business_state": "idle",
            "gate": {"mode": "dry_run", "state": "closed"},
            "components": {
                "camera": "ok",
                "recognizer": "ok",
                "database": "ok",
            },
            "spaces": spaces(),
        }
    )
    return payload


def event_payload(event_type: str, message_id: str, operation_id: int) -> dict[str, Any]:
    payload = base_payload(f"vehicle_{event_type}", message_id)
    payload.update(
        {
            "operation_id": operation_id,
            "lane_mode": event_type,
            "plate_number": "浙A66666",
            "plate_color": "蓝色",
            "detect_confidence": 0.95,
            "amount_cents": 0 if event_type == "entry" else 50,
            "passage_source": "vision_timeout",
            "result": "committed",
            "spaces": spaces(),
        }
    )
    return payload


def encode(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()

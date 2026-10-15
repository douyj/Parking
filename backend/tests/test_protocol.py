from __future__ import annotations

import pytest

from app.protocol import ProtocolError, decode_message
from tests.helpers import base_payload, encode, event_payload, status_payload


def test_decodes_status_and_checks_topic_identity() -> None:
    decoded = decode_message(
        "parking/v1/lot-a/gate01/status", encode(status_payload())
    )
    assert decoded.kind == "device_status"
    assert decoded.site_id == "lot-a"
    assert decoded.device_id == "gate01"

    payload = status_payload()
    payload["device_id"] = "other"
    with pytest.raises(ProtocolError, match="Topic"):
        decode_message("parking/v1/lot-a/gate01/status", encode(payload))


def test_accepts_only_trailing_nul_compatibility() -> None:
    heartbeat = base_payload("heartbeat", "heartbeat-1")
    heartbeat.update({"sequence": 1, "uptime_us": 2_000_000})

    decoded = decode_message(
        "parking/v1/lot-a/gate01/heartbeat", encode(heartbeat) + b"\x00"
    )
    assert decoded.stripped_trailing_nul is True

    with pytest.raises(ProtocolError, match="NUL"):
        decode_message(
            "parking/v1/lot-a/gate01/heartbeat",
            b'{"schema_version":1}\x00garbage',
        )


def test_rejects_commands_and_inconsistent_spaces() -> None:
    with pytest.raises(ProtocolError, match="不接受"):
        decode_message(
            "parking/v1/lot-a/gate01/commands/request", b"{}"
        )

    payload = status_payload()
    payload["spaces"]["available"] = 80
    with pytest.raises(ProtocolError, match="车位统计"):
        decode_message("parking/v1/lot-a/gate01/status", encode(payload))


def test_validates_committed_vehicle_events() -> None:
    payload = event_payload("entry", "entry-42", 42)
    decoded = decode_message(
        "parking/v1/lot-a/gate01/events/entry", encode(payload)
    )
    assert decoded.kind == "vehicle_entry"

    payload["amount_cents"] = 1
    with pytest.raises(ProtocolError, match="入场事件"):
        decode_message(
            "parking/v1/lot-a/gate01/events/entry", encode(payload)
        )

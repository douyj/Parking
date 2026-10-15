from __future__ import annotations

from datetime import datetime
from zoneinfo import ZoneInfo

from app.database import Database
from app.protocol import decode_message
from tests.helpers import base_payload, encode, event_payload, status_payload


def decode(topic: str, payload: dict):
    return decode_message(topic, encode(payload))


def test_ingest_snapshots_events_and_deduplication(tmp_path) -> None:
    database = Database(tmp_path / "backend.db")
    database.initialize()
    received = 1_700_000_100_000_000

    online = base_payload("availability", "online-1")
    online["state"] = "online"
    result = database.ingest(
        decode("parking/v1/lot-a/gate01/availability", online), received
    )
    assert result.inserted is True

    heartbeat = base_payload("heartbeat", "heartbeat-1")
    heartbeat.update({"sequence": 7, "uptime_us": 5_000_000})
    database.ingest(
        decode("parking/v1/lot-a/gate01/heartbeat", heartbeat), received + 1
    )
    database.ingest(
        decode("parking/v1/lot-a/gate01/status", status_payload()),
        received + 2,
    )

    entry = event_payload("entry", "entry-42", 42)
    event = decode("parking/v1/lot-a/gate01/events/entry", entry)
    first = database.ingest(event, received + 3)
    duplicate = database.ingest(event, received + 4)
    assert first.inserted is True
    assert duplicate.duplicate is True

    devices = database.list_devices()
    assert len(devices) == 1
    assert devices[0]["availability"] == "online"
    assert devices[0]["heartbeat_sequence"] == 7
    assert devices[0]["available_spaces"] == 79

    events = database.list_events(plate="A666", event_type="entry")
    assert events["total"] == 1
    assert events["items"][0]["operation_id"] == 42


def test_offline_uses_receive_time_and_heartbeat_has_no_history(tmp_path) -> None:
    database = Database(tmp_path / "backend.db")
    database.initialize()

    heartbeat = base_payload("heartbeat", "heartbeat-old-id")
    heartbeat.update({"sequence": 1, "uptime_us": 10})
    topic = "parking/v1/lot-a/gate01/heartbeat"
    database.ingest(decode(topic, heartbeat), 2_000_000)
    heartbeat["sequence"] = 2
    database.ingest(decode(topic, heartbeat), 3_000_000)

    offline = base_payload("availability", "offline-1")
    offline["state"] = "offline"
    database.ingest(
        decode("parking/v1/lot-a/gate01/availability", offline), 4_000_000
    )

    device = database.list_devices()[0]
    assert device["heartbeat_sequence"] == 2
    assert device["availability"] == "offline"
    assert device["availability_received_at_us"] == 4_000_000

    with database._connect() as connection:
        count = connection.execute(
            "SELECT COUNT(*) FROM processed_messages"
        ).fetchone()[0]
    assert count == 1


def test_overview_uses_one_latest_snapshot_per_site(tmp_path) -> None:
    database = Database(tmp_path / "backend.db")
    database.initialize()
    received = int(
        datetime(2026, 10, 15, 10, 0, tzinfo=ZoneInfo("Asia/Shanghai")).timestamp()
        * 1_000_000
    )

    first = status_payload("status-gate01")
    database.ingest(
        decode("parking/v1/lot-a/gate01/status", first), received
    )

    second = status_payload("status-gate02")
    second["device_id"] = "gate02"
    second["spaces"] = {
        "total": 100,
        "occupied": 21,
        "pending_entries": 0,
        "available": 79,
    }
    database.ingest(
        decode("parking/v1/lot-a/gate02/status", second), received + 1
    )

    result = database.overview(received - 1, received + 100)
    assert result["device_count"] == 2
    assert result["site_count"] == 1
    assert result["spaces"]["occupied"] == 21


def test_payment_lifecycle_is_idempotent_and_can_expire(tmp_path) -> None:
    database = Database(tmp_path / "backend.db")
    database.initialize()

    payment = database.create_payment(
        token="payment-token-0001",
        site_id="lot-a",
        device_id="gate01",
        operation_id=7,
        plate_number="浙A66666",
        amount_cents=50,
        now_us=1_000_000,
        ttl_seconds=120,
    )
    duplicate = database.create_payment(
        token="payment-token-ignored",
        site_id="lot-a",
        device_id="gate01",
        operation_id=7,
        plate_number="浙A66666",
        amount_cents=50,
        now_us=2_000_000,
        ttl_seconds=120,
    )
    assert payment["status"] == "pending"
    assert duplicate["token"] == payment["token"]

    paid = database.pay(payment["token"], 3_000_000)
    assert paid is not None
    assert paid["status"] == "paid"
    assert database.pay(payment["token"], 4_000_000)["status"] == "paid"

    expiring = database.create_payment(
        token="payment-token-0002",
        site_id="lot-a",
        device_id="gate01",
        operation_id=8,
        plate_number="浙A88888",
        amount_cents=100,
        now_us=5_000_000,
        ttl_seconds=30,
    )
    expired = database.get_payment(expiring["token"], 35_000_000)
    assert expired is not None
    assert expired["status"] == "expired"

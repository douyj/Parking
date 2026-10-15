from __future__ import annotations

from pathlib import Path

from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app
from app.protocol import decode_message
from tests.helpers import encode, event_payload, status_payload


def settings(database_path: Path) -> Settings:
    return Settings(
        database_path=database_path,
        mqtt_host="127.0.0.1",
        mqtt_port=1883,
        mqtt_username=None,
        mqtt_password=None,
        mqtt_ca_file=None,
        mqtt_topic="parking/v1/+/+/#",
        mqtt_client_id="parking-backend-test",
        timezone="Asia/Shanghai",
    )


def test_read_only_api_and_static_page(tmp_path) -> None:
    app = create_app(settings(tmp_path / "api.db"), start_mqtt=False)
    with TestClient(app) as client:
        database = app.state.database
        received = 1_792_029_600_000_000
        database.ingest(
            decode_message(
                "parking/v1/lot-a/gate01/status", encode(status_payload())
            ),
            received,
        )
        database.ingest(
            decode_message(
                "parking/v1/lot-a/gate01/events/exit",
                encode(event_payload("exit", "exit-43", 43)),
            ),
            received + 1,
        )

        health = client.get("/api/health")
        assert health.status_code == 200
        assert health.json()["mqtt"]["disabled"] is True

        devices = client.get("/api/devices").json()["items"]
        assert devices[0]["business_state"] == "idle"

        events = client.get("/api/events", params={"plate": "浙A"}).json()
        assert events["total"] == 1
        assert events["items"][0]["amount_cents"] == 50

        assert client.post("/api/events").status_code == 405
        page = client.get("/")
        assert page.status_code == 200
        assert "停车场运行总览" in page.text

from __future__ import annotations

import json
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .protocol import DecodedMessage


SCHEMA = """
CREATE TABLE IF NOT EXISTS schema_version (
    id INTEGER PRIMARY KEY CHECK(id = 1),
    version INTEGER NOT NULL
);

INSERT OR IGNORE INTO schema_version(id, version) VALUES(1, 1);

CREATE TABLE IF NOT EXISTS devices (
    site_id TEXT NOT NULL,
    device_id TEXT NOT NULL,
    availability TEXT NOT NULL DEFAULT 'unknown'
        CHECK(availability IN ('unknown', 'online', 'offline')),
    availability_session_id TEXT,
    availability_received_at_us INTEGER,
    last_seen_at_us INTEGER,
    heartbeat_session_id TEXT,
    heartbeat_sequence INTEGER,
    heartbeat_uptime_us INTEGER,
    heartbeat_received_at_us INTEGER,
    software_version TEXT,
    lane_mode TEXT,
    business_state TEXT,
    gate_mode TEXT,
    gate_state TEXT,
    camera_state TEXT,
    recognizer_state TEXT,
    database_state TEXT,
    total_spaces INTEGER,
    occupied_spaces INTEGER,
    pending_entries INTEGER,
    available_spaces INTEGER,
    status_received_at_us INTEGER,
    updated_at_us INTEGER NOT NULL,
    PRIMARY KEY(site_id, device_id)
);

CREATE TABLE IF NOT EXISTS processed_messages (
    site_id TEXT NOT NULL,
    device_id TEXT NOT NULL,
    message_id TEXT NOT NULL,
    message_type TEXT NOT NULL,
    received_at_us INTEGER NOT NULL,
    PRIMARY KEY(site_id, device_id, message_id)
);

CREATE TABLE IF NOT EXISTS vehicle_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    site_id TEXT NOT NULL,
    device_id TEXT NOT NULL,
    message_id TEXT NOT NULL,
    session_id TEXT NOT NULL,
    event_type TEXT NOT NULL CHECK(event_type IN ('entry', 'exit')),
    operation_id INTEGER NOT NULL,
    plate_number TEXT NOT NULL,
    plate_color TEXT NOT NULL,
    detect_confidence REAL NOT NULL,
    amount_cents INTEGER NOT NULL,
    passage_source TEXT NOT NULL,
    message_timestamp_us INTEGER NOT NULL,
    received_at_us INTEGER NOT NULL,
    total_spaces INTEGER NOT NULL,
    occupied_spaces INTEGER NOT NULL,
    pending_entries INTEGER NOT NULL,
    available_spaces INTEGER NOT NULL,
    raw_json TEXT NOT NULL,
    UNIQUE(site_id, device_id, message_id),
    UNIQUE(site_id, device_id, event_type, operation_id)
);

CREATE INDEX IF NOT EXISTS idx_vehicle_events_received
ON vehicle_events(received_at_us DESC);

CREATE INDEX IF NOT EXISTS idx_vehicle_events_plate
ON vehicle_events(plate_number, received_at_us DESC);

CREATE TABLE IF NOT EXISTS payments (
    token TEXT PRIMARY KEY,
    site_id TEXT NOT NULL,
    device_id TEXT NOT NULL,
    operation_id INTEGER NOT NULL,
    plate_number TEXT NOT NULL,
    amount_cents INTEGER NOT NULL CHECK(amount_cents > 0),
    status TEXT NOT NULL CHECK(status IN ('pending', 'paid', 'expired')),
    created_at_us INTEGER NOT NULL,
    expires_at_us INTEGER NOT NULL,
    paid_at_us INTEGER,
    UNIQUE(site_id, device_id, operation_id)
);

CREATE INDEX IF NOT EXISTS idx_payments_status
ON payments(status, created_at_us DESC);
"""


@dataclass(frozen=True)
class IngestResult:
    inserted: bool
    duplicate: bool
    kind: str
    site_id: str
    device_id: str


class Database:
    def __init__(self, path: Path | str):
        self.path = Path(path)

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path, timeout=5.0)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA busy_timeout = 5000")
        return connection

    def initialize(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as connection:
            connection.execute("PRAGMA journal_mode = WAL")
            connection.execute("PRAGMA synchronous = FULL")
            connection.executescript(SCHEMA)
            version = connection.execute(
                "SELECT version FROM schema_version WHERE id = 1"
            ).fetchone()
            if version is None or version["version"] not in (1, 2):
                raise RuntimeError("不支持的后台数据库 schema 版本")
            if version["version"] == 1:
                connection.execute(
                    "UPDATE schema_version SET version = 2 WHERE id = 1"
                )

    @staticmethod
    def _ensure_device(
        connection: sqlite3.Connection,
        site_id: str,
        device_id: str,
        received_at_us: int,
    ) -> None:
        connection.execute(
            """
            INSERT INTO devices(site_id, device_id, updated_at_us)
            VALUES(?, ?, ?)
            ON CONFLICT(site_id, device_id) DO NOTHING
            """,
            (site_id, device_id, received_at_us),
        )

    @staticmethod
    def _claim_message(
        connection: sqlite3.Connection,
        message: DecodedMessage,
        received_at_us: int,
    ) -> bool:
        cursor = connection.execute(
            """
            INSERT OR IGNORE INTO processed_messages(
                site_id, device_id, message_id, message_type, received_at_us
            ) VALUES(?, ?, ?, ?, ?)
            """,
            (
                message.site_id,
                message.device_id,
                message.payload["message_id"],
                message.kind,
                received_at_us,
            ),
        )
        return cursor.rowcount == 1

    def ingest(
        self, message: DecodedMessage, received_at_us: int
    ) -> IngestResult:
        if received_at_us <= 0:
            raise ValueError("received_at_us 必须大于 0")

        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            self._ensure_device(
                connection, message.site_id, message.device_id, received_at_us
            )

            if message.kind != "heartbeat" and not self._claim_message(
                connection, message, received_at_us
            ):
                return IngestResult(
                    inserted=False,
                    duplicate=True,
                    kind=message.kind,
                    site_id=message.site_id,
                    device_id=message.device_id,
                )

            if message.kind == "availability":
                self._ingest_availability(connection, message, received_at_us)
            elif message.kind == "heartbeat":
                self._ingest_heartbeat(connection, message, received_at_us)
            elif message.kind == "device_status":
                self._ingest_status(connection, message, received_at_us)
            else:
                self._ingest_vehicle_event(connection, message, received_at_us)

        return IngestResult(
            inserted=True,
            duplicate=False,
            kind=message.kind,
            site_id=message.site_id,
            device_id=message.device_id,
        )

    @staticmethod
    def _ingest_availability(
        connection: sqlite3.Connection,
        message: DecodedMessage,
        received_at_us: int,
    ) -> None:
        payload = message.payload
        connection.execute(
            """
            UPDATE devices SET
                availability = ?,
                availability_session_id = ?,
                availability_received_at_us = ?,
                last_seen_at_us = ?,
                updated_at_us = ?
            WHERE site_id = ? AND device_id = ?
            """,
            (
                payload["state"],
                payload["session_id"],
                received_at_us,
                received_at_us,
                received_at_us,
                message.site_id,
                message.device_id,
            ),
        )

    @staticmethod
    def _ingest_heartbeat(
        connection: sqlite3.Connection,
        message: DecodedMessage,
        received_at_us: int,
    ) -> None:
        payload = message.payload
        connection.execute(
            """
            UPDATE devices SET
                availability = 'online',
                heartbeat_session_id = ?,
                heartbeat_sequence = ?,
                heartbeat_uptime_us = ?,
                heartbeat_received_at_us = ?,
                last_seen_at_us = ?,
                updated_at_us = ?
            WHERE site_id = ? AND device_id = ?
            """,
            (
                payload["session_id"],
                payload["sequence"],
                payload["uptime_us"],
                received_at_us,
                received_at_us,
                received_at_us,
                message.site_id,
                message.device_id,
            ),
        )

    @staticmethod
    def _ingest_status(
        connection: sqlite3.Connection,
        message: DecodedMessage,
        received_at_us: int,
    ) -> None:
        payload = message.payload
        gate = payload["gate"]
        components = payload["components"]
        spaces = payload["spaces"]
        connection.execute(
            """
            UPDATE devices SET
                software_version = ?,
                lane_mode = ?,
                business_state = ?,
                gate_mode = ?,
                gate_state = ?,
                camera_state = ?,
                recognizer_state = ?,
                database_state = ?,
                total_spaces = ?,
                occupied_spaces = ?,
                pending_entries = ?,
                available_spaces = ?,
                status_received_at_us = ?,
                last_seen_at_us = ?,
                updated_at_us = ?
            WHERE site_id = ? AND device_id = ?
            """,
            (
                payload["software_version"],
                payload["lane_mode"],
                payload["business_state"],
                gate["mode"],
                gate["state"],
                components["camera"],
                components["recognizer"],
                components["database"],
                spaces["total"],
                spaces["occupied"],
                spaces["pending_entries"],
                spaces["available"],
                received_at_us,
                received_at_us,
                received_at_us,
                message.site_id,
                message.device_id,
            ),
        )

    @staticmethod
    def _ingest_vehicle_event(
        connection: sqlite3.Connection,
        message: DecodedMessage,
        received_at_us: int,
    ) -> None:
        payload = message.payload
        spaces = payload["spaces"]
        event_type = "entry" if message.kind == "vehicle_entry" else "exit"
        connection.execute(
            """
            INSERT INTO vehicle_events(
                site_id, device_id, message_id, session_id, event_type,
                operation_id, plate_number, plate_color, detect_confidence,
                amount_cents, passage_source, message_timestamp_us,
                received_at_us, total_spaces, occupied_spaces,
                pending_entries, available_spaces, raw_json
            ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                message.site_id,
                message.device_id,
                payload["message_id"],
                payload["session_id"],
                event_type,
                payload["operation_id"],
                payload["plate_number"],
                payload["plate_color"],
                payload["detect_confidence"],
                payload["amount_cents"],
                payload["passage_source"],
                payload["timestamp_unix_us"],
                received_at_us,
                spaces["total"],
                spaces["occupied"],
                spaces["pending_entries"],
                spaces["available"],
                json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
            ),
        )
        connection.execute(
            """
            UPDATE devices SET last_seen_at_us = ?, updated_at_us = ?
            WHERE site_id = ? AND device_id = ?
            """,
            (received_at_us, received_at_us, message.site_id, message.device_id),
        )

    def list_devices(self) -> list[dict[str, Any]]:
        with self._connect() as connection:
            rows = connection.execute(
                """
                SELECT * FROM devices
                ORDER BY site_id, device_id
                """
            ).fetchall()
        return [dict(row) for row in rows]

    @staticmethod
    def _expire_payment(
        connection: sqlite3.Connection, token: str, now_us: int
    ) -> None:
        connection.execute(
            """
            UPDATE payments SET status = 'expired'
            WHERE token = ? AND status = 'pending' AND expires_at_us <= ?
            """,
            (token, now_us),
        )

    def create_payment(
        self,
        *,
        token: str,
        site_id: str,
        device_id: str,
        operation_id: int,
        plate_number: str,
        amount_cents: int,
        now_us: int,
        ttl_seconds: int,
    ) -> dict[str, Any]:
        if not token or operation_id <= 0 or amount_cents <= 0 or now_us <= 0:
            raise ValueError("支付订单参数无效")
        expires_at_us = now_us + ttl_seconds * 1_000_000

        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            existing = connection.execute(
                """
                SELECT * FROM payments
                WHERE site_id = ? AND device_id = ? AND operation_id = ?
                """,
                (site_id, device_id, operation_id),
            ).fetchone()
            if existing is not None:
                if (
                    existing["plate_number"] != plate_number
                    or existing["amount_cents"] != amount_cents
                ):
                    raise ValueError("同一 operation_id 的支付信息不一致")
                self._expire_payment(connection, existing["token"], now_us)
                existing = connection.execute(
                    "SELECT * FROM payments WHERE token = ?",
                    (existing["token"],),
                ).fetchone()
                assert existing is not None
                if existing["status"] != "expired":
                    return dict(existing)
                connection.execute(
                    """
                    UPDATE payments SET token = ?, status = 'pending',
                        created_at_us = ?, expires_at_us = ?, paid_at_us = NULL
                    WHERE site_id = ? AND device_id = ? AND operation_id = ?
                    """,
                    (
                        token,
                        now_us,
                        expires_at_us,
                        site_id,
                        device_id,
                        operation_id,
                    ),
                )
            else:
                connection.execute(
                    """
                    INSERT INTO payments(
                        token, site_id, device_id, operation_id,
                        plate_number, amount_cents, status,
                        created_at_us, expires_at_us
                    ) VALUES(?, ?, ?, ?, ?, ?, 'pending', ?, ?)
                    """,
                    (
                        token,
                        site_id,
                        device_id,
                        operation_id,
                        plate_number,
                        amount_cents,
                        now_us,
                        expires_at_us,
                    ),
                )

            row = connection.execute(
                "SELECT * FROM payments WHERE token = ?", (token,)
            ).fetchone()
        assert row is not None
        return dict(row)

    def get_payment(self, token: str, now_us: int) -> dict[str, Any] | None:
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            self._expire_payment(connection, token, now_us)
            row = connection.execute(
                "SELECT * FROM payments WHERE token = ?", (token,)
            ).fetchone()
        return dict(row) if row is not None else None

    def pay(self, token: str, now_us: int) -> dict[str, Any] | None:
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            self._expire_payment(connection, token, now_us)
            connection.execute(
                """
                UPDATE payments SET status = 'paid', paid_at_us = ?
                WHERE token = ? AND status = 'pending'
                """,
                (now_us, token),
            )
            row = connection.execute(
                "SELECT * FROM payments WHERE token = ?", (token,)
            ).fetchone()
        return dict(row) if row is not None else None

    def overview(self, day_start_us: int, day_end_us: int) -> dict[str, Any]:
        devices = self.list_devices()
        latest_by_site: dict[str, dict[str, Any]] = {}
        for device in devices:
            if device["status_received_at_us"] is None:
                continue
            current = latest_by_site.get(device["site_id"])
            if current is None or device["status_received_at_us"] > current[
                "status_received_at_us"
            ]:
                latest_by_site[device["site_id"]] = device

        spaces = {
            "total": sum(item["total_spaces"] or 0 for item in latest_by_site.values()),
            "occupied": sum(
                item["occupied_spaces"] or 0 for item in latest_by_site.values()
            ),
            "pending_entries": sum(
                item["pending_entries"] or 0 for item in latest_by_site.values()
            ),
            "available": sum(
                item["available_spaces"] or 0 for item in latest_by_site.values()
            ),
        }

        with self._connect() as connection:
            event_rows = connection.execute(
                """
                SELECT event_type, COUNT(*) AS count
                FROM vehicle_events
                WHERE received_at_us >= ? AND received_at_us < ?
                GROUP BY event_type
                """,
                (day_start_us, day_end_us),
            ).fetchall()
        event_counts = {row["event_type"]: row["count"] for row in event_rows}
        return {
            "spaces": spaces,
            "site_count": len(latest_by_site),
            "device_count": len(devices),
            "online_device_count": sum(
                device["availability"] == "online" for device in devices
            ),
            "today": {
                "entries": event_counts.get("entry", 0),
                "exits": event_counts.get("exit", 0),
            },
        }

    def list_events(
        self,
        *,
        plate: str | None = None,
        event_type: str | None = None,
        start_us: int | None = None,
        end_us: int | None = None,
        limit: int = 50,
        offset: int = 0,
    ) -> dict[str, Any]:
        clauses: list[str] = []
        parameters: list[Any] = []
        if plate:
            clauses.append("plate_number LIKE ?")
            parameters.append(f"%{plate}%")
        if event_type:
            clauses.append("event_type = ?")
            parameters.append(event_type)
        if start_us is not None:
            clauses.append("received_at_us >= ?")
            parameters.append(start_us)
        if end_us is not None:
            clauses.append("received_at_us < ?")
            parameters.append(end_us)

        where = " WHERE " + " AND ".join(clauses) if clauses else ""
        with self._connect() as connection:
            total = connection.execute(
                f"SELECT COUNT(*) AS count FROM vehicle_events{where}",
                parameters,
            ).fetchone()["count"]
            rows = connection.execute(
                f"""
                SELECT id, site_id, device_id, message_id, session_id,
                       event_type, operation_id, plate_number, plate_color,
                       detect_confidence, amount_cents, passage_source,
                       message_timestamp_us, received_at_us, total_spaces,
                       occupied_spaces, pending_entries, available_spaces
                FROM vehicle_events{where}
                ORDER BY received_at_us DESC, id DESC
                LIMIT ? OFFSET ?
                """,
                [*parameters, limit, offset],
            ).fetchall()
        return {"items": [dict(row) for row in rows], "total": total}

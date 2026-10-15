from __future__ import annotations

import asyncio
import json
import logging
from contextlib import asynccontextmanager
from datetime import date, datetime, time, timedelta
from typing import AsyncIterator
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import StreamingResponse
from fastapi.staticfiles import StaticFiles

from .config import BACKEND_DIR, Settings
from .database import Database
from .events import UpdateHub
from .mqtt_subscriber import MqttSubscriber


LOGGER = logging.getLogger(__name__)
STATIC_DIR = BACKEND_DIR / "static"


def _unix_us(value: datetime) -> int:
    return int(value.timestamp() * 1_000_000)


def _date_range(
    timezone: ZoneInfo,
    date_from: date | None,
    date_to: date | None,
) -> tuple[int | None, int | None]:
    if date_from is not None and date_to is not None and date_from > date_to:
        raise HTTPException(status_code=422, detail="date_from 不能晚于 date_to")

    start_us = None
    end_us = None
    if date_from is not None:
        start_us = _unix_us(datetime.combine(date_from, time.min, timezone))
    if date_to is not None:
        end_us = _unix_us(
            datetime.combine(date_to + timedelta(days=1), time.min, timezone)
        )
    return start_us, end_us


def create_app(
    settings: Settings | None = None,
    *,
    start_mqtt: bool = True,
) -> FastAPI:
    resolved = settings or Settings.from_env()
    try:
        timezone = ZoneInfo(resolved.timezone)
    except ZoneInfoNotFoundError as exc:
        raise ValueError(f"未知时区: {resolved.timezone}") from exc

    database = Database(resolved.database_path)
    update_hub = UpdateHub()
    subscriber = (
        MqttSubscriber(resolved, database, update_hub) if start_mqtt else None
    )

    @asynccontextmanager
    async def lifespan(application: FastAPI) -> AsyncIterator[None]:
        del application
        database.initialize()
        update_hub.bind_loop(asyncio.get_running_loop())
        if subscriber is not None:
            subscriber.start()
        try:
            yield
        finally:
            if subscriber is not None:
                subscriber.stop()

    app = FastAPI(
        title="Parking Read-only Management API",
        version="1.0.0",
        lifespan=lifespan,
    )
    app.state.database = database
    app.state.update_hub = update_hub
    app.state.subscriber = subscriber

    @app.get("/api/health")
    def health() -> dict:
        mqtt_state = subscriber.snapshot() if subscriber is not None else {
            "connected": False,
            "disabled": True,
        }
        return {"status": "ok", "mqtt": mqtt_state}

    @app.get("/api/overview")
    def overview() -> dict:
        today = datetime.now(timezone).date()
        start_us, end_us = _date_range(timezone, today, today)
        assert start_us is not None and end_us is not None
        result = database.overview(start_us, end_us)
        result["date"] = today.isoformat()
        return result

    @app.get("/api/devices")
    def devices() -> dict:
        return {"items": database.list_devices()}

    @app.get("/api/events")
    def events(
        plate: str | None = Query(default=None, max_length=31),
        event_type: str | None = Query(default=None, pattern="^(entry|exit)$"),
        date_from: date | None = None,
        date_to: date | None = None,
        limit: int = Query(default=50, ge=1, le=200),
        offset: int = Query(default=0, ge=0),
    ) -> dict:
        start_us, end_us = _date_range(timezone, date_from, date_to)
        result = database.list_events(
            plate=plate.strip() if plate and plate.strip() else None,
            event_type=event_type,
            start_us=start_us,
            end_us=end_us,
            limit=limit,
            offset=offset,
        )
        result.update({"limit": limit, "offset": offset})
        return result

    @app.get("/api/stream")
    async def stream(request: Request) -> StreamingResponse:
        queue = update_hub.subscribe()

        async def generate() -> AsyncIterator[str]:
            yield "retry: 3000\n\n"
            try:
                while not await request.is_disconnected():
                    try:
                        event = await asyncio.wait_for(queue.get(), timeout=15.0)
                    except TimeoutError:
                        yield ": keepalive\n\n"
                        continue
                    payload = json.dumps(event, ensure_ascii=False)
                    yield f"event: update\ndata: {payload}\n\n"
            finally:
                update_hub.unsubscribe(queue)

        return StreamingResponse(
            generate(),
            media_type="text/event-stream",
            headers={
                "Cache-Control": "no-cache",
                "X-Accel-Buffering": "no",
            },
        )

    if not STATIC_DIR.is_dir():
        raise RuntimeError(f"静态页面目录不存在: {STATIC_DIR}")
    app.mount("/", StaticFiles(directory=STATIC_DIR, html=True), name="web")
    return app


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
app = create_app()

from __future__ import annotations

import asyncio
import html
import json
import logging
import secrets
import time as system_time
from contextlib import asynccontextmanager
from datetime import date, datetime, time, timedelta
from typing import AsyncIterator
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from fastapi import FastAPI, HTTPException, Path, Query, Request
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from .config import BACKEND_DIR, Settings
from .database import Database
from .events import UpdateHub
from .mqtt_subscriber import MqttSubscriber


LOGGER = logging.getLogger(__name__)
STATIC_DIR = BACKEND_DIR / "static"
PAYMENT_TOKEN_PATTERN = r"^[A-Za-z0-9_-]{16,64}$"


class PaymentCreate(BaseModel):
    site_id: str = Field(
        min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$"
    )
    device_id: str = Field(
        min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$"
    )
    operation_id: int = Field(ge=1)
    plate_number: str = Field(min_length=1, max_length=31)
    amount_cents: int = Field(ge=1, le=100_000_000)


def _unix_us(value: datetime) -> int:
    return int(value.timestamp() * 1_000_000)


def _now_us() -> int:
    return system_time.time_ns() // 1_000


def _payment_response(payment: dict, pay_url: str) -> dict:
    return {
        "token": payment["token"],
        "site_id": payment["site_id"],
        "device_id": payment["device_id"],
        "operation_id": payment["operation_id"],
        "plate_number": payment["plate_number"],
        "amount_cents": payment["amount_cents"],
        "status": payment["status"],
        "created_at_us": payment["created_at_us"],
        "expires_at_us": payment["expires_at_us"],
        "paid_at_us": payment["paid_at_us"],
        "pay_url": pay_url,
    }


def _payment_page(payment: dict) -> str:
    plate = html.escape(payment["plate_number"])
    amount = payment["amount_cents"] / 100
    status = payment["status"]
    if status == "paid":
        title = "支付成功"
        detail = "模拟支付已经完成，可以返回车辆等待通行。"
        action = "<div class=\"success-mark\">✓</div>"
    elif status == "expired":
        title = "订单已过期"
        detail = "请返回出口终端重新识别车牌并生成二维码。"
        action = "<div class=\"expired-mark\">!</div>"
    else:
        title = "停车费模拟支付"
        detail = "这是秋招项目演示，不会产生真实扣款。"
        action = (
            f'<form method="post" action="/pay/{payment["token"]}/confirm">'
            '<button type="submit">确认模拟支付</button></form>'
        )
    return f"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport"
content="width=device-width,initial-scale=1"><title>{title}</title>
<style>
:root{{font-family:Inter,"Microsoft YaHei",sans-serif;color:#102a43;background:#eef5f5}}
*{{box-sizing:border-box}}body{{margin:0;min-height:100vh;display:grid;place-items:center;padding:20px}}
main{{width:min(420px,100%);padding:34px 28px;border:1px solid #dce4ed;border-radius:24px;
background:white;box-shadow:0 22px 60px rgba(20,55,70,.12)}}
.brand{{margin:0 0 28px;color:#08726f;font-size:13px;font-weight:800;letter-spacing:.16em}}
h1{{margin:0 0 8px;font-size:28px}}.hint{{margin:0 0 28px;color:#66758a;line-height:1.6}}
.order{{padding:20px;border-radius:16px;background:#f4f8f8}}.row{{display:flex;justify-content:space-between;
gap:16px;margin:10px 0;color:#66758a}}.row strong{{color:#132238}}.amount{{margin:24px 0;text-align:center}}
.amount span{{display:block;color:#66758a;font-size:12px}}.amount strong{{font-size:42px;color:#0a8f8b}}
button{{width:100%;height:52px;border:0;border-radius:12px;color:white;background:#0a8f8b;
font-size:16px;font-weight:800}}.success-mark,.expired-mark{{width:72px;height:72px;margin:26px auto 0;
display:grid;place-items:center;border-radius:50%;font-size:40px;font-weight:900}}
.success-mark{{color:#16865c;background:#e5f5ed}}.expired-mark{{color:#c64b55;background:#fbeaec}}
footer{{margin-top:24px;color:#9aa6b4;text-align:center;font-size:11px}}
</style></head><body><main><p class="brand">泊云 · DEMO PAY</p><h1>{title}</h1>
<p class="hint">{detail}</p><section class="order"><div class="row"><span>车牌号码</span>
<strong>{plate}</strong></div><div class="row"><span>订单号</span>
<strong>#{payment["operation_id"]}</strong></div></section><div class="amount"><span>应付金额</span>
<strong>¥ {amount:.2f}</strong></div>{action}<footer>仅用于局域网嵌入式项目演示</footer>
</main></body></html>"""


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
        title="Parking Management and Demo Payment API",
        version="1.1.0",
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

    def payment_url(request: Request, token: str) -> str:
        base_url = resolved.public_base_url or str(request.base_url).rstrip("/")
        return f"{base_url}/pay/{token}"

    @app.post("/api/payments")
    def create_payment(payload: PaymentCreate, request: Request) -> dict:
        try:
            payment = database.create_payment(
                token=secrets.token_urlsafe(18),
                site_id=payload.site_id,
                device_id=payload.device_id,
                operation_id=payload.operation_id,
                plate_number=payload.plate_number,
                amount_cents=payload.amount_cents,
                now_us=_now_us(),
                ttl_seconds=resolved.payment_ttl_seconds,
            )
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc
        update_hub.publish_from_thread(
            {"kind": "payment", "status": payment["status"]}
        )
        return _payment_response(
            payment, payment_url(request, payment["token"])
        )

    @app.get("/api/payments/{token}")
    def get_payment(
        request: Request,
        token: str = Path(pattern=PAYMENT_TOKEN_PATTERN),
    ) -> dict:
        payment = database.get_payment(token, _now_us())
        if payment is None:
            raise HTTPException(status_code=404, detail="支付订单不存在")
        return _payment_response(payment, payment_url(request, token))

    @app.post("/api/payments/{token}/pay")
    def pay(
        request: Request,
        token: str = Path(pattern=PAYMENT_TOKEN_PATTERN),
    ) -> dict:
        payment = database.pay(token, _now_us())
        if payment is None:
            raise HTTPException(status_code=404, detail="支付订单不存在")
        if payment["status"] == "expired":
            raise HTTPException(status_code=409, detail="支付订单已过期")
        update_hub.publish_from_thread(
            {"kind": "payment", "status": payment["status"]}
        )
        return _payment_response(payment, payment_url(request, token))

    @app.get("/pay/{token}", response_class=HTMLResponse)
    def payment_page(
        token: str = Path(pattern=PAYMENT_TOKEN_PATTERN),
    ) -> HTMLResponse:
        payment = database.get_payment(token, _now_us())
        if payment is None:
            raise HTTPException(status_code=404, detail="支付订单不存在")
        return HTMLResponse(_payment_page(payment))

    @app.post("/pay/{token}/confirm", response_class=HTMLResponse)
    def confirm_payment(
        token: str = Path(pattern=PAYMENT_TOKEN_PATTERN),
    ) -> HTMLResponse:
        payment = database.pay(token, _now_us())
        if payment is None:
            raise HTTPException(status_code=404, detail="支付订单不存在")
        update_hub.publish_from_thread(
            {"kind": "payment", "status": payment["status"]}
        )
        return HTMLResponse(_payment_page(payment))

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

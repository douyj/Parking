const elements = {
    liveDot: document.querySelector("#live-dot"),
    liveLabel: document.querySelector("#live-label"),
    clock: document.querySelector("#clock"),
    today: document.querySelector("#today"),
    available: document.querySelector("#available-spaces"),
    occupied: document.querySelector("#occupied-spaces"),
    occupancyRate: document.querySelector("#occupancy-rate"),
    entries: document.querySelector("#today-entries"),
    exits: document.querySelector("#today-exits"),
    deviceSummary: document.querySelector("#device-summary"),
    deviceList: document.querySelector("#device-list"),
    eventTotal: document.querySelector("#event-total"),
    eventRows: document.querySelector("#event-rows"),
    filter: document.querySelector("#event-filter"),
    filterReset: document.querySelector("#filter-reset"),
    toast: document.querySelector("#toast"),
};

let refreshTimer = null;
let toastTimer = null;

function escapeHtml(value) {
    return String(value ?? "")
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}

function showError(message) {
    elements.toast.textContent = message;
    elements.toast.classList.add("visible");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => elements.toast.classList.remove("visible"), 4500);
}

async function getJson(url) {
    const response = await fetch(url, {headers: {Accept: "application/json"}});
    if (!response.ok) throw new Error(`请求失败：${response.status}`);
    return response.json();
}

function formatTime(unixUs) {
    if (!unixUs) return "--";
    return new Intl.DateTimeFormat("zh-CN", {
        month: "2-digit", day: "2-digit", hour: "2-digit",
        minute: "2-digit", second: "2-digit", hour12: false,
    }).format(new Date(Number(unixUs) / 1000));
}

function renderOverview(data) {
    const spaces = data.spaces;
    elements.available.textContent = spaces.available;
    elements.occupied.textContent = spaces.occupied;
    elements.entries.textContent = data.today.entries;
    elements.exits.textContent = data.today.exits;
    const rate = spaces.total ? Math.round(spaces.occupied / spaces.total * 100) : 0;
    elements.occupancyRate.textContent = `占用率 ${rate}% · 总计 ${spaces.total}`;
    elements.deviceSummary.textContent = `${data.online_device_count} / ${data.device_count} 在线`;
}

function statusLabel(value) {
    const labels = {
        idle: "空闲", checking: "业务检查", wait_payment: "等待支付",
        opening: "正在开闸", wait_passage: "等待通行", closing: "正在关闸",
        completing: "提交记录", reject_cooldown: "拒绝冷却",
        initializing: "初始化", error: "错误",
    };
    return labels[value] || value || "--";
}

function renderDevices(data) {
    const devices = data.items;
    if (!devices.length) {
        elements.deviceList.innerHTML = '<div class="empty-state">等待终端上报状态…</div>';
        return;
    }

    elements.deviceList.innerHTML = devices.map((device) => {
        const availability = device.availability || "unknown";
        const availabilityLabel = {online: "在线", offline: "离线", unknown: "未知"}[availability];
        return `
            <article class="device-card">
                <div class="device-title">
                    <strong>${escapeHtml(device.device_id)}</strong>
                    <span class="badge badge-${escapeHtml(availability)}">${availabilityLabel}</span>
                </div>
                <p class="device-site">${escapeHtml(device.site_id)} · ${escapeHtml(device.lane_mode || "未上报模式")}</p>
                <div class="device-meta">
                    <div><span>业务状态</span><strong>${escapeHtml(statusLabel(device.business_state))}</strong></div>
                    <div><span>闸门</span><strong>${escapeHtml(device.gate_state || "--")}</strong></div>
                    <div><span>最后心跳</span><strong>${escapeHtml(formatTime(device.heartbeat_received_at_us))}</strong></div>
                    <div><span>可用车位</span><strong>${device.available_spaces ?? "--"}</strong></div>
                </div>
            </article>`;
    }).join("");
}

function renderEvents(data) {
    elements.eventTotal.textContent = `${data.total} 条记录`;
    if (!data.items.length) {
        elements.eventRows.innerHTML = '<tr><td colspan="6" class="empty-cell">暂无符合条件的车辆事件</td></tr>';
        return;
    }

    elements.eventRows.innerHTML = data.items.map((event) => {
        const isEntry = event.event_type === "entry";
        return `
            <tr>
                <td>${escapeHtml(formatTime(event.received_at_us))}</td>
                <td><span class="badge badge-${isEntry ? "entry" : "exit"}">${isEntry ? "入场" : "出场"}</span></td>
                <td><strong>${escapeHtml(event.plate_number)}</strong></td>
                <td>${escapeHtml(event.plate_color)}</td>
                <td>${isEntry ? "--" : `${event.amount_cents / 100} 元`}</td>
                <td>${escapeHtml(event.site_id)} / ${escapeHtml(event.device_id)}</td>
            </tr>`;
    }).join("");
}

function eventQuery() {
    const form = new FormData(elements.filter);
    const query = new URLSearchParams();
    for (const [key, value] of form.entries()) {
        if (String(value).trim()) query.set(key, String(value).trim());
    }
    query.set("limit", "100");
    return query.toString();
}

async function refreshAll() {
    try {
        const [overview, devices, events] = await Promise.all([
            getJson("/api/overview"),
            getJson("/api/devices"),
            getJson(`/api/events?${eventQuery()}`),
        ]);
        renderOverview(overview);
        renderDevices(devices);
        renderEvents(events);
    } catch (error) {
        showError(error.message || "读取后台数据失败");
    }
}

function scheduleRefresh() {
    clearTimeout(refreshTimer);
    refreshTimer = setTimeout(refreshAll, 250);
}

function connectEvents() {
    const source = new EventSource("/api/stream");
    source.onopen = () => {
        elements.liveDot.classList.add("online");
        elements.liveLabel.textContent = "实时数据已连接";
    };
    source.addEventListener("update", scheduleRefresh);
    source.onerror = () => {
        elements.liveDot.classList.remove("online");
        elements.liveLabel.textContent = "实时连接重试中";
    };
}

function updateClock() {
    const now = new Date();
    elements.clock.textContent = new Intl.DateTimeFormat("zh-CN", {
        hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false,
    }).format(now);
    elements.today.textContent = new Intl.DateTimeFormat("zh-CN", {
        year: "numeric", month: "long", day: "numeric", weekday: "short",
    }).format(now);
}

elements.filter.addEventListener("submit", (event) => {
    event.preventDefault();
    refreshAll();
});
elements.filterReset.addEventListener("click", () => {
    elements.filter.reset();
    refreshAll();
});

updateClock();
setInterval(updateClock, 1000);
refreshAll();
connectEvents();

const $ = (id) => document.getElementById(id);
const state = {
  ws: null,
  socketUp: false,
  connected: false,
  linkState: "reconnecting",
  info: null,
  telemetry: null,
  jobs: new Map(),
  requestSeq: 0,
  reconnectDelay: 500,
  reconnectTimer: null,
  retryCount: 0,
  printTotal: 0,
  printDone: 0,
  toastTimer: null,
};
const pending = new Map();
const STEPS_PER_MM = 32;
const MAX_HEATER_SLICE_US = 2500;
function log(message, level = "info") {
  const row = document.createElement("div");
  row.className = `log-entry ${level}`;
  const time = document.createElement("time");
  time.textContent = new Date().toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
  const text = document.createElement("span");
  text.textContent = message;
  row.append(time, text);
  $("log").append(row);
  if ($("autoScroll").checked) $("log").scrollTop = $("log").scrollHeight;
  while ($("log").children.length > 400) $("log").firstElementChild.remove();
}
function toast(message, level = "info") {
  const el = $("toast");
  el.textContent = message;
  el.className = `show ${level}`;
  clearTimeout(state.toastTimer);
  state.toastTimer = setTimeout(() => (el.className = ""), 3600);
}
function send(action, extra = {}) {
  if (!state.ws || state.ws.readyState !== WebSocket.OPEN) {
    toast("Host connection is unavailable. Retrying…", "error");
    return Promise.reject(new Error("WebSocket disconnected"));
  }
  const requestId = ++state.requestSeq;
  state.ws.send(JSON.stringify({ action, ...extra, requestId }));
  return new Promise((resolve, reject) => {
    pending.set(requestId, { resolve, reject, action });
    const timeoutMs = action === "print-solid" ? 3600000 : 15000;
    setTimeout(() => {
      const p = pending.get(requestId);
      if (p) {
        pending.delete(requestId);
        p.reject(new Error("Host request timed out"));
      }
    }, timeoutMs);
  });
}
function safeSend(action, extra = {}) {
  void send(action, extra).catch((e) => toast(e.message, "error"));
}
function setConnection(c) {
  state.linkState = c.state ?? state.linkState;
  state.connected = state.linkState === "connected";
  if (Object.hasOwn(c, "info")) state.info = c.info;
  if (!state.connected) {
    state.telemetry = null;
    $("tempValue").innerHTML = "— <small>°C</small>";
    $("tempFoot").textContent =
      "Sensor not available · cutoff 70 °C · abort 75 °C · resume 50 °C";
    $("batteryValue").innerHTML = "— <small>mV</small>";
    $("batteryFoot").textContent = "Waiting for telemetry";
    $("queueValue").textContent = "—";
    $("queueCapacity").textContent = "/ — slots";
    $("queueFoot").textContent = "No active session";
    $("paperValue").textContent = "Unknown";
    $("paperFoot").textContent = "Status not reported";
    $("jobState").textContent = "Idle";
    $("jobDescription").textContent = "Connect to inspect the printer queue.";
    $("jobRing").className = "state-ring";
  }
  const led = $("statusLed");
  led.className =
    "status-led " +
    (state.connected
      ? "connected"
      : state.linkState === "reconnecting" || state.linkState === "connecting"
        ? "reconnecting"
        : "");
  $("footerLed").className =
    "footer-led " + (state.connected ? "connected" : "");
  $("connectionLabel").textContent = state.connected
    ? "Printer connected"
    : state.linkState === "disconnected"
      ? "Disconnected"
      : state.linkState === "connecting"
        ? "Opening UART…"
        : "Reconnecting to printer";
  $("portLabel").textContent = state.connected
    ? c.path || "UART"
    : "UART · AUTO";
  $("deviceState").textContent = state.linkState.toUpperCase();
  $("deviceSub").textContent = state.connected
    ? "Session established · TBP v1"
    : state.linkState === "disconnected"
      ? "Reconnect paused"
      : "Looking for USB CDC";
  $("connectBtn").textContent = state.connected ? "Connected" : "Connect";
  $("connectBtn").disabled =
    state.connected || state.linkState === "connecting";
  $("disconnectBtn").disabled = state.linkState === "disconnected";
  for (const id of [
    "printBtn",
    "feedBtn",
    "reverseBtn",
    "releaseCoilsBtn",
    "refreshStatus",
    "flushBtn",
    "estopBtn",
  ])
    $(id).disabled = !state.connected;
  const info = state.info;
  const wasHeatReady = state.heatReady;
  const requiredSensors = 0x01 | 0x02 | 0x08;
  const heatReady = !!(
    info &&
    info.burnMaxUs > 0 &&
    info.burnTickUs > 0 &&
    info.guardUs > 0 &&
    info.dutyWindowMs > 0 &&
    info.dutyMaxUs > 0 &&
    info.shiftUs > 0 &&
    info.tempLimitRaw !== 0xffff &&
    (info.battMinRaw !== 0xffff || !!(info.caps & 0x04)) &&
    (info.caps & requiredSensors) === requiredSensors
  );
  $("burnUs").max = String(
    heatReady ? Math.min(info.burnMaxUs, MAX_HEATER_SLICE_US) : 0,
  );
  $("burnUs").disabled = !heatReady;
  if (heatReady && !wasHeatReady && Number($("burnUs").value) === 0) {
    // Match the known-good width-ramp demo once firmware sensor gates permit heat.
    $("burnUs").value = String(Math.min(6000, info.burnMaxUs));
  }
  state.heatReady = heatReady;
  if (!heatReady) {
    $("burnUs").value = "0";
    $("heaterNotice").querySelector("p").textContent =
      `Heating disabled until temperature, battery and paper sensors are valid. Conservative per-slice cap: ${MAX_HEATER_SLICE_US} µs; thermal and duty safeguards remain active. Only heatless feed tests are allowed until sensor gates are valid.`;
  } else {
    $("heaterNotice").querySelector("p").textContent =
      `Conservative per-slice pulse cap ${MAX_HEATER_SLICE_US} µs; heated rows remain limited to ${info.burnMaxUs} µs total by firmware duty and thermal safeguards. Burn remains zero by default; physical validation is still required.`;
  }
  $("printBtn").textContent = "";
  const printLabel = document.createElement("span");
  printLabel.textContent = heatReady
    ? "Queue solid-black print"
    : "Queue heatless row test";
  const printIcon = document.createElement("b");
  printIcon.textContent = "↗";
  $("printBtn").append(printLabel, printIcon);
  if (state.connected) {
    $("protocolValue").textContent = `TBP v${state.info?.version ?? 1}`;
    $("buildValue").textContent = state.info?.buildId ?? "Unknown";
    $("uidValue").textContent = state.info?.uid || "—";
    $("queueCapacity").textContent = `/ ${state.info?.queueDepth ?? "—"} slots`;
  } else if (!state.info) {
    $("protocolValue").textContent = "TBP v1 · pending";
  }
}
function setTelemetry(t) {
  if (!t) return;
  state.telemetry = t;
  const caps = state.info?.caps ?? 0;
  const tempValid = !!(caps & 0x01);
  const batteryValid = !!(caps & 0x02);
  const batteryMvCalibrated = !!(caps & 0x04);
  const paperValid = !!(caps & 0x08);
  const temp =
    tempValid && t.headTempC !== 65535 && t.headTempC != null
      ? t.headTempC
      : null;
  const batt =
    batteryValid &&
    batteryMvCalibrated &&
    t.batteryMv !== 65535 &&
    t.batteryMv != null
      ? t.batteryMv
      : null;
  $("tempValue").innerHTML =
    temp == null
      ? "— <small>°C</small>"
      : `<span title="Raw ADC: ${t.headTempRaw}">${temp} <small>°C</small></span>`;
  const tempLimits = "Cutoff 70 °C · abort 75 °C · resume 50 °C";
  $("tempFoot").textContent = !tempValid
    ? `Sensor mapping not validated · ${tempLimits}`
    : t.flags & 1
      ? `Temperature gate active · ${tempLimits}`
      : temp == null
        ? `Sensor not available · ${tempLimits}`
        : `Stock table · raw ADC ${t.headTempRaw} · ${tempLimits}`;
  $("tempDot").className =
    "metric-dot " + (temp == null ? "" : t.flags & 1 ? "" : "good");
  $("batteryValue").innerHTML =
    batt == null ? "— <small>mV</small>" : `${batt} <small>mV</small>`;
  $("batteryFoot").textContent = !batteryValid
    ? "Sensor mapping not validated"
    : !batteryMvCalibrated
      ? "Voltage calibration unavailable"
      : t.batteryRaw === 65535
        ? "Battery ADC unavailable"
        : t.batteryRaw == null
          ? "Waiting for telemetry"
          : `Raw ADC ${t.batteryRaw}`;
  $("batteryDot").className = "metric-dot " + (batt == null ? "" : "good");
  $("queueValue").textContent = t.queueUsed ?? "—";
  $("queueCapacity").textContent = `/ ${t.queueDepth ?? "—"} slots`;
  $("queueFoot").textContent = t.runningId
    ? `Running command #${t.runningId}`
    : "No running command";
  $("paperValue").textContent = !paperValid
    ? "Unknown"
    : t.paper === 1
      ? "Present"
      : t.paper === 0
        ? "Absent"
        : "Unknown";
  $("paperFoot").textContent = !paperValid
    ? "Sensor mapping not validated"
    : t.paper === 1
      ? "Paper path clear"
      : t.paper === 0
        ? "Check paper roll"
        : "Sensor not reported";
  $("paperDot").className =
    "metric-dot " + (paperValid && t.paper === 1 ? "good" : "");
  const flags = t.flags ?? 0;
  const heatReady = !!(
    state.info &&
    state.info.burnMaxUs > 0 &&
    state.info.burnTickUs > 0 &&
    state.info.guardUs > 0 &&
    state.info.dutyWindowMs > 0 &&
    state.info.dutyMaxUs > 0 &&
    state.info.shiftUs > 0 &&
    state.info.tempLimitRaw !== 0xffff &&
    (state.info.battMinRaw !== 0xffff || !!(state.info.caps & 0x04)) &&
    (state.info.caps & 0x0b) === 0x0b
  );
  $("jobState").textContent =
    heatReady && flags & 8
      ? "Heating"
      : t.runningId
        ? "Printing / moving"
        : t.queueUsed
          ? "Queued"
          : "Idle";
  $("jobDescription").textContent = !heatReady
    ? `${t.queueUsed ?? 0} queued · heater disabled · sensor/limit capabilities unavailable`
    : `${t.queueUsed ?? 0} queued · ${t.queueDepth ?? 0} capacity · ${flags & 4 ? "stepper coils energised" : "stepper idle"}`;
  $("jobRing").className =
    "state-ring " + (t.runningId || t.queueUsed ? "busy" : "");
}

function updateProgress(done, total) {
  state.printDone = done;
  state.printTotal = total;
  $("progressWrap").classList.remove("hidden");
  const pct = total ? Math.round((done / total) * 100) : 0;
  $("progressPct").textContent = pct + "%";
  $("progressLabel").textContent =
    done >= total
      ? "All rows accepted by printer"
      : `Queueing row ${done} of ${total}`;
  $("progressBar").style.width = pct + "%";
  if (done >= total)
    setTimeout(() => $("progressWrap").classList.add("hidden"), 4500);
}
function handleMessage(m) {
  if (m.type === "snapshot") {
    setConnection(m);
    if (m.status) setTelemetry(m.status);
    return;
  }
  if (m.type === "connection") {
    setConnection(m);
    log(
      `${m.state}${m.path ? " · " + m.path : ""}${m.error ? " · " + m.error : ""}`,
      m.state === "connected"
        ? "success"
        : m.state === "reconnecting"
          ? "warn"
          : "info",
    );
    if (m.error && m.state === "reconnecting") state.retryCount++;
    $("retryValue").textContent = state.retryCount
      ? `${state.retryCount} reconnects`
      : "Automatic";
    return;
  }
  if (m.type === "printer") {
    if (m.info) {
      state.info = m.info;
      setConnection({
        state: state.linkState,
        path: $("portLabel").textContent,
        info: m.info,
      });
    }
    if (m.status) setTelemetry(m.status);
    return;
  }
  if (m.type === "result") {
    const p = pending.get(m.requestId);
    if (p) {
      pending.delete(m.requestId);
      if (m.error) {
        p.reject(new Error(m.error));
        log(`${p.action}: ${m.error}`, "error");
      } else {
        p.resolve(m);
        if (!["ports", "status"].includes(p.action))
          log(`${p.action}: accepted`, "success");
      }
    }
    return;
  }
  if (m.type === "toast") {
    toast(m.message, m.level);
    log(m.message, m.level);
    return;
  }
  if (m.type === "log") {
    log(m.message, m.level);
    return;
  }
  if (m.type === "ack") {
    log(`ACK #${m.id} · queue ${m.queueUsed}`, "success");
    return;
  }
  if (m.type === "job-status") {
    const names = {
      1: "queued",
      2: "running",
      3: "done",
      4: "failed",
      5: "cancelled",
      6: "progress",
    };
    log(
      `Job #${m.id}: ${names[m.state] || "state " + m.state}${m.body?.length ? " · " + m.body.map((x) => x.toString(16).padStart(2, "0")).join(" ") : ""}`,
      m.state === 4 ? "error" : m.state === 3 ? "success" : "event",
    );
    return;
  }
  if (m.type === "event") {
    log(
      `Printer event · ${m.data.map((x) => x.toString(16).padStart(2, "0")).join(" ")}`,
      "event",
    );
    return;
  }
  if (m.type === "print-progress") {
    updateProgress(m.done, m.total);
    return;
  }
  if (m.type === "command-error") {
    toast(m.message, "error");
    return;
  }
}
function scheduleSocketReconnect() {
  if (state.reconnectTimer !== null) return;
  const delay = state.reconnectDelay;
  log(`Retrying host WebSocket in ${delay} ms`, "warn");
  state.reconnectTimer = setTimeout(() => {
    state.reconnectTimer = null;
    connectSocket();
  }, delay);
  state.reconnectDelay = Math.min(8000, state.reconnectDelay * 1.7);
}
function connectSocket() {
  if (
    state.ws &&
    (state.ws.readyState === WebSocket.CONNECTING ||
      state.ws.readyState === WebSocket.OPEN)
  )
    return;
  const scheme = location.protocol === "https:" ? "wss:" : "ws:";
  let ws;
  try {
    log(`Connecting to host WebSocket at ${scheme}//${location.host}/ws`);
    ws = new WebSocket(`${scheme}//${location.host}/ws`);
  } catch (err) {
    log(`Could not create host WebSocket: ${err}`, "error");
    scheduleSocketReconnect();
    return;
  }
  state.ws = ws;
  ws.onopen = () => {
    if (state.reconnectTimer !== null) clearTimeout(state.reconnectTimer);
    state.reconnectTimer = null;
    state.socketUp = true;
    state.reconnectDelay = 500;
    log("Host WebSocket connected", "success");
    void send("ports")
      .then((r) => renderPorts(r.ports || []))
      .catch(() => {});
    safeSend("connect", { path: $("portSelect").value });
  };
  ws.onmessage = (e) => {
    try {
      handleMessage(JSON.parse(e.data));
    } catch (err) {
      log("Invalid host message: " + err, "error");
    }
  };
  ws.onerror = () => {
    log("Host WebSocket connection error", "warn");
  };
  ws.onclose = (event) => {
    if (state.ws !== ws) return;
    state.socketUp = false;
    state.ws = null;
    setConnection({ state: "reconnecting" });
    log(
      `Host WebSocket closed (code ${event.code}${event.reason ? `: ${event.reason}` : ""})`,
      "warn",
    );
    scheduleSocketReconnect();
  };
}
function renderPorts(ports) {
  const select = $("portSelect");
  const old = select.value;
  select.replaceChildren(new Option("Auto-detect UART", ""));
  for (const p of ports) {
    const label = [
      p.path,
      p.manufacturer,
      p.vendorId && p.productId ? `${p.vendorId}:${p.productId}` : "",
    ]
      .filter(Boolean)
      .join(" · ");
    select.add(new Option(label, p.path));
  }
  if (ports.some((p) => p.path === old)) select.value = old;
}
$("refreshPorts").onclick = () =>
  void send("ports")
    .then((r) => {
      renderPorts(r.ports || []);
      toast(`Found ${(r.ports || []).length} UART device(s)`);
    })
    .catch((e) => toast(e.message, "error"));
$("connectBtn").onclick = () =>
  safeSend("connect", { path: $("portSelect").value });
$("disconnectBtn").onclick = () => safeSend("disconnect");
$("refreshStatus").onclick = () => safeSend("status");
$("flushBtn").onclick = () => {
  if (confirm("Flush queued jobs and stop at the next step boundary?"))
    safeSend("flush");
};
$("estopBtn").onclick = () => {
  if (confirm("Request stop after the current firmware operation completes?"))
    safeSend("estop");
};
$("clearLog").onclick = () => $("log").replaceChildren();
$("moveMm").oninput = () => {
  $("moveMmReadout").textContent = "+" + $("moveMm").value;
};
function updateEffectiveSpeed(inputId, outputId) {
  const requested = Number($(inputId).value);
  const output = $(outputId);
  if (!Number.isFinite(requested) || requested <= 0) {
    output.textContent = "Effective: — mm/s";
    return;
  }
  const periodUs = Math.round(1e6 / (requested * STEPS_PER_MM));
  if (periodUs < 1000 || periodUs > 65000) {
    output.textContent = "Effective: outside firmware range";
    return;
  }
  const effectivePeriodUs = Math.ceil(periodUs / 1000) * 1000;
  const effectiveSpeed = 1e6 / effectivePeriodUs / STEPS_PER_MM;
  output.textContent = `Effective: ${effectiveSpeed.toFixed(2)} mm/s`;
}
$("moveSpeed").oninput = () =>
  updateEffectiveSpeed("moveSpeed", "moveSpeedEffective");
updateEffectiveSpeed("moveSpeed", "moveSpeedEffective");
$("feedBtn").onclick = () =>
  safeSend("move", {
    distanceMm: Number($("moveMm").value),
    speedMmS: Number($("moveSpeed").value),
    releaseAfter: $("coilMode").value === "release",
  });
$("reverseBtn").onclick = () =>
  safeSend("move", {
    distanceMm: -Number($("moveMm").value),
    speedMmS: Number($("moveSpeed").value),
    releaseAfter: $("coilMode").value === "release",
  });
$("releaseCoilsBtn").onclick = () => safeSend("coils", { enabled: false });
$("coilMode").onchange = () => {
  log("Stepper coil policy: " + $("coilMode").value, "info");
};
$("printBtn").onclick = async () => {
  const rows = Number($("rowCount").value),
    burnUs = Number($("burnUs").value);
  if (
    !Number.isInteger(rows) ||
    rows < 1 ||
    rows > 500 ||
    burnUs < 0 ||
    burnUs > Number($("burnUs").max)
  ) {
    toast("Check print settings and allowed ranges", "error");
    return;
  }
  if (
    !confirm(
      `Queue ${rows} full-width black rows using four print events per row, one feed half-step per event (four steps per row), a fixed 15 ms step tick, and ${burnUs} µs heat per event? Heater temperature, sensor and duty safeguards remain active.`,
    )
  )
    return;
  $("printBtn").disabled = true;
  updateProgress(0, rows);
  try {
    const r = await send("print-solid", {
      rows,
      burnUs,
      releaseAfter: $("coilMode").value === "release",
    });
    toast(`${r.rows ?? rows} rows accepted by printer`, "success");
    log(
      burnUs === 0
        ? `Heatless row test queued · ${rows} rows · 4 steps/row`
        : `Solid print queued · ${rows} rows · 4 events/row · ${burnUs} µs per event · 4 steps/row`,
      "success",
    );
  } catch (e) {
    toast(e.message, "error");
  } finally {
    $("printBtn").disabled = !state.connected;
  }
};
connectSocket();

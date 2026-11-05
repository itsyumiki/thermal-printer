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
  timeTestRunning: false,
};
const pending = new Map();
const STEPS_PER_MM = 32;
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
    const timeoutMs =
      action === "print-solid"
        ? 3600000
        : action === "time-test"
          ? 180000
          : 15000;
    const timer = setTimeout(() => {
      const p = pending.get(requestId);
      if (p) {
        pending.delete(requestId);
        p.reject(new Error("Host request timed out"));
      }
    }, timeoutMs);
    pending.set(requestId, { resolve, reject, action, timer });
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
  for (const b of document.querySelectorAll(".time-btn"))
    b.disabled = !state.connected || state.timeTestRunning;
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
  // burnUs is the TOTAL per row (split over 4 events). No limit is enforced here: the firmware
  // owns the per-event cap, row total, duty budget and temperature gates and rejects mistakes.
  $("burnUs").disabled = !heatReady;
  if (heatReady && !wasHeatReady && Number($("burnUs").value) === 0) {
    // Old checker demo: 2000 us per event x 4 events = 8000 us per row (stock is ~2.9-8.9 ms).
    $("burnUs").value = "8000";
  }
  state.heatReady = heatReady;
  if (!heatReady) {
    $("burnUs").value = "0";
    $("heaterNotice").querySelector("p").textContent =
      `Heating disabled until temperature, battery and paper sensors are valid. Thermal and duty safeguards remain active in the firmware. Only heatless feed tests are allowed until sensor gates are valid.`;
  } else {
    $("heaterNotice").querySelector("p").textContent =
      `Heat is the total per row, split over 4 events. No limit is checked in this page: the firmware enforces the per-event cap, row total (reports ${info.burnMaxUs} µs), duty budget and temperature limits, and thermal and duty safeguards remain active.`;
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
      clearTimeout(p.timer);
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
  if (m.type === "time-test-progress") {
    timeShowProgress(m);
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
    for (const [requestId, p] of pending) {
      clearTimeout(p.timer);
      pending.delete(requestId);
      p.reject(new Error("Host WebSocket disconnected"));
    }
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
  if (!Number.isInteger(rows) || rows < 1 || rows > 500) {
    toast("Rows must be a whole number from 1 to 500", "error");
    return;
  }
  if (!Number.isInteger(burnUs) || burnUs < 0) {
    toast("Heat per row must be a whole number of µs, 0 or more", "error");
    return;
  }
  if (
    !confirm(
      `Print ${rows} full-width black rows using one bitmap shift/latch per row, four heater slices synchronized to four feed edges at a 6 ms step period, and ${burnUs} µs total heat per row? Heater temperature, sensor and duty safeguards remain active.`,
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
        : `Solid print queued · ${rows} rows · one shift/latch per row · ${burnUs} µs total/row · four synchronized events · 6 ms steps`,
      "success",
    );
  } catch (e) {
    toast(e.message, "error");
  } finally {
    $("printBtn").disabled = !state.connected;
  }
};
// ---- Timer self-test: binary TIMETEST (F0) + host wall clock ---------------------------------
// Each clock counts ~1000 ms in its own units; the host times the printer's RUNNING and DONE frames.
// ratio = wall / what the firmware thinks it counted (1.00 = the clock is right).
const TIME_SOURCES = [
  "TIMEBASE",
  "T3D16",
  "T3D4",
  "T3D64",
  "T3D256",
  "STEP",
  "JIFFY",
  "DELAY",
  "DELAY2",
];
const TIME_LABELS = {
  TIMEBASE: "timebase (micros)",
  T3D16: "heater timer /16",
  T3D4: "heater timer /4",
  T3D64: "heater timer /64",
  T3D256: "heater timer /256",
  STEP: "step tick",
  JIFFY: "jiffies",
  DELAY: "delay 1000us",
  DELAY2: "delay 2us",
};
const timeReport = [];
function timeVerdict(r) {
  const off = (r.ratio - 1) * 100;
  if (Math.abs(off) <= (r.gate ?? 0.02) * 100)
    return r.isrMatchesN === false ? "ISR COUNT MISMATCH" : "OK";
  return r.ratio > 1
    ? `SLOW ${r.ratio.toFixed(2)}x (real time is longer than the firmware assumes)`
    : `FAST ${(1 / r.ratio).toFixed(2)}x (real time is shorter than the firmware assumes)`;
}
function timeDetail(r) {
  const bits = [];
  if (r.impliedOscMHz != null)
    bits.push(`oscillator ${r.impliedOscMHz.toFixed(2)} MHz (assumed 24)`);
  if (r.src === "TIMEBASE")
    bits.push(
      `timebase counted ${r.nPerRun} us while the host saw ${r.meanWallMs.toFixed(1)} ms (gate 1 %)`,
    );
  if (r.src === "STEP" && r.msPerUnit != null)
    bits.push(`${r.msPerUnit.toFixed(3)} ms per step tick (assumed 1)`);
  if (r.src === "JIFFY" && r.msPerUnit != null)
    bits.push(`${r.msPerUnit.toFixed(2)} ms per jiffy (assumed 10)`);
  if (r.src === "DELAY" && r.msPerUnit != null)
    bits.push(`${(r.msPerUnit * 1000).toFixed(1)} us per call (asked 1000)`);
  if (r.src === "DELAY2" && r.msPerUnit != null)
    bits.push(`${(r.msPerUnit * 1000).toFixed(3)} us per call (asked 2)`);
  if (r.msPerJiffy != null)
    bits.push(`jiffy cross-check ${r.msPerJiffy.toFixed(2)} ms`);
  if (r.isrPerRun != null)
    bits.push(
      `compare IRQ fired ${r.isrPerRun.toFixed(0)}x per run (counted ${r.nPerRun})`,
    );
  return bits.join(", ");
}
function timeRender(extra = "") {
  const head =
    "clock               want ms   wall ms (mean +- sd, min..max)       ratio  verdict";
  const lines = timeReport.map((r) => {
    const label = (TIME_LABELS[r.src] ?? r.src).padEnd(19);
    if (r.error) return `${label} ${r.error}`;
    return (
      `${label} ${r.wantMs.toFixed(0).padStart(7)}   ` +
      `${r.meanWallMs.toFixed(1).padStart(7)} +- ${r.stdevMs.toFixed(1).padEnd(5)} ` +
      `${r.minWallMs.toFixed(1)}..${r.maxWallMs.toFixed(1)}`.padEnd(22) +
      ` ${r.ratio.toFixed(3)}  ${timeVerdict(r)}\n${" ".repeat(20)}${timeDetail(r)}`
    );
  });
  $("timeOut").textContent = [head, ...lines, extra].filter(Boolean).join("\n");
}
function timeShowProgress(m) {
  timeRender(
    `${TIME_LABELS[m.src] ?? m.src}: run ${m.run}/${m.reps}, ${m.wallMs.toFixed(1)} ms wall for ${m.wantMs.toFixed(0)} ms counted...`,
  );
}
function timeSetRunning(on) {
  state.timeTestRunning = on;
  for (const b of document.querySelectorAll(".time-btn"))
    b.disabled = on || !state.connected;
}
async function runTimeTests(sources) {
  if (state.timeTestRunning) return;
  timeSetRunning(true);
  timeReport.length = 0;
  timeRender(`starting ${sources.length} test(s)...`);
  try {
    for (const src of sources) {
      try {
        const r = await send("time-test", { src, ms: 1000, reps: 5 });
        timeReport.push(r);
        log(
          `Timer test ${src}: ${r.meanWallMs.toFixed(1)} ms wall for ${r.wantMs.toFixed(0)} ms counted (${r.ratio.toFixed(3)}x) ${timeVerdict(r)}`,
          r.gated === false ? "info" : r.withinGate ? "success" : "warn",
        );
      } catch (e) {
        timeReport.push({ src, error: e.message });
        log(`Timer test ${src} failed: ${e.message}`, "error");
        // A busy printer or a dead link will not get better by retrying the next source.
        break;
      }
      timeRender();
    }
  } finally {
    timeSetRunning(false);
    timeRender();
  }
}
$("timeAllBtn").onclick = () => runTimeTests(TIME_SOURCES);
for (const b of document.querySelectorAll(".time-btn[data-src]"))
  b.onclick = () => runTimeTests([b.dataset.src]);
connectSocket();

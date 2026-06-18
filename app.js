const STORAGE_KEY = "guardbox-app-state-v1";

const defaultState = {
  demoMode: true,
  serialConnected: false,
  deviceName: "演示设备 GuardBox-001",
  cameraUrl: "http://192.168.4.1/capture",
  lastCameraShot: "",
  status: {
    state: "IDLE",
    calibrated: true,
    weightG: 650.25,
    originalWeightG: 0,
    dropG: 0,
    dropThresholdG: 300,
    alarm: false,
    offset: 128436,
    scale: 213.54,
    raw: 269378
  },
  records: [],
  log: []
};

let app = loadState();
let serial = {
  port: null,
  reader: null,
  writer: null,
  buffer: "",
  reading: false
};
let timers = {
  demo: null,
  poll: null
};

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));

const elements = {
  connectionLed: $("#connectionLed"),
  connectionText: $("#connectionText"),
  deviceName: $("#deviceName"),
  statusPanel: $("#statusPanel"),
  stateLabel: $("#stateLabel"),
  stateHeadline: $("#stateHeadline"),
  stateHint: $("#stateHint"),
  currentWeight: $("#currentWeight"),
  originalWeight: $("#originalWeight"),
  dropWeight: $("#dropWeight"),
  dropThreshold: $("#dropThreshold"),
  alarmState: $("#alarmState"),
  calibratedText: $("#calibratedText"),
  offsetText: $("#offsetText"),
  scaleText: $("#scaleText"),
  rawText: $("#rawText"),
  logBox: $("#logBox"),
  recordList: $("#recordList"),
  alarmSheet: $("#alarmSheet"),
  alarmMessage: $("#alarmMessage"),
  cameraFrame: $("#cameraFrame"),
  cameraImage: $("#cameraImage"),
  cameraCaption: $("#cameraCaption"),
  knownWeightInput: $("#knownWeightInput"),
  customThresholdInput: $("#customThresholdInput"),
  cameraUrlInput: $("#cameraUrlInput"),
  demoModeToggle: $("#demoModeToggle"),
  connectSerialBtn: $("#connectSerialBtn")
};

function loadState() {
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_KEY));
    if (saved && saved.status) {
      return {
        ...defaultState,
        ...saved,
        status: { ...defaultState.status, ...saved.status },
        serialConnected: false
      };
    }
  } catch (error) {
    console.warn("State load failed", error);
  }
  return structuredClone(defaultState);
}

function saveState() {
  const copy = { ...app, serialConnected: false };
  localStorage.setItem(STORAGE_KEY, JSON.stringify(copy));
}

function formatWeight(value) {
  return Number(value || 0).toFixed(2);
}

function nowText() {
  return new Date().toLocaleTimeString("zh-CN", { hour12: false });
}

function addLog(message, source = "APP") {
  app.log.unshift(`[${nowText()}] ${source} ${message}`);
  app.log = app.log.slice(0, 80);
  saveState();
  renderLog();
}

function render() {
  const status = app.status;
  const stateMeta = getStateMeta(status.state);

  elements.deviceName.textContent = app.deviceName;
  elements.connectionText.textContent = app.serialConnected
    ? "真实串口已连接"
    : app.demoMode
      ? "演示模式运行中"
      : "未连接设备";
  elements.connectionLed.classList.toggle("offline", !app.serialConnected && !app.demoMode);
  elements.connectSerialBtn.textContent = app.serialConnected ? "断开串口" : "连接串口";

  elements.statusPanel.className = `status-panel ${stateMeta.className}`;
  elements.stateLabel.textContent = stateMeta.label;
  elements.stateHeadline.textContent = stateMeta.headline;
  elements.stateHint.textContent = stateMeta.hint;

  elements.currentWeight.textContent = formatWeight(status.weightG);
  elements.originalWeight.textContent = formatWeight(status.originalWeightG);
  elements.dropWeight.textContent = formatWeight(status.dropG);
  elements.dropThreshold.textContent = status.dropThresholdG;
  elements.alarmState.textContent = status.alarm ? "开启" : "关闭";

  elements.calibratedText.textContent = status.calibrated ? "已校准" : "未校准";
  elements.offsetText.textContent = String(status.offset);
  elements.scaleText.textContent = `${Number(status.scale || 0).toFixed(3)} raw/g`;
  elements.rawText.textContent = String(status.raw);
  elements.demoModeToggle.checked = app.demoMode;
  elements.customThresholdInput.value = status.dropThresholdG;
  elements.cameraUrlInput.value = app.cameraUrl;

  $("#startGuardBtn").disabled = !canControl() || !status.calibrated || status.state === "MONITORING";
  $("#stopGuardBtn").disabled = !canControl() || status.state === "IDLE";
  $("#alarmOffBtn").disabled = !canControl() || !status.alarm;
  $("#sheetAlarmOffBtn").disabled = !canControl() || !status.alarm;

  elements.alarmSheet.classList.toggle("active", status.alarm);
  elements.alarmSheet.setAttribute("aria-hidden", status.alarm ? "false" : "true");
  elements.cameraFrame.classList.toggle("alarm", status.alarm);
  elements.cameraFrame.classList.toggle("has-image", Boolean(app.lastCameraShot));
  elements.cameraImage.src = app.lastCameraShot || "";
  elements.cameraCaption.textContent = app.lastCameraShot
    ? "ESP32 摄像头画面已更新。"
    : "摄像头未测试，报警时将尝试请求 ESP32 抓拍地址。";

  updateThresholdButtons();
  renderLog();
  renderRecords();
  saveState();
}

function getStateMeta(state) {
  if (state === "MONITORING") {
    return {
      label: "监测中",
      className: "monitoring",
      headline: "正在守护外卖",
      hint: "重量下降超过阈值或低于 100g 时会触发报警。"
    };
  }
  if (state === "ALARM") {
    return {
      label: "报警中",
      className: "alarm",
      headline: "疑似外卖被拿走",
      hint: "蜂鸣器已启动，记录已生成，可关闭报警或等待物品放回。"
    };
  }
  return {
    label: app.status.calibrated ? "空闲" : "未校准",
    className: "",
    headline: app.status.calibrated ? "可以放置外卖后开始防盗" : "请先完成去皮和校准",
    hint: app.demoMode ? "演示模式已内置重量变化，可用于答辩展示完整闭环。" : "连接设备后可读取真实重量。"
  };
}

function canControl() {
  return app.demoMode || app.serialConnected;
}

function renderLog() {
  elements.logBox.innerHTML = app.log.length
    ? app.log.map((line) => `<div>${escapeHtml(line)}</div>`).join("")
    : "<div>暂无日志</div>";
}

function renderRecords() {
  if (!app.records.length) {
    elements.recordList.innerHTML = `<article class="record-card recovered"><strong>暂无报警记录</strong><span>点击“模拟报警”或连接设备触发 ALARM 后会生成事件。</span></article>`;
    return;
  }

  elements.recordList.innerHTML = app.records.map((record) => {
    const klass = record.status === "RECOVERED" || record.status === "MANUAL_OFF" ? "recovered" : "";
    const statusText = record.status === "RECOVERED" ? "已恢复" : record.status === "MANUAL_OFF" ? "手动关闭" : "报警中";
    return `
      <article class="record-card ${klass}">
        <strong>${escapeHtml(record.reason)} · ${statusText}</strong>
        <span>${escapeHtml(record.time)} ｜ 当前 ${formatWeight(record.weightG)}g ｜ 下降 ${formatWeight(record.dropG)}g</span>
        <span>证据：${escapeHtml(record.evidence)}</span>
      </article>
    `;
  }).join("");
}

function updateThresholdButtons() {
  $$("#thresholdOptions button").forEach((button) => {
    button.classList.toggle("active", Number(button.dataset.threshold) === Number(app.status.dropThresholdG));
  });
}

function switchView(viewName) {
  $$(".tab").forEach((tab) => tab.classList.toggle("active", tab.dataset.view === viewName));
  $$(".view").forEach((view) => view.classList.toggle("active", view.id === `${viewName}View`));
  if (viewName === "records") {
    elements.alarmSheet.classList.remove("active");
    elements.alarmSheet.setAttribute("aria-hidden", "true");
  }
}

async function sendCommand(command) {
  if (app.demoMode || !app.serialConnected) {
    runDemoCommand(command);
    return;
  }

  if (!serial.writer) {
    addLog("串口未就绪，命令未发送", "ERR");
    return;
  }

  const payload = `${command}\r\n`;
  const encoded = new TextEncoder().encode(payload);
  await serial.writer.write(encoded);
  addLog(`> ${command}`, "TX");
}

function runDemoCommand(command) {
  addLog(`> ${command}`, "DEMO");
  const [name, arg] = command.split(/\s+/);
  const status = app.status;

  if (name === "hx711_tare") {
    status.offset = Math.round(120000 + Math.random() * 12000);
    status.weightG = 0;
    status.raw = status.offset;
    addLog(`Tare OK. offset=${status.offset}`, "DEV");
  }

  if (name === "hx711_cal") {
    const knownWeight = Math.max(1, Number(arg || 500));
    status.scale = Number((205 + Math.random() * 20).toFixed(3));
    status.raw = Math.round(status.offset + status.scale * knownWeight);
    status.weightG = knownWeight;
    status.calibrated = true;
    addLog(`Calibration OK. scale=${status.scale.toFixed(3)} raw_count/g`, "DEV");
  }

  if (name === "hx711_raw") {
    status.raw = Math.round(status.offset + status.scale * Math.max(0, status.weightG));
    addLog(`raw=${status.raw}, offset=${status.offset}, net=${status.raw - status.offset}`, "DEV");
  }

  if (name === "guard_start") {
    if (!status.calibrated) {
      addLog("Start failed: HX711 not calibrated.", "DEV");
      return render();
    }
    if (status.weightG < 100) {
      status.weightG = 650.25;
    }
    status.originalWeightG = status.weightG;
    status.dropG = 0;
    status.state = "MONITORING";
    status.alarm = false;
    addLog(`Guard started. Original food weight = ${formatWeight(status.originalWeightG)} g`, "DEV");
  }

  if (name === "guard_stop") {
    status.state = "IDLE";
    status.originalWeightG = 0;
    status.dropG = 0;
    status.alarm = false;
    markLatestRecord("MANUAL_OFF");
    addLog("Guard stopped. State is now IDLE.", "DEV");
  }

  if (name === "alarm_off") {
    status.state = "IDLE";
    status.alarm = false;
    markLatestRecord("MANUAL_OFF");
    addLog("[ALARM] OFF", "DEV");
  }

  if (name === "set_drop") {
    status.dropThresholdG = Math.max(50, Number(arg || 300));
    addLog(`Drop threshold set to ${status.dropThresholdG} g`, "DEV");
  }

  render();
}

function demoTick() {
  if (!app.demoMode || app.status.state !== "MONITORING") {
    return;
  }

  const status = app.status;
  const smallNoise = (Math.random() - 0.5) * 2.8;
  status.weightG = Math.max(0, status.weightG + smallNoise);
  status.dropG = Math.max(0, status.originalWeightG - status.weightG);
  status.raw = Math.round(status.offset + status.scale * status.weightG);

  if (Math.random() < 0.08) {
    status.weightG = Math.max(20, status.weightG - (status.dropThresholdG + 80));
    status.dropG = Math.max(0, status.originalWeightG - status.weightG);
    triggerAlarm("DROP_TOO_MUCH");
  }

  render();
}

function triggerAlarm(reason) {
  const status = app.status;
  status.state = "ALARM";
  status.alarm = true;
  const evidence = captureCameraShot();
  const record = {
    id: `evt_${Date.now()}`,
    time: new Date().toLocaleString("zh-CN", { hour12: false }),
    reason,
    weightG: status.weightG,
    dropG: status.dropG,
    status: "ALARM",
    evidence
  };
  app.records.unshift(record);
  app.records = app.records.slice(0, 30);
  elements.alarmMessage.textContent = `当前重量 ${formatWeight(status.weightG)}g，下降 ${formatWeight(status.dropG)}g。`;
  addLog(`[ALARM] ON: ${reason}`, "DEV");
}

function captureCameraShot() {
  if (!app.cameraUrl) {
    return "未配置 ESP32 摄像头地址";
  }
  const separator = app.cameraUrl.includes("?") ? "&" : "?";
  app.lastCameraShot = `${app.cameraUrl}${separator}t=${Date.now()}`;
  return `ESP32 抓拍：${app.cameraUrl}`;
}

function markLatestRecord(status) {
  const record = app.records.find((item) => item.status === "ALARM");
  if (record) {
    record.status = status;
  }
}

function seedAlarm() {
  if (app.status.state !== "MONITORING") {
    app.status.originalWeightG = app.status.weightG || 650.25;
    app.status.state = "MONITORING";
  }
  app.status.weightG = Math.max(20, app.status.originalWeightG - app.status.dropThresholdG - 80);
  app.status.dropG = Math.max(0, app.status.originalWeightG - app.status.weightG);
  triggerAlarm("DROP_TOO_MUCH");
  render();
}

function recoverDemoAlarm() {
  if (!app.demoMode || app.status.state !== "ALARM") {
    return;
  }
  if (Math.random() > 0.18) {
    return;
  }
  app.status.weightG = app.status.originalWeightG - Math.random() * 30;
  app.status.dropG = Math.max(0, app.status.originalWeightG - app.status.weightG);
  app.status.originalWeightG = app.status.weightG;
  app.status.state = "MONITORING";
  app.status.alarm = false;
  markLatestRecord("RECOVERED");
  addLog("[RECOVER] Item returned. Alarm off automatically.", "DEV");
  render();
}

async function connectSerial() {
  if (app.serialConnected) {
    await disconnectSerial();
    return;
  }

  if (!("serial" in navigator)) {
    addLog("当前浏览器不支持 Web Serial，请使用 Chrome/Edge 并通过 localhost 打开。", "ERR");
    return;
  }

  try {
    serial.port = await navigator.serial.requestPort();
    await serial.port.open({ baudRate: 115200 });
    serial.writer = serial.port.writable.getWriter();
    app.serialConnected = true;
    app.demoMode = false;
    addLog("串口已连接，波特率 115200", "APP");
    readSerialLoop();
    startPolling();
    render();
  } catch (error) {
    addLog(`串口连接失败：${error.message}`, "ERR");
  }
}

async function disconnectSerial() {
  stopPolling();
  try {
    if (serial.reader) {
      await serial.reader.cancel();
      serial.reader.releaseLock();
    }
    if (serial.writer) {
      serial.writer.releaseLock();
    }
    if (serial.port) {
      await serial.port.close();
    }
  } catch (error) {
    addLog(`断开串口时出现异常：${error.message}`, "ERR");
  } finally {
    serial = { port: null, reader: null, writer: null, buffer: "", reading: false };
    app.serialConnected = false;
    addLog("串口已断开", "APP");
    render();
  }
}

async function readSerialLoop() {
  if (!serial.port || serial.reading) {
    return;
  }
  serial.reading = true;
  const decoder = new TextDecoder();

  try {
    serial.reader = serial.port.readable.getReader();
    while (app.serialConnected) {
      const { value, done } = await serial.reader.read();
      if (done) {
        break;
      }
      serial.buffer += decoder.decode(value, { stream: true });
      consumeSerialBuffer();
    }
  } catch (error) {
    if (app.serialConnected) {
      addLog(`串口读取失败：${error.message}`, "ERR");
    }
  } finally {
    serial.reading = false;
  }
}

function consumeSerialBuffer() {
  const lines = serial.buffer.split(/\r?\n/);
  serial.buffer = lines.pop() || "";
  lines.map((line) => line.trim()).filter(Boolean).forEach((line) => {
    addLog(line, "RX");
    parseDeviceLine(line);
  });
}

function parseDeviceLine(line) {
  parseJsonLine(line) || parseMshLine(line);
}

function parseJsonLine(line) {
  if (!line.startsWith("{")) {
    return false;
  }
  try {
    const message = JSON.parse(line);
    if (message.type === "STATUS") {
      app.status = {
        ...app.status,
        state: message.state || app.status.state,
        weightG: Number(message.weightG ?? app.status.weightG),
        originalWeightG: Number(message.originalWeightG ?? app.status.originalWeightG),
        dropG: Number(message.dropG ?? app.status.dropG),
        alarm: Boolean(message.alarm),
        calibrated: Boolean(message.calibrated)
      };
      render();
      return true;
    }
    if (message.type === "EVENT" && message.event === "ALARM") {
      app.status.weightG = Number(message.weightG ?? app.status.weightG);
      app.status.dropG = Number(message.dropG ?? app.status.dropG);
      triggerAlarm(message.reason || "DEVICE_ALARM");
      render();
      return true;
    }
    if (message.type === "EVENT" && message.event === "RECOVER") {
      app.status.state = "MONITORING";
      app.status.alarm = false;
      app.status.weightG = Number(message.weightG ?? app.status.weightG);
      app.status.originalWeightG = Number(message.newOriginalWeightG ?? app.status.weightG);
      markLatestRecord("RECOVERED");
      render();
      return true;
    }
  } catch (error) {
    addLog(`JSON 解析失败：${error.message}`, "ERR");
  }
  return false;
}

function parseMshLine(line) {
  const status = app.status;
  const keyValue = line.match(/^([a-zA-Z ]+):\s*(.+)$/);
  if (keyValue) {
    const key = keyValue[1].trim();
    const value = keyValue[2].trim();
    if (key === "state") status.state = value;
    if (key === "calibrated") status.calibrated = value === "YES";
    if (key === "offset raw") status.offset = Number(value) || status.offset;
    if (key === "drop threshold") status.dropThresholdG = Number(value.replace(/\D/g, "")) || status.dropThresholdG;
    if (key === "alarm") status.alarm = value === "ON";
    render();
    return;
  }

  const weight = line.match(/weight=([-]?\d+(?:\.\d+)?)\s*g/);
  if (weight) {
    status.weightG = Number(weight[1]);
  }

  const original = line.match(/original=([-]?\d+(?:\.\d+)?)\s*g/);
  if (original) {
    status.originalWeightG = Number(original[1]);
  }

  const drop = line.match(/drop=([-]?\d+(?:\.\d+)?)\s*g/);
  if (drop) {
    status.dropG = Number(drop[1]);
  }

  const raw = line.match(/raw=([-]?\d+),\s*offset=([-]?\d+),\s*net=([-]?\d+)/);
  if (raw) {
    status.raw = Number(raw[1]);
    status.offset = Number(raw[2]);
  }

  const scale = line.match(/scale=([-]?\d+(?:\.\d+)?)/);
  if (scale) {
    status.scale = Number(scale[1]);
    status.calibrated = true;
  }

  if (line.includes("Guard started")) {
    status.state = "MONITORING";
    status.alarm = false;
  }

  if (line.includes("Guard stopped")) {
    status.state = "IDLE";
    status.alarm = false;
  }

  if (line.includes("[ALARM] ON") || line.includes("[WARN]")) {
    status.state = "ALARM";
    status.alarm = true;
    if (!app.records.some((record) => record.status === "ALARM")) {
      triggerAlarm(line.includes("too low") ? "WEIGHT_TOO_LOW" : "DROP_TOO_MUCH");
    }
  }

  if (line.includes("[ALARM] OFF")) {
    status.alarm = false;
    markLatestRecord("MANUAL_OFF");
  }

  if (line.includes("[RECOVER]")) {
    status.state = "MONITORING";
    status.alarm = false;
    markLatestRecord("RECOVERED");
  }

  render();
}

function startPolling() {
  stopPolling();
  timers.poll = setInterval(() => {
    if (app.serialConnected) {
      sendCommand("guard_status");
    }
  }, 1200);
}

function stopPolling() {
  if (timers.poll) {
    clearInterval(timers.poll);
    timers.poll = null;
  }
}

function startDemoTimers() {
  if (timers.demo) {
    clearInterval(timers.demo);
  }
  timers.demo = setInterval(() => {
    demoTick();
    recoverDemoAlarm();
  }, 1000);
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function bindEvents() {
  $("#refreshBtn").addEventListener("click", () => sendCommand("guard_status"));
  $("#connectSerialBtn").addEventListener("click", connectSerial);
  $("#startGuardBtn").addEventListener("click", () => sendCommand("guard_start"));
  $("#stopGuardBtn").addEventListener("click", () => sendCommand("guard_stop"));
  $("#alarmOffBtn").addEventListener("click", () => sendCommand("alarm_off"));
  $("#sheetAlarmOffBtn").addEventListener("click", () => sendCommand("alarm_off"));
  $("#tareBtn").addEventListener("click", () => sendCommand("hx711_tare"));
  $("#readRawBtn").addEventListener("click", () => sendCommand("hx711_raw"));
  $("#calibrateBtn").addEventListener("click", () => {
    const known = Math.max(1, Number(elements.knownWeightInput.value || 500));
    sendCommand(`hx711_cal ${known}`);
  });
  $("#clearLogBtn").addEventListener("click", () => {
    app.log = [];
    addLog("日志已清空", "APP");
  });
  $("#seedAlarmBtn").addEventListener("click", seedAlarm);
  $("#setThresholdBtn").addEventListener("click", () => {
    const value = Math.max(50, Number(elements.customThresholdInput.value || 300));
    sendCommand(`set_drop ${value}`);
  });
  $("#testCameraBtn").addEventListener("click", () => {
    app.cameraUrl = elements.cameraUrlInput.value.trim();
    if (!app.cameraUrl) {
      addLog("请先填写 ESP32 抓拍地址", "ERR");
      render();
      return;
    }
    captureCameraShot();
    addLog(`测试 ESP32 摄像头：${app.cameraUrl}`, "APP");
    render();
  });
  $("#demoModeToggle").addEventListener("change", async (event) => {
    app.demoMode = event.target.checked;
    if (app.demoMode && app.serialConnected) {
      await disconnectSerial();
    }
    addLog(app.demoMode ? "演示模式已开启" : "演示模式已关闭", "APP");
    render();
  });

  $$(".tab").forEach((tab) => {
    tab.addEventListener("click", () => switchView(tab.dataset.view));
  });

  $$("[data-jump]").forEach((button) => {
    button.addEventListener("click", () => switchView(button.dataset.jump));
  });

  $$("#thresholdOptions button").forEach((button) => {
    button.addEventListener("click", () => {
      elements.customThresholdInput.value = button.dataset.threshold;
      sendCommand(`set_drop ${button.dataset.threshold}`);
    });
  });
}

bindEvents();
startDemoTimers();
render();
addLog("App 已启动，P0 功能就绪", "APP");

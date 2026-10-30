(function () {
  const MAX_POINTS = 50;
  const ONLINE_THRESHOLD_SECONDS = 15;
  const POLL_INTERVAL_MS = 2500;
  const VALUE_TOLERANCE = 0.001;

  const root = document.getElementById("dashboard-root");
  if (!root) return;

  const apiUrl = root.dataset.apiUrl;
  const commandUrlTemplate = root.dataset.commandUrlTemplate || "";
  const seedDataEl = document.getElementById("seed-data");
  const seedData = seedDataEl ? JSON.parse(seedDataEl.textContent) : {};

  function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  function hexToRgba(hex, alpha) {
    const clean = hex.replace("#", "");
    const bigint = parseInt(clean, 16);
    const r = (bigint >> 16) & 255;
    const g = (bigint >> 8) & 255;
    const b = bigint & 255;
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
  }

  function formatTime(iso) {
    const d = new Date(iso);
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  }

  function formatDuration(seconds) {
    const total = Math.max(0, Math.round(Number(seconds) || 0));
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    if (h > 0) return `${h} h ${m} min`;
    if (m > 0) return `${m} min ${s} s`;
    return `${s} s`;
  }

  function printerStateLabel(value) {
    const states = {
      0: "Offline",
      1: "Operativa",
      2: "Imprimiendo",
      3: "Pausada",
      4: "Error",
    };
    return states[Math.round(Number(value))] || `Estado ${value}`;
  }

  function readingIsFresh(sensor, serverNow, maxAgeSeconds = ONLINE_THRESHOLD_SECONDS) {
    if (!sensor.timestamp) return false;
    const ageSeconds = (serverNow - new Date(sensor.timestamp).getTime()) / 1000;
    return ageSeconds <= maxAgeSeconds;
  }

  function formattedSensorValue(sensor, serverNow = Date.now()) {
    if (sensor.variable === "printer_state") return printerStateLabel(sensor.value);
    if (sensor.variable === "raspberry_online") {
      if (!readingIsFresh(sensor, serverNow)) return "Sin señal";
      return Number(sensor.value) >= 1 ? "En línea" : "Sin conexión";
    }
    if (sensor.variable === "print_time_s") return formatDuration(sensor.value);
    if (sensor.variable === "print_progress") return `${Number(sensor.value).toFixed(1)} %`;
    return `${sensor.value} ${sensor.unit || ""}`.trim();
  }

  function updateSummary(sensor, serverNow) {
    document.querySelectorAll(`[data-summary-variable="${sensor.variable}"]`).forEach((el) => {
      el.textContent = formattedSensorValue(sensor, serverNow);
    });
  }

  function getCookie(name) {
    const match = document.cookie.match(new RegExp("(^| )" + name + "=([^;]+)"));
    return match ? decodeURIComponent(match[2]) : null;
  }

  function commandUrl(sensorId) {
    return commandUrlTemplate.replace(/\/0\/command\//, `/${sensorId}/command/`);
  }

  async function postCommand(sensorId, value) {
    try {
      const res = await fetch(commandUrl(sensorId), {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-CSRFToken": getCookie("csrftoken"),
        },
        body: JSON.stringify({ value: value }),
      });
      if (!res.ok) {
        const detail = await res.text();
        console.error("Command rejected", res.status, detail);
        return false;
      }
      return true;
    } catch (err) {
      console.error("Command failed", err);
      return false;
    }
  }

  function createChart(canvas, seedPoints) {
    const seriesColor = cssVar("--series-1");
    const grid = cssVar("--gridline");
    const muted = cssVar("--text-muted");
    const surface = cssVar("--surface-1");

    return new Chart(canvas, {
      type: "line",
      data: {
        labels: seedPoints.map((p) => formatTime(p.t)),
        datasets: [
          {
            data: seedPoints.map((p) => p.v),
            borderColor: seriesColor,
            backgroundColor: hexToRgba(seriesColor, 0.1),
            fill: true,
            borderWidth: 2,
            borderCapStyle: "round",
            borderJoinStyle: "round",
            pointRadius: 0,
            pointHoverRadius: 4,
            pointHoverBackgroundColor: seriesColor,
            pointHoverBorderColor: surface,
            pointHoverBorderWidth: 2,
            tension: 0.3,
            cubicInterpolationMode: "monotone",
          },
        ],
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        interaction: { mode: "nearest", intersect: false },
        plugins: {
          legend: { display: false },
          tooltip: { displayColors: false },
        },
        scales: {
          x: { display: false, grid: { display: false } },
          y: {
            grid: { color: grid },
            ticks: { color: muted, maxTicksLimit: 4 },
          },
        },
      },
    });
  }

  const charts = {};
  const lastTimestamps = {};
  const pendingCommands = {}; // sensorId -> value we told the device to reach, cleared once confirmed

  document.querySelectorAll(".sensor-card").forEach((card) => {
    const sensorId = card.dataset.sensorId;
    const widgetType = card.dataset.widgetType;

    if (widgetType === "line_chart") {
      const canvas = card.querySelector('[data-role="chart"]');
      const seed = seedData[sensorId] || [];
      charts[sensorId] = createChart(canvas, seed);
      if (seed.length) {
        lastTimestamps[sensorId] = seed[seed.length - 1].t;
      }
    }

    if (widgetType === "button") {
      const btn = card.querySelector('[data-role="gadget-button"]');
      btn.addEventListener("click", async () => {
        const onValue = parseFloat(btn.dataset.onValue);
        const offValue = parseFloat(btn.dataset.offValue);
        const isLit = btn.classList.contains("lit");
        const target = isLit ? offValue : onValue;
        btn.classList.add("pending");
        pendingCommands[sensorId] = target;
        await postCommand(sensorId, target);
      });
    }

    if (widgetType === "slider") {
      const slider = card.querySelector('[data-role="gadget-slider"]');
      const status = card.querySelector('[data-role="command-status"]');

      slider.addEventListener("input", () => {
        if (status) status.textContent = "Seleccionado: " + slider.value;
      });

      slider.addEventListener("change", async () => {
        const target = parseFloat(slider.value);
        pendingCommands[sensorId] = target;
        if (status) status.textContent = "Enviando " + target + "...";
        const ok = await postCommand(sensorId, target);
        if (ok) {
          if (status) status.textContent = "Comando enviado: " + target;
        } else {
          delete pendingCommands[sensorId];
          if (status) status.textContent = "ERROR al enviar comando";
        }
      });
    }
  });

  function isSliderBeingDragged(slider) {
    return document.activeElement === slider;
  }

  async function poll() {
    try {
      const res = await fetch(apiUrl, { headers: { "X-Requested-With": "XMLHttpRequest" } });
      if (!res.ok) return;
      const data = await res.json();
      const serverNow = new Date(data.server_time).getTime();

      data.sensors.forEach((s) => {
        if (s.value === null) return;
        const card = document.querySelector(`.sensor-card[data-sensor-id="${s.id}"]`);
        if (!card) return;
        const widgetType = card.dataset.widgetType;

        const valueEl = card.querySelector('[data-role="latest-value"]');
        updateSummary(s, serverNow);
        if (valueEl) {
          valueEl.textContent = widgetType === "slider" ? String(s.value) : formattedSensorValue(s, serverNow);
        }

        if (widgetType === "line_chart") {
          const chart = charts[s.id];
          if (!chart || lastTimestamps[s.id] === s.timestamp) return;
          lastTimestamps[s.id] = s.timestamp;
          chart.data.labels.push(formatTime(s.timestamp));
          chart.data.datasets[0].data.push(s.value);
          if (chart.data.datasets[0].data.length > MAX_POINTS) {
            chart.data.labels.shift();
            chart.data.datasets[0].data.shift();
          }
          chart.update("none");
        }

        if (widgetType === "meter") {
          const fill = card.querySelector('[data-role="meter-fill"]');
          const min = parseFloat(fill.dataset.min);
          const max = parseFloat(fill.dataset.max);
          const pct = Math.max(0, Math.min(100, ((s.value - min) / (max - min)) * 100));
          fill.style.width = `${pct}%`;
        }

        if (widgetType === "led") {
          const dot = card.querySelector('[data-role="led-dot"]');
          const threshold = parseFloat(dot.dataset.threshold);
          dot.classList.toggle("lit", s.value >= threshold);
        }

        if (widgetType === "button") {
          const btn = card.querySelector('[data-role="gadget-button"]');
          const onValue = parseFloat(btn.dataset.onValue);
          const isOn = Math.abs(s.value - onValue) < VALUE_TOLERANCE;
          btn.textContent = isOn ? "Encendido" : "Apagado";
          btn.classList.toggle("lit", isOn);

          const pending = pendingCommands[s.id];
          if (pending !== undefined && Math.abs(s.value - pending) < VALUE_TOLERANCE) {
            delete pendingCommands[s.id];
            btn.classList.remove("pending");
          }
        }

        if (widgetType === "slider") {
          const slider = card.querySelector('[data-role="gadget-slider"]');
          const pending = pendingCommands[s.id];

          if (pending !== undefined) {
            if (Math.abs(s.value - pending) < VALUE_TOLERANCE) {
              delete pendingCommands[s.id];
              const status = card.querySelector('[data-role="command-status"]');
              if (status) status.textContent = `Confirmado: ${s.value} ${s.unit || ""}`.trim();
              if (!isSliderBeingDragged(slider)) {
                slider.value = s.value;
              }
            } else if (!isSliderBeingDragged(slider)) {
              slider.value = pending;
            }
          } else if (!isSliderBeingDragged(slider)) {
            slider.value = s.value;
          }
        }
      });

      data.devices.forEach((dev) => {
        const card = document.querySelector(`.project-device-panel[data-device-id="${dev.id}"]`);
        if (!card) return;
        const badge = card.querySelector('[data-role="status-badge"]');
        if (!badge) return;

        if (!dev.last_seen_at) {
          badge.textContent = "sin datos";
          badge.className = "badge project-badge";
          return;
        }
        const ageSeconds = (serverNow - new Date(dev.last_seen_at).getTime()) / 1000;
        if (ageSeconds <= ONLINE_THRESHOLD_SECONDS) {
          badge.textContent = "en línea";
          badge.className = "badge project-badge online";
        } else {
          badge.textContent = "sin señal";
          badge.className = "badge project-badge offline";
        }
      });
    } catch (err) {
      console.error("Polling failed", err);
    }
  }

  poll();
  setInterval(poll, POLL_INTERVAL_MS);
})();

// Signal Light web UI. Pure DOM, no framework.

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

const state = {
  config: null,
  snapshot: null,
  // active animation timers per "<container-id>"
  timers: new Map(),
  // current tempo for the LIVE LED only (gallery cards always play at 1.0x).
  liveTempo: 1.0,
};

// ---------------- helpers ----------------

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") node.className = v;
    else if (k === "html") node.innerHTML = v;
    else if (k.startsWith("on") && typeof v === "function") node.addEventListener(k.slice(2), v);
    else if (v !== null && v !== undefined) node.setAttribute(k, v);
  }
  for (const c of children.flat()) {
    if (c == null) continue;
    if (typeof c === "string") node.appendChild(document.createTextNode(c));
    else node.appendChild(c);
  }
  return node;
}

function clamp(n, lo, hi) { return Math.max(lo, Math.min(hi, n)); }

// ---------------- LED animator ----------------

/**
 * Animate a 3-LED SVG by frames. Each frame: {r, y, g, ms}.
 * Brightness 0-255 mapped to opacity for lens + glow.
 * Frame with ms=null holds forever (last frame).
 * Loops indefinitely (or stops at the hold frame).
 *
 * key uniquely identifies the SVG so we can cancel previous timers.
 */
function animateLed(svg, frames, key, tempoGetter) {
  // tempoGetter: () => number (read live so user can see speed change without restarting)
  // cancel previous
  const prev = state.timers.get(key);
  if (prev) {
    clearTimeout(prev);
    state.timers.delete(key);
  }
  if (!frames || frames.length === 0) {
    paintLed(svg, { r: 0, y: 0, g: 0 });
    return;
  }
  let i = 0;
  const step = () => {
    const f = frames[i % frames.length];
    paintLed(svg, f);
    if (f.ms == null) return; // hold forever
    const tempo = tempoGetter ? Math.max(0.25, Math.min(6, tempoGetter())) : 1.0;
    const dur = Math.max(20, f.ms / tempo);
    const t = setTimeout(() => {
      i = (i + 1) % frames.length;
      step();
    }, dur);
    state.timers.set(key, t);
  };
  step();
}

function paintLed(svg, frame) {
  const r = clamp((frame.r ?? 0) / 255, 0, 1);
  const y = clamp((frame.y ?? 0) / 255, 0, 1);
  const g = clamp((frame.g ?? 0) / 255, 0, 1);

  // Three channels, three colors.
  paintChannel(svg, "r", r, [255, 80, 80]);
  paintChannel(svg, "y", y, [255, 224, 80]);
  paintChannel(svg, "g", g, [80, 255, 180]);
}

function paintChannel(svg, ch, brightness, rgb) {
  const lens = svg.querySelector(`.lens-${ch}`);
  const glow = svg.querySelector(`.glow-${ch}`);
  if (!lens || !glow) return;

  const [R, G, B] = rgb;
  if (brightness < 0.02) {
    lens.setAttribute("fill", "#0a0d14");
    lens.setAttribute("filter", "none");
    glow.setAttribute("opacity", "0");
    return;
  }

  // Lens: bright saturated color, alpha tied to brightness so dim states feel dim.
  // We boost the minimum alpha so any nonzero value is clearly visible.
  const lensAlpha = 0.45 + 0.55 * brightness;
  lens.setAttribute("fill", `rgb(${R}, ${G}, ${B})`);
  lens.setAttribute("fill-opacity", String(lensAlpha.toFixed(3)));

  // Glow: blurred halo, alpha pumped to make even mid brightness pop.
  glow.setAttribute("opacity", String(Math.min(1, 0.4 + 0.7 * brightness).toFixed(3)));
  glow.setAttribute("fill", `rgb(${R}, ${G}, ${B})`);
}

// ---------------- gallery cards ----------------

function buildGallery() {
  const root = $("#gallery");
  root.innerHTML = "";
  const effects = state.config?.effects || [];
  for (const eff of effects) {
    const svgKey = `card-${eff.id}`;
    const svg = buildLedSvg(svgKey);
    svg.classList.add("led-svg", "led-svg-mini");
    const card = el("div", {
      class: "card",
      "data-effect-id": eff.id,
      onclick: () => manualSend(eff.id),
    },
      svg,
      el("div", { class: "card-name" }, eff.name || eff.id),
      el("div", { class: "card-summary" }, eff.summary || ""),
      el("div", { class: "card-id" }, eff.id),
    );
    root.appendChild(card);
    // gallery cards always play at base tempo so user has a reference for "what working looks like at 1x"
    animateLed(svg, eff.frames || [], svgKey, () => 1.0);
  }
  highlightCurrent();
}

function highlightCurrent() {
  const cur = state.snapshot?.effect_id;
  $$(".card").forEach(c => {
    c.classList.toggle("is-current", c.getAttribute("data-effect-id") === cur);
  });
}

function buildLedSvg(key) {
  // Reuse same structure as live; svg id includes key for uniqueness.
  const NS = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(NS, "svg");
  svg.setAttribute("viewBox", "0 0 220 460");
  svg.setAttribute("data-led-key", key);
  // We reuse the defs defined in the live svg in index.html (#blur-r etc).
  const rect = document.createElementNS(NS, "rect");
  rect.setAttribute("class", "led-case");
  rect.setAttribute("x", "20"); rect.setAttribute("y", "10");
  rect.setAttribute("width", "180"); rect.setAttribute("height", "440");
  rect.setAttribute("rx", "22");
  svg.appendChild(rect);
  const mkCircle = (cx, cy, cls, glowCls, filterId) => {
    const glow = document.createElementNS(NS, "circle");
    glow.setAttribute("cx", cx); glow.setAttribute("cy", cy); glow.setAttribute("r", "70");
    glow.setAttribute("filter", `url(#${filterId})`);
    glow.setAttribute("class", `glow ${glowCls}`);
    svg.appendChild(glow);
    const lens = document.createElementNS(NS, "circle");
    lens.setAttribute("cx", cx); lens.setAttribute("cy", cy); lens.setAttribute("r", "56");
    lens.setAttribute("class", `lens ${cls}`);
    svg.appendChild(lens);
  };
  mkCircle(110, 90,  "lens-r", "glow-r", "blur-r");
  mkCircle(110, 230, "lens-y", "glow-y", "blur-y");
  mkCircle(110, 370, "lens-g", "glow-g", "blur-g");
  return svg;
}

// ---------------- manual row ----------------

function buildManualRow() {
  const row = $("#manual-row");
  row.innerHTML = "";
  const effects = state.config?.effects || [];
  for (const eff of effects) {
    let dotCls = "dot-off";
    if (eff.color_hint === "green") dotCls = "dot-g";
    else if (eff.color_hint === "yellow") dotCls = "dot-y";
    else if (eff.color_hint === "red") dotCls = "dot-r";
    else if (eff.color_hint === "cycle") dotCls = "dot-y";
    const btn = el("button", {
      class: "btn",
      onclick: () => manualSend(eff.id),
    },
      el("span", { class: `dot ${dotCls}` }),
      eff.name || eff.id,
    );
    row.appendChild(btn);
  }
}

async function manualSend(effectId) {
  try {
    const r = await fetch("/api/manual", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ effect: effectId, hold_seconds: 6 }),
    });
    if (!r.ok) console.warn("manual failed", await r.text());
  } catch (e) { console.warn(e); }
}

// ---------------- live mirror ----------------

function applySnapshot(snap) {
  state.snapshot = snap;
  state.liveTempo = snap.tempo || 1.0;

  // live LED — animateLed will read state.liveTempo each frame
  const live = $("#live-led");
  const eff = snap.effect || {};
  animateLed(live, eff.frames || [], "live", () => state.liveTempo);
  $("#cur-effect-name").textContent = eff.name || snap.effect_id || "—";
  $("#cur-effect-summary").textContent = eff.summary || "";

  // tempo chip — only show for work-class effects (tempo != 1 OR multiple workers)
  const tempo = snap.tempo || 1.0;
  const workingCount = (snap.counts && snap.counts.working) || 0;
  const showTempo = workingCount >= 1 && (snap.effect_id === "working" || snap.effect_id === "thinking");
  const chip = $("#tempo-chip");
  if (showTempo) {
    chip.classList.remove("hidden");
    $("#tempo-val").textContent = `${tempo.toFixed(1)}×`;
    // light up bars 1-5 by tempo: 1x→1, 2x→2.5, 3.5x→5
    const lit = Math.round(((tempo - 1) / 2.5) * 4) + 1;
    chip.classList.toggle("tempo-fast", tempo >= 2.5);
    chip.querySelectorAll(".tempo-bars i").forEach((b, i) => {
      b.classList.toggle("on", i < lit);
    });
  } else {
    chip.classList.add("hidden");
  }

  // override banner
  const ob = $("#override-banner");
  if (snap.override && snap.override.effect_id) {
    ob.classList.remove("hidden");
    ob.classList.toggle("is-turn-end", snap.override.reason === "turn_end");
    $("#override-tag").textContent = snap.override.reason === "turn_end" ? "session done" : "manual";
    const txt = snap.override.reason === "turn_end"
      ? `一个 session 完成 · ${snap.override.remaining_s}s 后回到聚合状态`
      : `${snap.override.effect_id} · 还剩 ${snap.override.remaining_s}s 自动恢复`;
    $("#override-text").textContent = txt;
  } else {
    ob.classList.add("hidden");
  }

  // winner banner
  const winBanner = $("#winner-banner");
  if (snap.winner && snap.winner.sid) {
    winBanner.classList.remove("hidden");
    $("#winner-text").textContent = `${snap.winner.sid} · ${snap.winner.event}`;
  } else {
    winBanner.classList.add("hidden");
  }

  // filter counts
  if (snap.counts) {
    $("#cnt-all").textContent = snap.counts.all;
    $("#cnt-claude").textContent = snap.counts.claude;
    $("#cnt-codex").textContent = snap.counts.codex;
  }
  // filter active state (server may have flipped it without us asking)
  if (snap.agent_filter) {
    $$(".filter-btn").forEach(b => {
      b.classList.toggle("is-active", b.getAttribute("data-scope") === snap.agent_filter);
    });
  }

  // display profile + brightness + tempo toggle
  if (snap.display) {
    buildProfileButtons(snap);
    syncBrightness(snap);
    syncTempoToggle(snap);
  }

  // board pill
  const pill = $("#board-pill");
  const txt = $("#board-text");
  pill.classList.remove("pill-ok", "pill-warn", "pill-bad", "pill-muted");
  if (snap.board.online) {
    pill.classList.add("pill-ok");
    txt.textContent = `board · ${snap.board.host}:${snap.board.port}`;
  } else {
    pill.classList.add("pill-bad");
    const err = snap.board.last_error ? ` (${snap.board.last_error})` : "";
    txt.textContent = `offline${err}`;
  }

  // meta
  $("#meta-host").textContent = `${snap.board.host}:${snap.board.port}`;
  $("#meta-last-send").textContent = snap.board.last_send_at
    ? new Date(snap.board.last_send_at * 1000).toLocaleTimeString()
    : "—";
  $("#meta-connects").textContent = snap.board.connects;
  $("#meta-errors").textContent = snap.board.errors;
  $("#meta-last-err").textContent = snap.board.last_error || "—";

  // sessions
  buildSessions(snap.sessions || [], snap.winner ? snap.winner.sid : null);

  highlightCurrent();
}

function buildSessions(rows, winnerSid) {
  const root = $("#sessions");
  root.innerHTML = "";
  if (rows.length === 0) {
    root.appendChild(el("div", { class: "sessions-empty" }, "no active sessions"));
    return;
  }
  // sort winner to top, then by age
  const sorted = [...rows].sort((a, b) => {
    if (a.sid === winnerSid) return -1;
    if (b.sid === winnerSid) return 1;
    return a.age_s - b.age_s;
  });
  for (const s of sorted) {
    const isWinner = s.sid === winnerSid;
    root.appendChild(el("div", { class: "session-row" + (isWinner ? " is-winner" : "") },
      el("div", { class: "winner-dot", title: isWinner ? "currently driving the light" : "" }),
      el("div", { class: "session-agent" + (s.agent === "codex" ? " is-codex" : "") }, s.agent || "—"),
      el("div", { class: "session-cwd", title: s.cwd || "" }, prettyCwd(s.cwd) || "(no cwd)"),
      el("div", { class: "session-event" }, s.event),
      el("div", { class: "session-age" }, `${s.age_s}s`),
      el("button", {
        class: "session-x",
        title: "drop session",
        onclick: () => dropSession(s.sid),
      }, "✕"),
    ));
  }
}

function prettyCwd(cwd) {
  if (!cwd) return "";
  const parts = cwd.split("/").filter(Boolean);
  if (parts.length <= 2) return cwd;
  return ".../" + parts.slice(-2).join("/");
}

async function dropSession(sid) {
  try {
    await fetch("/api/session/remove", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ sid }),
    });
  } catch (e) { console.warn(e); }
}

// ---------------- boot ----------------

async function loadConfig() {
  const r = await fetch("/api/config");
  state.config = await r.json();
  buildGallery();
  buildManualRow();
}

function openStream() {
  const es = new EventSource("/stream");
  es.addEventListener("snapshot", e => {
    try { applySnapshot(JSON.parse(e.data)); } catch (err) { console.warn(err); }
  });
  es.addEventListener("ping", () => {});
  es.onerror = () => {
    // browser will auto-retry
  };
}

// ---------------- demo scenarios ----------------

// A step is one of:
//   {hook: {...payload}}            → POST /hook with payload
//   {drop: "sid"}                   → POST /api/session/remove
//   {wait: ms}                      → pause (no network)
//   {label: "..."}                  → just update status text
// All steps also have an optional `label` shown to the user during that step.

const SCENARIOS = {
  solo: {
    name: "Solo Claude",
    cleanup: ["solo-a"],
    steps: [
      { label: "📨 你提交了 prompt", wait: 600,
        hook: { hook_event_name: "UserPromptSubmit", session_id: "solo-a", cwd: "/Users/cherryai/demo/solo", agent: "claude", pid: 1 } },
      { label: "💭 Claude 在思考…", wait: 2500 },
      { label: "🔧 Claude 调用工具 (Bash/Read/Edit)", wait: 600,
        hook: { hook_event_name: "PreToolUse", session_id: "solo-a", cwd: "/Users/cherryai/demo/solo", agent: "claude", pid: 1 } },
      { label: "🔧 工具还在跑…", wait: 3500 },
      { label: "✅ 工具完成,Claude 继续工作", wait: 600,
        hook: { hook_event_name: "PostToolUse", session_id: "solo-a", cwd: "/Users/cherryai/demo/solo", agent: "claude", pid: 1 } },
      { label: "💭 Claude 整理结果…", wait: 2500 },
      { label: "🎉 Claude 完成 (Stop → 短闪绿)", wait: 600,
        hook: { hook_event_name: "Stop", session_id: "solo-a", agent: "claude", pid: 1 } },
      { label: "↩︎ 回到 idle", wait: 1500 },
    ],
  },

  two: {
    name: "Two agents",
    cleanup: ["two-claude", "two-codex"],
    steps: [
      { label: "📨 Claude 窗口开始工作", wait: 500,
        hook: { hook_event_name: "PreToolUse", session_id: "two-claude", cwd: "/Users/cherryai/demo/claude-proj", tool_name: "Bash", agent: "claude", pid: 1 } },
      { label: "Claude 在跑…", wait: 2500 },
      { label: "📨 Codex 窗口也开始跑 (tempo 变 1.4×)", wait: 500,
        hook: { hook_event_name: "PreToolUse", session_id: "two-codex", cwd: "/Users/cherryai/demo/codex-proj", tool_name: "Read", agent: "codex", pid: 1 } },
      { label: "两个 agent 并跑", wait: 3000 },
      { label: "❓ Claude 调 AskUserQuestion → 灯变黄 (attention)", wait: 500,
        hook: { hook_event_name: "PreToolUse", session_id: "two-claude", cwd: "/Users/cherryai/demo/claude-proj", tool_name: "AskUserQuestion", agent: "claude", pid: 1 } },
      { label: "Claude 在等你回答…", wait: 3500 },
      { label: "✅ 你回答完了,Claude 继续跑", wait: 500,
        hook: { hook_event_name: "PostToolUse", session_id: "two-claude", cwd: "/Users/cherryai/demo/claude-proj", tool_name: "AskUserQuestion", agent: "claude", pid: 1 } },
      { label: "(Claude 又是 working, Codex 还在跑)", wait: 2500 },
      { label: "🎉 Codex 完成 (短闪绿, Claude 仍 working)", wait: 500,
        hook: { hook_event_name: "Stop", session_id: "two-codex", agent: "codex", pid: 1 } },
      { label: "回到只有 Claude 的 working", wait: 2500 },
      { label: "🎉 Claude 也完成,回 idle", wait: 500,
        hook: { hook_event_name: "Stop", session_id: "two-claude", agent: "claude", pid: 1 } },
      { label: "↩︎ idle", wait: 1500 },
    ],
  },

  failures: {
    name: "Failures showcase",
    cleanup: ["fail-1", "fail-2", "fail-3", "fail-4"],
    steps: [
      { label: "📨 #1 在跑 Bash", wait: 500,
        hook: { hook_event_name: "PreToolUse", session_id: "fail-1", tool_name: "Bash", cwd: "/Users/cherryai/proj1", agent: "claude", pid: 1 } },
      { label: "#1 Bash 跑得好好的…", wait: 2000 },
      { label: "💥 #1 Bash 失败 (PostToolUse + error) → BLOCKED", wait: 300,
        hook: { hook_event_name: "PostToolUse", session_id: "fail-1", tool_name: "Bash", tool_response: { error: "command not found" }, agent: "claude", pid: 1 } },
      { label: "看红黄交替的 blocked…", wait: 3500 },
      { label: "✅ #1 修了,继续干活", wait: 300,
        hook: { hook_event_name: "PreToolUse", session_id: "fail-1", tool_name: "Bash", agent: "claude", pid: 1 } },
      { wait: 2000 },
      { label: "📨 #2 加入,Claude 主动调用 AskUserQuestion", wait: 300,
        hook: { hook_event_name: "PreToolUse", session_id: "fail-2", tool_name: "AskUserQuestion", cwd: "/Users/cherryai/proj2", agent: "claude", pid: 1 } },
      { label: "黄灯慢闪: Claude 在问你问题…", wait: 4000 },
      { label: "✅ 你回答了", wait: 300,
        hook: { hook_event_name: "PostToolUse", session_id: "fail-2", tool_name: "AskUserQuestion", agent: "claude", pid: 1 } },
      { wait: 2000 },
      { label: "🚦 #3 API 限流 (StopFailure rate_limit → ATTENTION)", wait: 300,
        hook: { hook_event_name: "StopFailure", session_id: "fail-3", error_type: "rate_limit", agent: "claude", pid: 1 } },
      { label: "rate_limit 只是黄闪, 不当 blocked…", wait: 3500 },
      { label: "💀 #4 计费失败 (StopFailure billing_error → BLOCKED)", wait: 300,
        hook: { hook_event_name: "StopFailure", session_id: "fail-4", error_type: "billing_error", agent: "claude", pid: 1 } },
      { label: "硬错误, 红灯狂闪…", wait: 4000 },
      { label: "🎉 #1 #2 收尾", wait: 300,
        hook: { hook_event_name: "Stop", session_id: "fail-1", agent: "claude", pid: 1 } },
      { wait: 200,
        hook: { hook_event_name: "Stop", session_id: "fail-2", agent: "claude", pid: 1 } },
      { wait: 1500 },
    ],
  },

  rush: {
    name: "Rush hour",
    cleanup: ["rush-1", "rush-2", "rush-3", "rush-4", "rush-5"],
    steps: [
      { label: "📨 #1 Claude 加入", wait: 300,
        hook: { hook_event_name: "PreToolUse", session_id: "rush-1", cwd: "/Users/cherryai/projA", agent: "claude", pid: 1 } },
      { wait: 600 },
      { label: "📨 #2 Codex 加入 (tempo 1.4×)", wait: 300,
        hook: { hook_event_name: "PreToolUse", session_id: "rush-2", cwd: "/Users/cherryai/projB", agent: "codex", pid: 1 } },
      { wait: 800 },
      { label: "📨 #3 Claude 加入 (tempo 1.8×)", wait: 300,
        hook: { hook_event_name: "PreToolUse", session_id: "rush-3", cwd: "/Users/cherryai/projC", agent: "claude", pid: 1 } },
      { wait: 800 },
      { label: "📨 #4 Codex 加入 (tempo 2.2×)", wait: 300,
        hook: { hook_event_name: "PreToolUse", session_id: "rush-4", cwd: "/Users/cherryai/projD", agent: "codex", pid: 1 } },
      { wait: 800 },
      { label: "📨 #5 Claude 加入 (tempo 2.6× 红色)", wait: 300,
        hook: { hook_event_name: "PreToolUse", session_id: "rush-5", cwd: "/Users/cherryai/projE", agent: "claude", pid: 1 } },
      { label: "🔥 灯效高速循环 — 卧槽好忙", wait: 4000 },
      { label: "💥 #4 工具失败 (PostToolUse + tool_response.error → 红闪)", wait: 300,
        hook: { hook_event_name: "PostToolUse", session_id: "rush-4", tool_name: "Bash", tool_response: { error: "exit 1" }, agent: "codex", pid: 1 } },
      { label: "处理错误中…", wait: 3000 },
      { label: "✅ #4 恢复,继续工作", wait: 300,
        hook: { hook_event_name: "PreToolUse", session_id: "rush-4", cwd: "/Users/cherryai/projD", agent: "codex", pid: 1 } },
      { wait: 1500 },
      { label: "🎉 #1 完成 (短闪绿,tempo 降到 2.2×)", wait: 300,
        hook: { hook_event_name: "Stop", session_id: "rush-1", agent: "claude", pid: 1 } },
      { wait: 1500 },
      { label: "🎉 #2 完成 (tempo 1.8×)", wait: 300,
        hook: { hook_event_name: "Stop", session_id: "rush-2", agent: "codex", pid: 1 } },
      { wait: 1200 },
      { label: "🎉 #3 完成 (tempo 1.4×)", wait: 300,
        hook: { hook_event_name: "Stop", session_id: "rush-3", agent: "claude", pid: 1 } },
      { wait: 1200 },
      { label: "🎉 #4 完成 (tempo 1.0×)", wait: 300,
        hook: { hook_event_name: "Stop", session_id: "rush-4", agent: "codex", pid: 1 } },
      { wait: 1200 },
      { label: "🎉 #5 完成,回 idle", wait: 300,
        hook: { hook_event_name: "Stop", session_id: "rush-5", agent: "claude", pid: 1 } },
      { wait: 1500 },
    ],
  },
};

const demoState = { running: false, cancel: false };

async function runScenario(key) {
  if (demoState.running) return;
  const scn = SCENARIOS[key];
  if (!scn) return;
  demoState.running = true;
  demoState.cancel = false;

  // disable all demo buttons, show runner panel
  $$(".demo-btn").forEach(b => b.disabled = true);
  $("#demo-runner").classList.remove("hidden");
  const stepEl = $("#demo-step");
  const barEl = $("#demo-progress");
  stepEl.textContent = `▶ ${scn.name} 开始…`;
  barEl.style.width = "0%";

  const totalSteps = scn.steps.length;

  try {
    for (let i = 0; i < scn.steps.length; i++) {
      if (demoState.cancel) {
        stepEl.textContent = "已取消";
        break;
      }
      const step = scn.steps[i];
      barEl.style.width = `${Math.round((i / totalSteps) * 100)}%`;
      if (step.label) stepEl.textContent = `[${i + 1}/${totalSteps}] ${step.label}`;
      if (step.hook) {
        try {
          await fetch("/hook", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(step.hook),
          });
        } catch (e) { console.warn("hook failed", e); }
      }
      if (step.drop) {
        try {
          await fetch("/api/session/remove", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ sid: step.drop }),
          });
        } catch (e) { console.warn("drop failed", e); }
      }
      if (step.wait) await sleep(step.wait);
    }
    if (!demoState.cancel) {
      barEl.style.width = "100%";
      stepEl.textContent = `✓ ${scn.name} 完成`;
      await sleep(1500);
    }
  } finally {
    // cleanup sessions the demo created (always, even on cancel)
    for (const sid of (scn.cleanup || [])) {
      try {
        await fetch("/api/session/remove", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ sid }),
        });
      } catch (e) {}
    }
    demoState.running = false;
    demoState.cancel = false;
    $("#demo-runner").classList.add("hidden");
    $$(".demo-btn").forEach(b => b.disabled = false);
  }
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function wireDemoButtons() {
  $$(".demo-btn").forEach(btn => {
    btn.addEventListener("click", () => {
      const key = btn.getAttribute("data-demo");
      runScenario(key);
    });
  });
  $("#demo-cancel").addEventListener("click", () => {
    demoState.cancel = true;
  });
}

// ---------------- display profile + brightness ----------------

function buildProfileButtons(snap) {
  const row = $("#profile-row");
  const profiles = (snap.display && snap.display.available_profiles) || [];
  const active = snap.display && snap.display.profile;
  // build once; if button count matches just sync active state
  const existing = row.querySelectorAll(".profile-btn");
  if (existing.length === profiles.length) {
    existing.forEach((btn, i) => {
      btn.classList.toggle("is-active", btn.getAttribute("data-profile") === active);
    });
    return;
  }
  row.innerHTML = "";
  for (const p of profiles) {
    const btn = el("button", {
      class: "profile-btn" + (p.id === active ? " is-active" : ""),
      "data-profile": p.id,
      title: p.summary || "",
      onclick: () => setDisplay({ profile: p.id }),
    }, p.name || p.id);
    row.appendChild(btn);
  }
}

function syncBrightness(snap) {
  const b = (snap.display && snap.display.brightness) || 1.0;
  const slider = $("#brightness-slider");
  const valEl = $("#brightness-val");
  const pct = Math.round(b * 100);
  if (document.activeElement !== slider) slider.value = String(pct);
  valEl.textContent = `${pct}%`;
}

function syncTempoToggle(snap) {
  const tog = $("#tempo-toggle");
  const enabled = !!(snap.display && snap.display.tempo_enabled);
  if (tog.checked !== enabled) tog.checked = enabled;
}

async function setDisplay(patch) {
  try {
    const r = await fetch("/api/display", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(patch),
    });
    if (!r.ok) console.warn("display patch failed", await r.text());
  } catch (e) { console.warn(e); }
}

let _brightnessTimer = null;
function wireDisplayControls() {
  $("#brightness-slider").addEventListener("input", e => {
    const pct = parseInt(e.target.value, 10);
    $("#brightness-val").textContent = `${pct}%`;
    clearTimeout(_brightnessTimer);
    _brightnessTimer = setTimeout(() => {
      setDisplay({ brightness: pct / 100 });
    }, 120);  // debounce so dragging doesn't spam the API
  });
  $("#tempo-toggle").addEventListener("change", e => {
    setDisplay({ tempo_enabled: e.target.checked });
  });
}

// ---------------- agent filter ----------------

async function setAgentFilter(scope) {
  try {
    await fetch("/api/agent-filter", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ scope }),
    });
  } catch (e) { console.warn(e); }
}

function wireFilterButtons() {
  $$(".filter-btn").forEach(btn => {
    btn.addEventListener("click", () => {
      const scope = btn.getAttribute("data-scope");
      $$(".filter-btn").forEach(b => b.classList.toggle("is-active", b === btn));
      setAgentFilter(scope);
    });
  });
}

(async function main() {
  await loadConfig();
  wireFilterButtons();
  wireDemoButtons();
  wireDisplayControls();
  openStream();
})();

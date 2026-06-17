#!/usr/bin/env python3
"""
ESP32 Claude Status Light - local daemon.

Single file. Stdlib only. Owns:
  - HTTP/SSE on 127.0.0.1:7878 (web UI + hook ingress + manual test)
  - per-session state aggregation with priority
  - frame scheduler that drives the physical light
  - long-lived TCP connection to the ESP32 (with heartbeat + reconnect)

The web UI is served as static assets from ./webui/ next to this file.

Run:
    python3 signal_daemon.py
"""

from __future__ import annotations

import json
import os
import queue
import socket
import sys
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable
from urllib.parse import urlparse, parse_qs


HERE = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = HERE / "config.default.json"
USER_CONFIG_DIR = Path(os.environ.get("SIGNAL_LIGHT_STATE_DIR") or (Path.home() / ".signal_light"))
USER_CONFIG_PATH = USER_CONFIG_DIR / "config.json"
WEBUI_DIR = HERE / "webui"


# ============================================================================
# Config
# ============================================================================

class Config:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._listeners: list[Callable[[], None]] = []
        self.data: dict[str, Any] = {}
        self.load()

    def load(self) -> None:
        with self._lock:
            if USER_CONFIG_PATH.exists():
                self.data = json.loads(USER_CONFIG_PATH.read_text("utf-8"))
            else:
                USER_CONFIG_DIR.mkdir(parents=True, exist_ok=True)
                defaults = json.loads(DEFAULT_CONFIG_PATH.read_text("utf-8"))
                USER_CONFIG_PATH.write_text(json.dumps(defaults, indent=2, ensure_ascii=False), "utf-8")
                self.data = defaults

    def reset(self) -> None:
        with self._lock:
            defaults = json.loads(DEFAULT_CONFIG_PATH.read_text("utf-8"))
            USER_CONFIG_PATH.write_text(json.dumps(defaults, indent=2, ensure_ascii=False), "utf-8")
            self.data = defaults
        self._fire()

    def save(self, new_data: dict[str, Any]) -> tuple[bool, str]:
        # Light validation: required top-level keys.
        for key in ("effects", "event_bindings", "event_priority", "board", "server"):
            if key not in new_data:
                return False, f"missing key: {key}"
        try:
            json.dumps(new_data)
        except (TypeError, ValueError) as exc:
            return False, f"not JSON serializable: {exc}"
        with self._lock:
            USER_CONFIG_PATH.write_text(json.dumps(new_data, indent=2, ensure_ascii=False), "utf-8")
            self.data = new_data
        self._fire()
        return True, "ok"

    def on_change(self, fn: Callable[[], None]) -> None:
        self._listeners.append(fn)

    def _fire(self) -> None:
        for fn in list(self._listeners):
            try:
                fn()
            except Exception as exc:
                print(f"[config] listener error: {exc}", file=sys.stderr)

    def effects(self) -> dict[str, dict[str, Any]]:
        """Return effects with the active profile's overrides applied + brightness scaled."""
        base = {e["id"]: dict(e) for e in self.data.get("effects", [])}
        display = self.data.get("display", {})
        profile_name = display.get("profile", "vivid")
        profiles = self.data.get("profiles", {})
        profile = profiles.get(profile_name) or {}
        overrides = profile.get("overrides", {})
        for eid, override in overrides.items():
            if eid in base:
                merged = dict(base[eid])
                # Override only the keys present; preserve color_hint/name unless given.
                for k, v in override.items():
                    merged[k] = v
                base[eid] = merged
        brightness = float(display.get("brightness", 1.0))
        if abs(brightness - 1.0) > 0.01:
            for eid, eff in base.items():
                eff["frames"] = [self._scale_frame(f, brightness) for f in eff.get("frames", [])]
        return base

    @staticmethod
    def _scale_frame(frame: dict, scale: float) -> dict:
        f = dict(frame)
        for k in ("r", "y", "g"):
            v = f.get(k, 0)
            if v is None:
                continue
            f[k] = max(0, min(255, int(round(v * scale))))
        return f

    def display(self) -> dict[str, Any]:
        return self.data.get("display", {})

    def set_display(self, **kwargs) -> None:
        with self._lock:
            disp = self.data.setdefault("display", {})
            for k, v in kwargs.items():
                disp[k] = v
            USER_CONFIG_PATH.write_text(json.dumps(self.data, indent=2, ensure_ascii=False), "utf-8")
        self._fire()

    def profiles(self) -> dict[str, dict[str, Any]]:
        return self.data.get("profiles", {})

    def effect_for_event(self, event: str) -> str:
        bindings = self.data.get("event_bindings", {})
        return bindings.get(event) or "idle"

    def priority_index(self, event: str) -> int:
        prio = self.data.get("event_priority", [])
        try:
            return prio.index(event)
        except ValueError:
            return 10_000  # unknown events lose

    def board(self) -> dict[str, Any]:
        return self.data.get("board", {})

    def server(self) -> dict[str, Any]:
        return self.data.get("server", {})

    def session_cfg(self) -> dict[str, Any]:
        return self.data.get("session", {})


# ============================================================================
# Sessions
# ============================================================================

@dataclass
class Session:
    sid: str
    event: str
    cwd: str = ""
    pid: int = 0
    agent: str = "claude"
    updated_at: float = field(default_factory=time.time)

    def to_dict(self) -> dict[str, Any]:
        return {
            "sid": self.sid,
            "event": self.event,
            "cwd": self.cwd,
            "pid": self.pid,
            "agent": self.agent,
            "updated_at": self.updated_at,
            "age_s": round(time.time() - self.updated_at, 1),
        }


class Sessions:
    def __init__(self, cfg: Config, on_change: Callable[[], None]) -> None:
        self._cfg = cfg
        self._lock = threading.RLock()
        self._items: dict[str, Session] = {}
        self._on_change = on_change
        self._stop = threading.Event()
        threading.Thread(target=self._sweeper, daemon=True, name="session-sweep").start()

    def update(self, sid: str, event: str, cwd: str = "", pid: int = 0, agent: str = "claude") -> None:
        with self._lock:
            self._items[sid] = Session(sid=sid, event=event, cwd=cwd, pid=pid, agent=agent)
        self._on_change()

    def remove(self, sid: str) -> None:
        with self._lock:
            self._items.pop(sid, None)
        self._on_change()

    def snapshot(self) -> list[dict[str, Any]]:
        with self._lock:
            return [s.to_dict() for s in self._items.values()]

    def _filter(self, agent_filter: str) -> list[Session]:
        if agent_filter == "all" or not agent_filter:
            return list(self._items.values())
        return [s for s in self._items.values() if s.agent == agent_filter]

    def winner(self, agent_filter: str = "all") -> tuple[str, str | None]:
        """Returns (winning_event, winning_sid). winning_sid None if no sessions."""
        with self._lock:
            items = self._filter(agent_filter)
            if not items:
                return "_idle", None
            best_event = "_idle"
            best_sid = None
            best_rank = 10_001
            for s in items:
                rank = self._cfg.priority_index(s.event)
                if rank < best_rank:
                    best_rank = rank
                    best_event = s.event
                    best_sid = s.sid
            return best_event, best_sid

    WORKING_EVENTS = {
        "PreToolUse", "PostToolUse", "PostToolBatch",
        "PreCompact", "PostCompact",
        "SubagentStart", "SubagentStop",
        "UserPromptSubmit", "UserPromptExpansion",
        "ElicitationResult",
        "PostToolUse:AskUserQuestion",
    }

    def working_count(self, agent_filter: str = "all") -> int:
        """How many sessions are currently in a 'work' state. Used for tempo."""
        with self._lock:
            return sum(1 for s in self._filter(agent_filter) if s.event in self.WORKING_EVENTS)

    def count(self, agent_filter: str = "all") -> int:
        with self._lock:
            return len(self._filter(agent_filter))

    def _sweeper(self) -> None:
        while not self._stop.is_set():
            sleep_s = float(self._cfg.session_cfg().get("sweep_seconds", 30))
            ttl = float(self._cfg.session_cfg().get("ttl_seconds", 600))
            time.sleep(max(2.0, sleep_s))
            changed = False
            now = time.time()
            with self._lock:
                for sid, s in list(self._items.items()):
                    if now - s.updated_at > ttl:
                        del self._items[sid]
                        changed = True
                        continue
                    if s.pid and not _pid_alive(s.pid):
                        del self._items[sid]
                        changed = True
            if changed:
                self._on_change()


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return True
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # We don't have permission to signal it — that means the pid is in use
        # by another user (init/system process counts as alive).
        return True
    except OSError:
        return False


# ============================================================================
# Board client (long-lived TCP to ESP32, with heartbeat + reconnect)
# ============================================================================

class BoardClient:
    """
    Maintains a single TCP connection to the ESP32. Sends one JSON object per
    line. Heartbeats keep the link warm and detect half-open sockets.
    """

    def __init__(self, cfg: Config) -> None:
        self._cfg = cfg
        self._lock = threading.RLock()
        self._sock: socket.socket | None = None
        self._send_q: queue.Queue[dict | None] = queue.Queue(maxsize=64)
        self._status: dict[str, Any] = {
            "online": False,
            "host": "",
            "port": 0,
            "last_send_at": 0.0,
            "last_recv_at": 0.0,
            "last_error": "",
            "rtt_ms": 0.0,
            "connects": 0,
            "errors": 0,
        }
        self._stop = threading.Event()
        threading.Thread(target=self._connect_loop, daemon=True, name="board-conn").start()
        threading.Thread(target=self._heartbeat_loop, daemon=True, name="board-hb").start()

    def send_effect(self, effect: dict[str, Any]) -> None:
        msg = {"type": "effect", "effect": effect}
        try:
            self._send_q.put_nowait(msg)
        except queue.Full:
            self._status["last_error"] = "send queue full"

    def status(self) -> dict[str, Any]:
        with self._lock:
            return dict(self._status)

    # ------ internals -----------------------------------------------------

    def _board_addr(self) -> tuple[str, int]:
        b = self._cfg.board()
        return str(b.get("host", "127.0.0.1")), int(b.get("port", 8080))

    def _connect_loop(self) -> None:
        while not self._stop.is_set():
            host, port = self._board_addr()
            self._status["host"] = host
            self._status["port"] = port
            sock = None
            try:
                sock = socket.create_connection((host, port), timeout=3.0)
                sock.settimeout(None)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                with self._lock:
                    self._sock = sock
                    self._status["online"] = True
                    self._status["connects"] += 1
                    self._status["last_error"] = ""
                self._pump(sock)
            except OSError as exc:
                with self._lock:
                    self._status["online"] = False
                    self._status["last_error"] = str(exc)
                    self._status["errors"] += 1
            finally:
                with self._lock:
                    self._sock = None
                    self._status["online"] = False
                if sock is not None:
                    try:
                        sock.close()
                    except OSError:
                        pass
            time.sleep(max(0.5, int(self._cfg.board().get("reconnect_ms", 2000)) / 1000.0))

    def _pump(self, sock: socket.socket) -> None:
        # Drain queue → socket. Block at most 1s so we can notice socket death.
        while not self._stop.is_set():
            try:
                msg = self._send_q.get(timeout=1.0)
            except queue.Empty:
                # passive probe: a quick zero-length send won't trigger but
                # readable check will detect remote close.
                if _peer_closed(sock):
                    raise OSError("peer closed")
                continue
            if msg is None:
                return
            line = (json.dumps(msg, separators=(",", ":")) + "\n").encode("utf-8")
            t0 = time.time()
            try:
                sock.sendall(line)
            except OSError as exc:
                with self._lock:
                    self._status["last_error"] = f"send: {exc}"
                raise
            with self._lock:
                self._status["last_send_at"] = t0
                self._status["rtt_ms"] = round((time.time() - t0) * 1000.0, 2)

    def _heartbeat_loop(self) -> None:
        while not self._stop.is_set():
            interval = max(1.0, int(self._cfg.board().get("heartbeat_ms", 5000)) / 1000.0)
            time.sleep(interval)
            try:
                self._send_q.put_nowait({"type": "ping", "ts": int(time.time())})
            except queue.Full:
                pass


def _peer_closed(sock: socket.socket) -> bool:
    import select
    r, _, _ = select.select([sock], [], [], 0)
    if not r:
        return False
    try:
        data = sock.recv(64, socket.MSG_PEEK)
        return len(data) == 0
    except OSError:
        return True


# ============================================================================
# Scheduler — picks current effect, drives BoardClient + SSE listeners
# ============================================================================

class Scheduler:
    TURN_END_EFFECT = "done"  # the brief green blink
    TURN_END_SECONDS = 1.5
    ALERT_EFFECTS = {"permission", "blocked", "attention", "offline"}
    WORK_EFFECTS = {"working", "thinking"}
    # tempo caps + curve
    TEMPO_MAX = 3.5
    TEMPO_K = 0.4  # 1 + K*(n-1), capped at TEMPO_MAX

    def __init__(self, cfg: Config, sessions: Sessions, board: BoardClient) -> None:
        self._cfg = cfg
        self._sessions = sessions
        self._board = board
        self._lock = threading.RLock()
        self._override: str | None = None
        self._override_until: float = 0.0
        self._override_reason: str = ""  # "manual" or "turn_end"
        self._current_effect_id: str = "off"
        self._current_tempo: float = 1.0
        self._agent_filter: str = "all"  # display scope
        self._listeners: list[Callable[[dict[str, Any]], None]] = []

    def on_state(self, fn: Callable[[dict[str, Any]], None]) -> None:
        self._listeners.append(fn)

    def set_agent_filter(self, scope: str) -> bool:
        if scope not in ("all", "claude", "codex"):
            return False
        with self._lock:
            self._agent_filter = scope
        self.evaluate(force=True)
        return True

    def agent_filter(self) -> str:
        return self._agent_filter

    def manual_override(self, effect_id: str, hold_seconds: float = 6.0) -> bool:
        if effect_id not in self._cfg.effects():
            return False
        with self._lock:
            self._override = effect_id
            self._override_until = time.time() + max(0.5, hold_seconds)
            self._override_reason = "manual"
        self.evaluate(force=True)
        return True

    def cue_turn_end(self) -> None:
        """Show the brief 'done' blink, but only if no alert (red/yellow) is current."""
        with self._lock:
            # If a stronger alert is already running, don't overwrite it.
            winning_event, _ = self._sessions.winner(self._agent_filter)
            cur = self._cfg.effect_for_event(winning_event) if winning_event != "_idle" else "idle"
            if cur in self.ALERT_EFFECTS:
                return
            # Don't replace a more-important manual override either.
            if self._override and self._override_reason == "manual":
                return
            self._override = self.TURN_END_EFFECT
            self._override_until = time.time() + self.TURN_END_SECONDS
            self._override_reason = "turn_end"
        self.evaluate(force=True)

    def event_to_effect(self, event: str) -> str:
        return self._cfg.effect_for_event(event)

    def current_effect_id(self) -> str:
        return self._current_effect_id

    def _compute_tempo(self, effect_id: str) -> float:
        """Speed up work-class effects when there are more concurrent workers."""
        if not self._cfg.display().get("tempo_enabled", True):
            return 1.0
        if effect_id not in self.WORK_EFFECTS:
            return 1.0
        n = self._sessions.working_count(self._agent_filter)
        if n <= 1:
            return 1.0
        return min(self.TEMPO_MAX, 1.0 + self.TEMPO_K * (n - 1))

    def evaluate(self, force: bool = False) -> None:
        with self._lock:
            now = time.time()
            chosen: str
            if self._override and now < self._override_until:
                chosen = self._override
            else:
                if self._override and now >= self._override_until:
                    self._override = None
                    self._override_reason = ""
                if not self._board.status()["online"]:
                    chosen = "offline"
                else:
                    winning_event, _ = self._sessions.winner(self._agent_filter)
                    chosen = "idle" if winning_event == "_idle" else self.event_to_effect(winning_event)
            tempo = self._compute_tempo(chosen)
            if not force and chosen == self._current_effect_id and abs(tempo - self._current_tempo) < 0.05:
                return
            self._current_effect_id = chosen
            self._current_tempo = tempo

        effects = self._cfg.effects()
        eff = dict(effects.get(self._current_effect_id) or effects.get("off") or {"id": "off", "frames": []})
        eff["tempo"] = round(self._current_tempo, 2)
        self._board.send_effect(eff)
        self._broadcast()

    def snapshot(self) -> dict[str, Any]:
        effects = self._cfg.effects()
        eff = effects.get(self._current_effect_id, {})
        winning_event, winning_sid = self._sessions.winner(self._agent_filter)
        disp = self._cfg.display()
        profiles = self._cfg.profiles()
        return {
            "effect_id": self._current_effect_id,
            "effect": eff,
            "tempo": round(self._current_tempo, 2),
            "display": {
                "profile": disp.get("profile", "vivid"),
                "brightness": disp.get("brightness", 1.0),
                "tempo_enabled": disp.get("tempo_enabled", True),
                "available_profiles": [
                    {"id": k, "name": v.get("name", k), "summary": v.get("summary", "")}
                    for k, v in profiles.items()
                ],
            },
            "board": self._board.status(),
            "sessions": self._sessions.snapshot(),
            "winner": {
                "sid": winning_sid,
                "event": None if winning_event == "_idle" else winning_event,
            },
            "agent_filter": self._agent_filter,
            "counts": {
                "all": self._sessions.count("all"),
                "claude": self._sessions.count("claude"),
                "codex": self._sessions.count("codex"),
                "working": self._sessions.working_count(self._agent_filter),
            },
            "override": {
                "effect_id": self._override,
                "reason": self._override_reason,
                "remaining_s": max(0.0, round(self._override_until - time.time(), 1)) if self._override else 0.0,
            },
        }

    def _broadcast(self) -> None:
        snap = self.snapshot()
        for fn in list(self._listeners):
            try:
                fn(snap)
            except Exception as exc:
                print(f"[sched] listener error: {exc}", file=sys.stderr)


# ============================================================================
# SSE hub
# ============================================================================

class SseHub:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._clients: list[queue.Queue[str]] = []

    def add(self) -> queue.Queue[str]:
        q: queue.Queue[str] = queue.Queue(maxsize=32)
        with self._lock:
            self._clients.append(q)
        return q

    def remove(self, q: queue.Queue[str]) -> None:
        with self._lock:
            try:
                self._clients.remove(q)
            except ValueError:
                pass

    def broadcast(self, event: str, data: Any) -> None:
        payload = f"event: {event}\ndata: {json.dumps(data, ensure_ascii=False)}\n\n"
        with self._lock:
            dead = []
            for q in self._clients:
                try:
                    q.put_nowait(payload)
                except queue.Full:
                    dead.append(q)
            for q in dead:
                try:
                    self._clients.remove(q)
                except ValueError:
                    pass


# ============================================================================
# HTTP handler
# ============================================================================

# Globals set by main()
CFG: Config
SESS: Sessions
BOARD: BoardClient
SCHED: Scheduler
SSE: SseHub


class Handler(BaseHTTPRequestHandler):
    server_version = "SignalLight/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:  # quieter logs
        return

    # ------ helpers -------------------------------------------------------

    def _host_ok(self) -> bool:
        host = self.headers.get("Host", "")
        host_only = host.split(":")[0]
        return host_only in ("127.0.0.1", "localhost", "::1", "")

    def _send_bytes(self, code: int, body: bytes, ctype: str = "text/plain; charset=utf-8") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, code: int, obj: Any) -> None:
        self._send_bytes(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"), "application/json")

    def _send_file(self, path: Path, ctype: str) -> None:
        if not path.exists():
            self._send_bytes(404, b"not found")
            return
        body = path.read_bytes()
        self._send_bytes(200, body, ctype)

    # ------ GET -----------------------------------------------------------

    def do_GET(self) -> None:
        if not self._host_ok():
            self._send_bytes(403, b"forbidden")
            return
        url = urlparse(self.path)
        path = url.path
        if path == "/":
            self._send_file(WEBUI_DIR / "index.html", "text/html; charset=utf-8")
            return
        if path == "/style.css":
            self._send_file(WEBUI_DIR / "style.css", "text/css; charset=utf-8")
            return
        if path == "/app.js":
            self._send_file(WEBUI_DIR / "app.js", "application/javascript; charset=utf-8")
            return
        if path == "/api/status":
            self._send_json(200, SCHED.snapshot())
            return
        if path == "/api/config":
            self._send_json(200, CFG.data)
            return
        if path == "/stream":
            self._serve_sse()
            return
        self._send_bytes(404, b"not found")

    def _serve_sse(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        q = SSE.add()
        try:
            # initial snapshot
            init = f"event: snapshot\ndata: {json.dumps(SCHED.snapshot(), ensure_ascii=False)}\n\n"
            self.wfile.write(init.encode("utf-8"))
            self.wfile.flush()
            while True:
                try:
                    msg = q.get(timeout=15.0)
                except queue.Empty:
                    msg = "event: ping\ndata: {}\n\n"
                try:
                    self.wfile.write(msg.encode("utf-8"))
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    return
        finally:
            SSE.remove(q)

    # ------ POST ----------------------------------------------------------

    def do_POST(self) -> None:
        if not self._host_ok():
            self._send_bytes(403, b"forbidden")
            return
        url = urlparse(self.path)
        path = url.path
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length > 0 else b""

        if path == "/hook":
            self._do_hook(raw)
            return
        if path == "/api/manual":
            self._do_manual(raw)
            return
        if path == "/api/config":
            self._do_save_config(raw)
            return
        if path == "/api/config/reset":
            CFG.reset()
            SCHED.evaluate(force=True)
            self._send_json(200, CFG.data)
            return
        if path == "/api/session/remove":
            self._do_session_remove(raw)
            return
        if path == "/api/agent-filter":
            self._do_agent_filter(raw)
            return
        if path == "/api/display":
            self._do_display(raw)
            return
        self._send_bytes(404, b"not found")

    # Events we deliberately do not act on. See README "Ignored events".
    IGNORED_EVENTS = {
        "MessageDisplay",          # token-streamed, would thrash the light
        "InstructionsLoaded",      # boot-time, no visual meaning
        "ConfigChange",            # settings.json edits
        "CwdChanged", "FileChanged",
        "WorktreeCreate", "WorktreeRemove",
        "Setup",                   # --init / --maintenance
        "TaskCreated", "TaskCompleted",  # internal task tracking
        "TeammateIdle",            # multi-agent teammate coordination
    }

    # StopFailure error_type → effect
    STOP_FAILURE_MAP = {
        "rate_limit":           "_attention",   # transient, just a heads-up
        "overloaded":           "_attention",
        "server_error":         "_attention",
        "max_output_tokens":    "_attention",
        # others (authentication_failed, oauth_org_not_allowed, billing_error,
        # invalid_request, model_not_found, unknown) → blocked, hard stop
    }

    # Notification subtype → effect
    NOTIFICATION_MAP = {
        "permission_prompt":   "_permission",
        "elicitation_dialog":  "_permission",
        "idle_prompt":         "_attention",
        "elicitation_complete": None,  # informational, no state change
        "elicitation_response": None,
        "auth_success":        None,
    }

    def _do_hook(self, raw: bytes) -> None:
        try:
            payload = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            payload = {}

        event = str(payload.get("hook_event_name") or payload.get("event") or "")
        sid = str(payload.get("session_id") or payload.get("sid") or "default")
        cwd = str(payload.get("cwd") or (payload.get("workspace", {}) or {}).get("current_dir") or "")
        pid = int(payload.get("pid") or 0)
        agent = str(payload.get("agent") or "claude")
        tool_name = str(payload.get("tool_name") or "")

        # 1) Explicit ignore list — silent drop.
        if event in self.IGNORED_EVENTS:
            self._send_json(200, {"ok": True, "ignored": event})
            return

        # 2) Session lifecycle events that change session map, not just the light.
        if event == "SessionEnd":
            SESS.remove(sid)
            SCHED.evaluate()
            self._send_json(200, {"ok": True})
            return

        # 3) Special event mapping with payload-driven branches.
        cue_turn_end = False
        if event == "Stop":
            stop_reason = str(payload.get("stop_reason") or "")
            if stop_reason in ("error", "max_tokens"):
                event = "_blocked"
            else:
                # Normal Stop: drop session, flash green, do not record.
                SESS.remove(sid)
                cue_turn_end = True
                event = ""
        elif event == "StopFailure":
            error_type = str(payload.get("error_type") or "")
            event = self.STOP_FAILURE_MAP.get(error_type, "_blocked")
            # StopFailure also ends the turn — drop session so it doesn't linger.
            SESS.remove(sid)
        elif event == "Notification":
            ntype = str(payload.get("notification_type") or payload.get("type") or "")
            mapped = self.NOTIFICATION_MAP.get(ntype)
            if mapped is None:
                # No state change for these subtypes; just ack.
                self._send_json(200, {"ok": True, "ignored_subtype": ntype})
                return
            event = mapped
        elif event == "PostToolUse":
            # Claude Code's PostToolUse fires for both success and failure.
            # Failure is signalled by tool_response.error or top-level error/tool_error.
            tool_resp = payload.get("tool_response") or {}
            has_error = bool(
                payload.get("tool_error")
                or payload.get("error")
                or (isinstance(tool_resp, dict) and tool_resp.get("error"))
            )
            if has_error:
                event = "_blocked"
            elif tool_name:
                # Tool-level binding: e.g. PostToolUse:AskUserQuestion → working
                tool_event = f"PostToolUse:{tool_name}"
                if tool_event in (CFG.data.get("event_bindings") or {}):
                    event = tool_event
        elif event == "PreToolUse":
            # Tool-level binding: PreToolUse:AskUserQuestion → attention
            if tool_name:
                tool_event = f"PreToolUse:{tool_name}"
                if tool_event in (CFG.data.get("event_bindings") or {}):
                    event = tool_event
        # PostToolUseFailure / PermissionDenied are already directly mapped to blocked
        # via the bindings; no extra translation needed here.

        if event:
            SESS.update(sid, event, cwd=cwd, pid=pid, agent=agent)
        if cue_turn_end:
            SCHED.cue_turn_end()
        else:
            SCHED.evaluate()
        self._send_json(200, {"ok": True, "event": event, "sid": sid})

    def _do_manual(self, raw: bytes) -> None:
        try:
            payload = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            payload = {}
        effect_id = str(payload.get("effect") or "")
        hold = float(payload.get("hold_seconds") or 6.0)
        ok = SCHED.manual_override(effect_id, hold)
        if not ok:
            self._send_json(400, {"ok": False, "error": f"unknown effect: {effect_id}"})
            return
        self._send_json(200, {"ok": True, "effect": effect_id, "hold_seconds": hold})

    def _do_save_config(self, raw: bytes) -> None:
        try:
            payload = json.loads(raw or b"{}")
        except json.JSONDecodeError as exc:
            self._send_json(400, {"ok": False, "error": f"bad JSON: {exc}"})
            return
        ok, msg = CFG.save(payload)
        if not ok:
            self._send_json(400, {"ok": False, "error": msg})
            return
        SCHED.evaluate(force=True)
        self._send_json(200, {"ok": True})

    def _do_session_remove(self, raw: bytes) -> None:
        try:
            payload = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            payload = {}
        sid = str(payload.get("sid") or "")
        if sid:
            SESS.remove(sid)
            SCHED.evaluate()
        self._send_json(200, {"ok": True})

    def _do_agent_filter(self, raw: bytes) -> None:
        try:
            payload = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            payload = {}
        scope = str(payload.get("scope") or "").lower()
        ok = SCHED.set_agent_filter(scope)
        if not ok:
            self._send_json(400, {"ok": False, "error": f"bad scope: {scope}"})
            return
        self._send_json(200, {"ok": True, "scope": scope})

    def _do_display(self, raw: bytes) -> None:
        try:
            payload = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            payload = {}
        updates: dict[str, Any] = {}
        if "profile" in payload:
            name = str(payload["profile"])
            if name not in CFG.profiles():
                self._send_json(400, {"ok": False, "error": f"unknown profile: {name}"})
                return
            updates["profile"] = name
        if "brightness" in payload:
            try:
                b = float(payload["brightness"])
            except (TypeError, ValueError):
                self._send_json(400, {"ok": False, "error": "brightness must be a number"})
                return
            updates["brightness"] = max(0.05, min(1.0, b))
        if "tempo_enabled" in payload:
            updates["tempo_enabled"] = bool(payload["tempo_enabled"])
        if not updates:
            self._send_json(400, {"ok": False, "error": "no recognized fields"})
            return
        CFG.set_display(**updates)
        SCHED.evaluate(force=True)
        self._send_json(200, {"ok": True, "display": CFG.display()})


# ============================================================================
# main
# ============================================================================

def main() -> int:
    global CFG, SESS, BOARD, SCHED, SSE
    CFG = Config()
    SSE = SseHub()

    def on_change_broadcast() -> None:
        SSE.broadcast("snapshot", SCHED.snapshot())

    BOARD = BoardClient(CFG)
    SESS = Sessions(CFG, on_change=lambda: SCHED.evaluate())
    SCHED = Scheduler(CFG, SESS, BOARD)
    SCHED.on_state(lambda snap: SSE.broadcast("snapshot", snap))

    # poll board status changes into evaluate()
    def board_watch() -> None:
        prev = None
        while True:
            cur = BOARD.status()["online"]
            if cur != prev:
                prev = cur
                SCHED.evaluate(force=True)
            time.sleep(0.5)
    threading.Thread(target=board_watch, daemon=True, name="board-watch").start()

    # tick: re-evaluate once a second so timed overrides expire on time
    def ticker() -> None:
        while True:
            time.sleep(1.0)
            SCHED.evaluate()
    threading.Thread(target=ticker, daemon=True, name="ticker").start()

    srv_cfg = CFG.server()
    host = str(srv_cfg.get("host", "127.0.0.1"))
    port = int(srv_cfg.get("port", 7878))

    SCHED.evaluate(force=True)

    httpd = ThreadingHTTPServer((host, port), Handler)
    print(f"signal-light daemon listening on http://{host}:{port}")
    print(f"  user config: {USER_CONFIG_PATH}")
    print(f"  board target: {CFG.board().get('host')}:{CFG.board().get('port')}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

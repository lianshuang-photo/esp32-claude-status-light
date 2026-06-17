#!/usr/bin/env python3
"""
Bridge Claude Code status/hooks to ESP32-C3 TCP LED states.

This is an inferred reconstruction from the video, not the author's original
source file. It intentionally keeps the visible constants and event mappings
from the video where readable.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
from pathlib import Path
from typing import Any


DEFAULT_HOST = os.environ.get("ESP32_LED_HOST", "192.168.31.106")
DEFAULT_PORT = int(os.environ.get("ESP32_LED_PORT", "8080"))
DEFAULT_TIMEOUT_SECONDS = float(os.environ.get("ESP32_LED_TIMEOUT", "0.35"))

EVENT_TO_STATE = {
    "SessionStart": "off",
    "UserPromptSubmit": "thinking",
    "UserPromptExpansion": "thinking",
    "PreToolUse": "busy",
    "PostToolUse": "thinking",
    "PostToolUseFailure": "error",
    "PostToolBatch": "thinking",
    "PermissionDenied": "error",
    "PermissionRequest": "alarm",
    "SubagentStart": "thinking",
    "SubagentStop": "thinking",
    "TaskCreated": "busy",
    "TaskCompleted": "thinking",
    "PreCompact": "busy",
    "PostCompact": "thinking",
    "Stop": "success",
    "StopFailure": "error",
    "TeammateIdle": "off",
    "SessionEnd": "off",
}

NOTIFICATION_TO_STATE = {
    "permission_prompt": "alarm",
    "elicitation_dialog": "alarm",
    "idle_prompt": "off",
    "auth_success": "success",
    "elicitation_complete": "thinking",
    "elicitation_response": "thinking",
}


def load_stdin_json() -> dict[str, Any]:
    try:
        raw = sys.stdin.read()
    except Exception:
        return {}

    if not raw.strip():
        return {}

    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return {"raw": raw}


def send_state(host: str, port: int, state: str, timeout: float) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall((state + "\n").encode("utf-8"))
        return True
    except OSError as exc:
        print(f"TCP send failed: {exc}", file=sys.stderr)
        return False


def normalize_state(state: str | None) -> str | None:
    if not state:
        return None

    state = state.strip().lower()
    if state.startswith("state "):
        state = state[6:].strip()

    if state in {"thinking", "ai", "busy", "success", "error", "alarm", "off"}:
        return state
    return None


def infer_state(payload: dict[str, Any], explicit_state: str | None) -> str | None:
    explicit_state = normalize_state(explicit_state)
    if explicit_state:
        return explicit_state

    event = str(payload.get("hook_event_name", ""))

    notification_type = payload.get("notification_type") or payload.get("type")
    if notification_type in NOTIFICATION_TO_STATE:
        return NOTIFICATION_TO_STATE[str(notification_type)]

    if event == "Stop":
        background_tasks = payload.get("background_tasks")
        session_crons = payload.get("session_crons")
        if background_tasks or session_crons:
            return "busy"
        return "success"

    return EVENT_TO_STATE.get(event)


def statusline_text(payload: dict[str, Any], state: str | None, ok: bool) -> str:
    model = payload.get("model", {})
    workspace = payload.get("workspace", {})

    model_name = model.get("display_name") or model.get("id") or "Claude"
    current_dir = workspace.get("current_dir") or payload.get("cwd") or os.getcwd()
    dirname = Path(str(current_dir)).name or str(current_dir)

    connection = "ESP32 ok" if ok else "ESP32 offline"
    led = state or "status"
    return f"[{model_name}] {dirname} | LED {led} | {connection}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bridge Claude Code status/hooks to ESP32-C3 TCP LED states."
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="ESP32-C3 IP address")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="ESP32-C3 TCP port")
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="TCP timeout seconds",
    )
    parser.add_argument(
        "--state",
        help="Explicit LED state",
    )
    parser.add_argument(
        "--mode",
        choices=["send", "hook", "statusline"],
        default="hook",
        help="send: direct CLI test; hook: Claude hook; statusline: Claude status line",
    )
    parser.add_argument(
        "--statusline-send",
        action="store_true",
        help="Also send inferred state when running in statusline mode",
    )
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        payload = load_stdin_json()
        state = infer_state(payload, args.state)
        ok = True

        if args.mode == "send":
            if not state:
                print("Missing --state", file=sys.stderr)
                return 2
            ok = send_state(args.host, args.port, state, args.timeout)
            print(f"{'ok' if ok else 'error'}: {state}")
            return 0 if ok else 1

        if args.mode == "hook":
            if state:
                send_state(args.host, args.port, state, args.timeout)
            return 0

        if args.mode == "statusline":
            if args.statusline_send and state:
                ok = send_state(args.host, args.port, state, args.timeout)
            print(statusline_text(payload, state, ok))
            return 0

        return 0
    except Exception as exc:
        print(f"Error in main: {exc}", file=sys.stderr)
        import traceback

        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

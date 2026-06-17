#!/usr/bin/env python3
"""
Thin hook forwarder. Designed for Claude Code's `settings.json` hooks, also
usable from Codex hooks.

Reads the agent's JSON hook payload from stdin, augments it with our pid and
agent name, POSTs it to the local daemon, and always exits 0 even on failure
— so a stopped or unreachable daemon never blocks the agent.

Total wall time is capped at ~0.5s (HOOK_DAEMON_TIMEOUT).

Usage:
  cat claude_event.json | hook_client.py                       # defaults to claude
  cat codex_event.json  | hook_client.py --agent codex
  hook_client.py --agent codex --event UserPromptSubmit < ...  # codex passes
                                                                 event name via
                                                                 argument, not
                                                                 stdin field

Env:
  HOOK_DAEMON_URL     default http://127.0.0.1:7878/hook
  HOOK_DAEMON_TIMEOUT default 0.5
  HOOK_AGENT          default claude
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(add_help=False)
    p.add_argument("--agent", default=os.environ.get("HOOK_AGENT", "claude"))
    p.add_argument("--event", default=None,
                   help="Override hook_event_name (useful for Codex, which passes the event as an argument)")
    p.add_argument("--url", default=os.environ.get("HOOK_DAEMON_URL", "http://127.0.0.1:7878/hook"))
    p.add_argument("--timeout", type=float, default=float(os.environ.get("HOOK_DAEMON_TIMEOUT", "0.5")))
    # Accept (and ignore) any positional args so callers can pass event name positionally too:
    #   ./hook_client.py --agent codex UserPromptSubmit
    p.add_argument("rest", nargs="*")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    try:
        raw = sys.stdin.read()
    except Exception:
        raw = ""

    try:
        payload = json.loads(raw) if raw.strip() else {}
        if not isinstance(payload, dict):
            payload = {"raw": payload}
    except json.JSONDecodeError:
        payload = {"raw": raw}

    # Event name resolution: --event > first positional > stdin field
    event = args.event or (args.rest[0] if args.rest else None)
    if event:
        payload["hook_event_name"] = event

    payload.setdefault("pid", os.getppid())
    payload["agent"] = args.agent  # always trust CLI/env, since stdin may lie

    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        args.url,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=args.timeout):
            pass
    except Exception:
        pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

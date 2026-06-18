#!/usr/bin/env python3
"""
Hook installer / uninstaller for signal-light.

Safe by default — running with no flags prints a dry-run plan and changes
nothing. Use --preview-merged to see the full merged config in /tmp without
touching real files. Use --apply to actually write (always makes a .bak first).

Targets:
  Claude Code  →  ~/.claude/settings.json
  Codex        →  ~/.codex/hooks.json     (or settings.json if hooks.json absent)

Behaviour:
  - Preserves your existing hooks on the same events (merges into the array)
  - Tags our entries with "_signal_light": true so uninstall can find them
  - Never touches hooks for events we don't manage

Examples:
  python3 install_hooks.py                 # dry-run, print plan only
  python3 install_hooks.py --preview-merged
  python3 install_hooks.py --apply --agent claude
  python3 install_hooks.py --apply --agent claude --agent codex
  python3 install_hooks.py --uninstall --apply
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
HOOK_CLIENT = HERE / "hook_client.py"
SIGNAL_DAEMON = HERE / "signal_daemon.py"

TAG_KEY = "_signal_light"
TAG_VAL = True

# Daemon health check — install_hooks is friendlier if it tells the user
# upfront whether the daemon is running.
DEFAULT_DAEMON_URL = os.environ.get("HOOK_DAEMON_URL", "http://127.0.0.1:7878/hook")
DEFAULT_DAEMON_STATUS = DEFAULT_DAEMON_URL.rsplit("/", 1)[0] + "/api/status"


def check_daemon() -> tuple[bool, str]:
    """Returns (running, detail). detail is empty if running, else a hint."""
    try:
        req = urllib.request.Request(DEFAULT_DAEMON_STATUS, method="GET")
        with urllib.request.urlopen(req, timeout=0.5) as r:
            data = json.loads(r.read().decode("utf-8"))
            board_online = data.get("board", {}).get("online", False)
            board_host = data.get("board", {}).get("host", "?")
            return True, f"board {board_host} {'online ✓' if board_online else 'OFFLINE — check board.host'}"
    except (urllib.error.URLError, socket.timeout, OSError) as exc:
        return False, f"daemon not reachable at {DEFAULT_DAEMON_URL.rsplit('/',1)[0]} ({exc.__class__.__name__})"


def check_python_works() -> bool:
    """Make sure python3 is callable and hook_client imports cleanly."""
    import subprocess
    try:
        # Run hook_client with empty stdin + a bogus URL we know won't connect,
        # so we get exit 0 (it always exits 0 on network failure) without
        # actually triggering anything. This proves the file parses and runs.
        r = subprocess.run(
            ["python3", str(HOOK_CLIENT), "--url", "http://127.0.0.1:1"],
            input="", capture_output=True, text=True, timeout=3,
        )
        return r.returncode == 0
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return False

# ------------------------------------------------------------------------------
# Claude Code: events whose hook the user usually expects us to attach to.
# We use these as the canonical list. Per-tool sub-bindings (PreToolUse with
# tool_name=AskUserQuestion) are handled by daemon-side parsing, not by
# settings.json matchers, so we just attach to PreToolUse globally.
# ------------------------------------------------------------------------------
CLAUDE_EVENTS = [
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PreCompact",
    "PostCompact",
    "SubagentStart",
    "SubagentStop",
    "PermissionRequest",
    "Notification",
    "Stop",
    "StopFailure",
    "SessionEnd",
]

# Codex emits a subset; we only attach to events it actually fires.
CODEX_EVENTS = [
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PreCompact",
    "PostCompact",
    "SubagentStart",
    "SubagentStop",
    "PermissionRequest",
    "Stop",
]


def claude_settings_path() -> Path:
    return Path.home() / ".claude" / "settings.json"


def codex_hooks_path() -> Path:
    # Codex stores hooks in either ~/.codex/hooks.json (newer) or inside
    # ~/.codex/config.toml under [hooks]. We only handle the JSON form;
    # for TOML the install will fall back to print-only and tell the user.
    return Path.home() / ".codex" / "hooks.json"


def codex_toml_path() -> Path:
    return Path.home() / ".codex" / "config.toml"


def claude_hook_entry() -> dict[str, Any]:
    return {
        "type": "command",
        "command": f"python3 {HOOK_CLIENT}",
        "timeout": 10,
        TAG_KEY: TAG_VAL,
    }


def codex_hook_entry(event_name: str) -> dict[str, Any]:
    # Codex passes the event name as a positional CLI argument, not via stdin.
    return {
        "type": "command",
        "command": f"python3 {HOOK_CLIENT} --agent codex {event_name}",
        "timeout": 10,
        TAG_KEY: TAG_VAL,
    }


# ------------------------------------------------------------------------------
# Merge helpers
# ------------------------------------------------------------------------------

def merge_event(existing_groups: list[dict[str, Any]], new_entry: dict[str, Any]
                ) -> tuple[list[dict[str, Any]], str]:
    """
    Insert our hook entry into the existing groups list for an event.
    If our entry already exists (tag match), replace it. Otherwise append.
    Returns (new_groups, action_taken).
    """
    # find any existing signal-light group
    sig_group_idx = None
    sig_hook_idx = None
    for gi, group in enumerate(existing_groups or []):
        for hi, h in enumerate(group.get("hooks", []) or []):
            if isinstance(h, dict) and h.get(TAG_KEY) == TAG_VAL:
                sig_group_idx, sig_hook_idx = gi, hi
                break
        if sig_group_idx is not None:
            break

    new_groups = list(existing_groups or [])

    if sig_group_idx is None:
        # No existing signal-light hook — append a new group
        new_groups.append({
            "matcher": "",
            "hooks": [new_entry],
        })
        return new_groups, "added"
    else:
        # Replace the existing one
        new_groups[sig_group_idx]["hooks"][sig_hook_idx] = new_entry
        return new_groups, "updated"


def remove_signal_hooks_from_event(existing_groups: list[dict[str, Any]]
                                   ) -> tuple[list[dict[str, Any]], int]:
    """Remove our tagged hook from the groups. Returns (new_groups, removed_count)."""
    removed = 0
    new_groups = []
    for group in existing_groups or []:
        new_hooks = []
        for h in group.get("hooks", []) or []:
            if isinstance(h, dict) and h.get(TAG_KEY) == TAG_VAL:
                removed += 1
                continue
            new_hooks.append(h)
        if new_hooks:
            new_group = dict(group)
            new_group["hooks"] = new_hooks
            new_groups.append(new_group)
        # if no hooks left in the group, drop the whole group
    return new_groups, removed


def plan_apply(settings: dict[str, Any], events: list[str],
               entry_fn) -> tuple[dict[str, Any], list[str]]:
    """
    Returns (new_settings, log_lines). entry_fn(event_name) → hook entry dict.
    """
    settings = dict(settings) if settings else {}
    hooks = dict(settings.get("hooks", {}) or {})
    log = []
    for event in events:
        cur = hooks.get(event, [])
        new_groups, action = merge_event(cur, entry_fn(event))
        hooks[event] = new_groups
        # count existing non-ours hooks on this event so user knows we preserved them
        preserved = sum(
            1
            for group in (cur or [])
            for h in (group.get("hooks") or [])
            if not (isinstance(h, dict) and h.get(TAG_KEY) == TAG_VAL)
        )
        log.append(f"  {action:>7s}  {event:<24s}  (preserved {preserved} existing hook{'s' if preserved != 1 else ''})")
    settings["hooks"] = hooks
    return settings, log


def plan_uninstall(settings: dict[str, Any], events: list[str]) -> tuple[dict[str, Any], list[str]]:
    settings = dict(settings) if settings else {}
    hooks = dict(settings.get("hooks", {}) or {})
    log = []
    for event in list(hooks.keys()):
        cur = hooks[event]
        new_groups, removed = remove_signal_hooks_from_event(cur)
        if removed > 0:
            log.append(f"  removed  {event:<24s}  ({removed} entry)")
        if new_groups:
            hooks[event] = new_groups
        else:
            del hooks[event]
    settings["hooks"] = hooks
    return settings, log


def read_json(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text("utf-8"))
    except json.JSONDecodeError as exc:
        print(f"!! {path} is not valid JSON: {exc}", file=sys.stderr)
        sys.exit(2)


def backup_path(path: Path) -> Path:
    ts = time.strftime("%Y%m%d-%H%M%S")
    return path.with_suffix(path.suffix + f".bak.{ts}")


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", "utf-8")


# ------------------------------------------------------------------------------
# Drivers
# ------------------------------------------------------------------------------

def run_for_claude(args) -> None:
    path = claude_settings_path()
    print(f"\n=== Claude Code  ({path}) ===")
    cur = read_json(path)
    if args.uninstall:
        new, log = plan_uninstall(cur, CLAUDE_EVENTS)
        action = "uninstall"
    else:
        new, log = plan_apply(cur, CLAUDE_EVENTS, lambda e: claude_hook_entry())
        action = "install"
    if not log:
        print("  (nothing to change)")
        return
    for line in log:
        print(line)

    if args.preview_merged:
        out = Path("/tmp/signal-light-preview/claude-settings.json")
        write_json(out, new)
        print(f"\n  → preview written to {out}")
        return

    if args.apply:
        if path.exists():
            bak = backup_path(path)
            path.rename(bak)
            print(f"\n  ✓ backed up to {bak}")
        write_json(path, new)
        print(f"  ✓ {action}ed to {path}")
    else:
        print("\n  (dry-run only — re-run with --apply to write)")


def run_for_codex(args) -> None:
    json_path = codex_hooks_path()
    toml_path = codex_toml_path()
    print(f"\n=== Codex  ({json_path}) ===")

    if toml_path.exists() and not json_path.exists():
        print(f"  !! found {toml_path} but no hooks.json")
        print(f"  !! this installer only writes JSON; please paste the snippet")
        print(f"     from {HERE/'codex.hooks.example.json'} into your config.toml")
        return

    cur = read_json(json_path)
    if args.uninstall:
        new, log = plan_uninstall(cur, CODEX_EVENTS)
        action = "uninstall"
    else:
        new, log = plan_apply(cur, CODEX_EVENTS, lambda e: codex_hook_entry(e))
        action = "install"
    if not log:
        print("  (nothing to change)")
        return
    for line in log:
        print(line)

    if args.preview_merged:
        out = Path("/tmp/signal-light-preview/codex-hooks.json")
        write_json(out, new)
        print(f"\n  → preview written to {out}")
        return

    if args.apply:
        if json_path.exists():
            bak = backup_path(json_path)
            json_path.rename(bak)
            print(f"\n  ✓ backed up to {bak}")
        write_json(json_path, new)
        print(f"  ✓ {action}ed to {json_path}")
    else:
        print("\n  (dry-run only — re-run with --apply to write)")


def main() -> int:
    p = argparse.ArgumentParser(
        description="Install/uninstall signal-light hooks for Claude Code and Codex.")
    p.add_argument("--agent", action="append", choices=["claude", "codex"],
                   help="which agent(s) to target (default: both)")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--apply", action="store_true",
                   help="actually write the config (with .bak backup)")
    g.add_argument("--preview-merged", action="store_true",
                   help="write the merged result to /tmp instead of the real path")
    p.add_argument("--uninstall", action="store_true",
                   help="remove signal-light's hooks (preserves all others)")
    args = p.parse_args()

    agents = args.agent or ["claude", "codex"]

    if not args.apply and not args.preview_merged:
        print("=" * 64)
        print("DRY-RUN — nothing will be written.")
        print("  --preview-merged   → see full merged config in /tmp/")
        print("  --apply            → write for real (always backs up first)")
        print("  --uninstall        → remove our hooks (combine with --apply)")
        print("=" * 64)

    # Pre-flight: hook_client.py exists?
    print(f"\nhook_client: {HOOK_CLIENT}")
    if not HOOK_CLIENT.exists():
        print(f"  !! hook_client.py missing at {HOOK_CLIENT}", file=sys.stderr)
        return 2

    # Pre-flight: python3 works and hook_client imports?
    if not check_python_works():
        print(f"  !! 'python3 {HOOK_CLIENT.name} --help' failed.")
        print(f"     Check that python3 is on PATH and the file is intact.")
        return 2
    print(f"  ✓ python3 + hook_client OK")

    # Pre-flight: daemon running?
    running, detail = check_daemon()
    if running:
        print(f"  ✓ daemon running, {detail}")
    else:
        print(f"  ⚠ {detail}")
        print(f"     Hooks will still install, but won't drive the LED until you start the daemon:")
        print(f"       python3 {SIGNAL_DAEMON}")

    if "claude" in agents:
        run_for_claude(args)
    if "codex" in agents:
        run_for_codex(args)

    # Post-install guidance
    if args.apply and not args.uninstall:
        print()
        print("=" * 64)
        print("Installed. Next steps:")
        print()
        if not running:
            print(f"  1. Start the daemon:")
            print(f"       python3 {SIGNAL_DAEMON}")
            print()
        print(f"  2. Open http://127.0.0.1:7878 — confirm the board pill is green.")
        print()
        print(f"  3. Restart your agent session so it picks up the new hook config:")
        if "claude" in agents:
            print(f"       Claude Code: quit (Cmd+Q) and reopen, OR `/restart` in current session")
        if "codex" in agents:
            print(f"       Codex:       quit and reopen — Codex will prompt to TRUST the new hook")
            print(f"                    (this is its security feature, choose 'allow')")
        print()
        print(f"  To remove later:")
        print(f"       python3 {Path(__file__).name} --uninstall --apply")
        print("=" * 64)

    if args.uninstall and args.apply:
        print()
        print("=" * 64)
        print("Uninstalled. Your other hooks (if any) are intact.")
        print("Restart your agent session to drop the now-removed hook entries.")
        print("=" * 64)

    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

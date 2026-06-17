#!/usr/bin/env python3
"""
Local stand-in for the ESP32. Speaks the new JSON-line protocol so you can
exercise the daemon without hardware.

Protocol (each line is one JSON object):
  daemon → board:  {"type":"effect","effect":{"id":..., "frames":[{r,y,g,ms},...]}}
                   {"type":"ping","ts":...}
  board  → daemon: {"type":"pong","ts":...}      (optional; not required)
                   {"type":"hello","id":"..."}    (optional)

Run:
  python3 board_simulator.py --host 127.0.0.1 --port 8080
"""

from __future__ import annotations

import argparse
import json
import socketserver
import sys
from datetime import datetime


COLOR = {
    "green": "\033[92m",
    "yellow": "\033[93m",
    "red": "\033[91m",
    "cycle": "\033[96m",
    "off": "\033[90m",
}
RESET = "\033[0m"


class Handler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        peer = self.client_address
        print(f"{ts()} [conn] from {peer[0]}:{peer[1]}", flush=True)
        try:
            for raw in self.rfile:
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    print(f"{ts()} [bad ] {line!r}", flush=True)
                    continue
                kind = msg.get("type")
                if kind == "effect":
                    eff = msg.get("effect", {})
                    name = eff.get("name") or eff.get("id") or "?"
                    color = COLOR.get(eff.get("color_hint", "off"), "")
                    frames = eff.get("frames", [])
                    tempo = eff.get("tempo", 1.0)
                    tempo_tag = f" {tempo:.1f}×" if tempo != 1.0 else ""
                    print(f"{ts()} [eff ] {color}{name}{RESET}{tempo_tag} · {len(frames)} frame(s)", flush=True)
                elif kind == "ping":
                    # Echo a pong so daemon could measure RTT in future versions.
                    try:
                        self.wfile.write((json.dumps({"type": "pong", "ts": msg.get("ts")}) + "\n").encode("utf-8"))
                    except OSError:
                        return
                else:
                    print(f"{ts()} [?   ] {msg}", flush=True)
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            print(f"{ts()} [dis ] {peer[0]}:{peer[1]}", flush=True)


def ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


class ReusableServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    args = p.parse_args()
    with ReusableServer((args.host, args.port), Handler) as srv:
        print(f"board simulator listening on {args.host}:{args.port}", flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
Local TCP simulator for the ESP32 status LED server.

Run this on your computer when you want to verify the Claude hook bridge before
flashing hardware:

    python3 simulator.py --host 127.0.0.1 --port 8080
"""

from __future__ import annotations

import argparse
import socketserver
from datetime import datetime


VALID_STATES = {"thinking", "ai", "busy", "success", "error", "alarm", "off"}
COLORS = {
    "thinking": "\033[94m",
    "ai": "\033[96m",
    "busy": "\033[93m",
    "success": "\033[92m",
    "error": "\033[91m",
    "alarm": "\033[95m",
    "off": "\033[90m",
}
RESET = "\033[0m"


def normalize_command(command: str) -> str:
    command = command.strip().lower()

    if command.startswith("state "):
        command = command[6:].strip()

    marker = '"state"'
    json_key = command.find(marker)
    if json_key >= 0:
        colon = command.find(":", json_key)
        first_quote = command.find('"', colon + 1)
        second_quote = command.find('"', first_quote + 1)
        if colon >= 0 and first_quote >= 0 and second_quote > first_quote:
            command = command[first_quote + 1 : second_quote].strip()

    return command


class LedRequestHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        for raw in self.rfile:
            command = raw.decode("utf-8", errors="replace")
            state = normalize_command(command)
            now = datetime.now().strftime("%H:%M:%S")

            if state in VALID_STATES:
                color = COLORS[state]
                print(f"{now} LED {color}{state}{RESET}", flush=True)
                self.wfile.write(b"OK\n")
            else:
                print(f"{now} ERR unknown command: {command.strip()!r}", flush=True)
                self.wfile.write(b"ERR\n")


class ReusableTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Simulate the ESP32 LED TCP server.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with ReusableTCPServer((args.host, args.port), LedRequestHandler) as server:
        print(f"ESP32 LED simulator listening on {args.host}:{args.port}", flush=True)
        print("Press Ctrl-C to stop.", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print("\nStopped.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

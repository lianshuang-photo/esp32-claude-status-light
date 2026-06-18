#!/usr/bin/env bash
# Install (or remove) the macOS LaunchAgent that keeps signal-light's daemon
# running at login and respawns it if it crashes.
#
# Usage:
#   ./install_launchagent.sh            install + start
#   ./install_launchagent.sh --uninstall stop + remove
#   ./install_launchagent.sh --status    show whether it's loaded + running
#
# The plist lives in ~/Library/LaunchAgents/com.signal-light.daemon.plist
# Logs land in ~/.signal_light/logs/

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAEMON="$HERE/signal_daemon.py"
TEMPLATE="$HERE/com.signal-light.daemon.plist.template"

LABEL="com.signal-light.daemon"
PLIST_PATH="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_DIR="$HOME/.signal_light/logs"
PYTHON_BIN="$(command -v python3 || true)"

cmd="${1:-install}"

case "$cmd" in
  --uninstall|uninstall|stop)
    if [ -f "$PLIST_PATH" ]; then
      launchctl unload "$PLIST_PATH" 2>/dev/null || true
      rm -f "$PLIST_PATH"
      echo "✓ unloaded and removed $PLIST_PATH"
    else
      echo "  (no plist installed at $PLIST_PATH)"
    fi
    # Belt-and-braces: also kill any signal_daemon.py still in flight.
    if pgrep -f signal_daemon.py >/dev/null; then
      pkill -f signal_daemon.py 2>/dev/null || true
      echo "✓ stopped running daemon process"
    fi
    exit 0
    ;;

  --status|status)
    if [ -f "$PLIST_PATH" ]; then
      echo "  plist: $PLIST_PATH (exists)"
      if launchctl list | grep -q "$LABEL"; then
        line=$(launchctl list | grep "$LABEL")
        echo "  launchctl: loaded"
        echo "    $line"
      else
        echo "  launchctl: NOT loaded"
      fi
    else
      echo "  plist: not installed"
    fi
    echo
    if pgrep -f signal_daemon.py >/dev/null; then
      pid=$(pgrep -f signal_daemon.py | head -1)
      echo "  daemon process: running (pid $pid)"
    else
      echo "  daemon process: not running"
    fi
    if curl -sf --max-time 1 http://127.0.0.1:7878/api/status >/dev/null 2>&1; then
      echo "  HTTP API: ✓ responding on 127.0.0.1:7878"
    else
      echo "  HTTP API: ✗ NOT responding"
    fi
    echo
    if [ -f "$LOG_DIR/daemon.err.log" ]; then
      err_lines=$(wc -l <"$LOG_DIR/daemon.err.log" | tr -d ' ')
      echo "  err log: $LOG_DIR/daemon.err.log ($err_lines lines)"
      if [ "$err_lines" -gt 0 ]; then
        echo "  --- last 5 err lines ---"
        tail -5 "$LOG_DIR/daemon.err.log" | sed 's/^/    /'
      fi
    fi
    exit 0
    ;;

  install|"")
    # fall through
    ;;

  *)
    echo "usage: $0 [install|--uninstall|--status]" >&2
    exit 2
    ;;
esac

# --- install ---

if [ -z "$PYTHON_BIN" ]; then
  echo "!! python3 not found on PATH" >&2
  exit 2
fi
if [ ! -f "$DAEMON" ]; then
  echo "!! daemon not found at $DAEMON" >&2
  exit 2
fi
if [ ! -f "$TEMPLATE" ]; then
  echo "!! template not found at $TEMPLATE" >&2
  exit 2
fi

mkdir -p "$LOG_DIR"
mkdir -p "$(dirname "$PLIST_PATH")"

echo "  python  : $PYTHON_BIN"
echo "  daemon  : $DAEMON"
echo "  logs    : $LOG_DIR"
echo "  plist   : $PLIST_PATH"
echo

# If something else is already running, stop it before we hand it over to launchd.
if pgrep -f signal_daemon.py >/dev/null; then
  echo "  stopping pre-existing daemon process..."
  pkill -f signal_daemon.py 2>/dev/null || true
  sleep 1
fi

# If our plist already exists, unload it before overwriting.
if [ -f "$PLIST_PATH" ]; then
  launchctl unload "$PLIST_PATH" 2>/dev/null || true
fi

# Render template — use a divider character that won't appear in paths.
sed \
  -e "s|__PYTHON__|$PYTHON_BIN|g" \
  -e "s|__DAEMON_PATH__|$DAEMON|g" \
  -e "s|__LOG_DIR__|$LOG_DIR|g" \
  -e "s|__WORKING_DIR__|$HERE|g" \
  "$TEMPLATE" >"$PLIST_PATH"

launchctl load "$PLIST_PATH"

echo "✓ loaded"
echo

# Wait up to ~5 s for the daemon to come up
for i in 1 2 3 4 5 6 7 8 9 10; do
  if curl -sf --max-time 1 http://127.0.0.1:7878/api/status >/dev/null 2>&1; then
    echo "✓ daemon responding on http://127.0.0.1:7878 (after ${i}/10 attempts)"
    break
  fi
  sleep 0.5
done

if ! curl -sf --max-time 1 http://127.0.0.1:7878/api/status >/dev/null 2>&1; then
  echo "!! daemon did not respond within 5s"
  echo "   check $LOG_DIR/daemon.err.log"
  exit 1
fi

echo
echo "Daemon will now:"
echo "  - start automatically at every login"
echo "  - auto-respawn if it ever crashes (5 s back-off)"
echo "  - log stdout/stderr to $LOG_DIR/"
echo
echo "Manage it:"
echo "  ./install_launchagent.sh --status     check state"
echo "  ./install_launchagent.sh --uninstall  remove"

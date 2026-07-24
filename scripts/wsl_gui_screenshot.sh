#!/usr/bin/env bash
set -uo pipefail
export XDG_RUNTIME_DIR=/run/user/0
export DISPLAY=:0
cd "$(dirname "$0")/.."

pgrep -x pipewire >/dev/null || { nohup pipewire >/tmp/pipewire.log 2>&1 & sleep 1; }
pgrep -x wireplumber >/dev/null || { nohup wireplumber >/tmp/wireplumber.log 2>&1 & sleep 1; }

./build/daemon/pipeeq-daemon > /tmp/daemon.log 2>&1 &
DAEMON_PID=$!
sleep 1

./build/gui/pipeeq-gui > /tmp/gui.log 2>&1 &
GUI_PID=$!
sleep 2

import -window "PipeEQ" /tmp/pipeeq_gui.png 2>/tmp/import.log
echo "import exit: $?"

kill "$GUI_PID" 2>/dev/null
kill "$DAEMON_PID" 2>/dev/null
sleep 1
kill -9 "$GUI_PID" "$DAEMON_PID" 2>/dev/null

echo "--- gui.log ---"
cat /tmp/gui.log
echo "--- import.log ---"
cat /tmp/import.log

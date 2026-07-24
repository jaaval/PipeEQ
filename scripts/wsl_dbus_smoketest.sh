#!/usr/bin/env bash
set -uo pipefail
export XDG_RUNTIME_DIR=/run/user/0
cd "$(dirname "$0")/.."

./build/daemon/pipeeq-daemon > /tmp/daemon.log 2>&1 &
DAEMON_PID=$!
sleep 1

echo "--- ListDevices ---"
busctl --user call org.pipeeq.Daemon1 /org/pipeeq/Daemon1 org.pipeeq.Daemon1 ListDevices

echo "--- ListRoutes (should be empty) ---"
busctl --user call org.pipeeq.Daemon1 /org/pipeeq/Daemon1 org.pipeeq.Daemon1 ListRoutes

echo "--- AddRoute for a fake device (should return empty string: device unknown) ---"
busctl --user call org.pipeeq.Daemon1 /org/pipeeq/Daemon1 org.pipeeq.Daemon1 AddRoute ss "nonexistent_device" "Nonexistent"

echo "--- Introspect ---"
busctl --user introspect org.pipeeq.Daemon1 /org/pipeeq/Daemon1

kill "$DAEMON_PID" 2>/dev/null
sleep 1
kill -9 "$DAEMON_PID" 2>/dev/null
echo "--- daemon.log ---"
cat /tmp/daemon.log

#!/usr/bin/env bash
set -uo pipefail
export XDG_RUNTIME_DIR=/run/user/0
cd "$(dirname "$0")/.."

./build/daemon/pipeeq-daemon > /tmp/daemon.log 2>&1 &
DAEMON_PID=$!
sleep 1

call() {
    busctl --user call org.pipeeq.Daemon1 /org/pipeeq/Daemon1 org.pipeeq.Daemon1 "$@"
}

echo "--- ListInputs (should be empty) ---"
call ListInputs

echo "--- AddInput 'Music' ---"
call AddInput s "Music"

echo "--- AddInput 'Voice Chat' ---"
call AddInput s "Voice Chat"

echo "--- ListInputs (should show both) ---"
call ListInputs

echo "--- GetMixMatrix (should be empty - no routes yet) ---"
call GetMixMatrix

echo "--- AddRoute for a fake device (should fail: unknown device) ---"
call AddRoute ss "nonexistent_device" "Nonexistent"

echo "--- GetState ---"
call GetState

echo "--- Introspect ---"
busctl --user introspect org.pipeeq.Daemon1 /org/pipeeq/Daemon1

kill "$DAEMON_PID" 2>/dev/null
sleep 1
kill -9 "$DAEMON_PID" 2>/dev/null
echo "--- daemon.log ---"
cat /tmp/daemon.log

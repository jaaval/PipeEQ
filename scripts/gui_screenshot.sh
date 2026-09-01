#!/usr/bin/env bash
# Starts a daemon plus the GUI against a throwaway config and captures a
# screenshot, so a UI change can be looked at without touching the real setup.
#
# Uses the offscreen Qt platform by default, which needs no compositor and
# produces a deterministic image; pass --onscreen to run it on the real display
# instead. Never changes card profiles or the default sink.
set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
OUT="${OUT:-/tmp/pipeeq_gui.png}"
GEOMETRY="${GEOMETRY:-1100x700}"
ONSCREEN=0
SEED_CONFIG="${SEED_CONFIG:-}"

for arg in "$@"; do
    case "$arg" in
        --onscreen) ONSCREEN=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

WORKDIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$WORKDIR/config"
mkdir -p "$XDG_CONFIG_HOME/pipeeq"
if [[ -n "$SEED_CONFIG" && -f "$SEED_CONFIG" ]]; then
    cp "$SEED_CONFIG" "$XDG_CONFIG_HOME/pipeeq/config.json"
fi

cleanup() {
    [[ -n "${GUI_PID:-}" ]] && kill "$GUI_PID" 2>/dev/null
    [[ -n "${DAEMON_PID:-}" ]] && kill "$DAEMON_PID" 2>/dev/null
    # Wait only on the jobs we started, with a bound: a bare `wait` here can
    # block indefinitely on a child that ignores the signal.
    [[ -n "${GUI_PID:-}" ]] && timeout 5 tail --pid="$GUI_PID" -f /dev/null 2>/dev/null
    [[ -n "${DAEMON_PID:-}" ]] && timeout 5 tail --pid="$DAEMON_PID" -f /dev/null 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

"$BUILD_DIR/daemon/pipeeq-daemon" > "$WORKDIR/daemon.log" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do
    busctl --user list 2>/dev/null | grep -q org.pipeeq.Daemon1 && break
    sleep 0.1
done

if [[ "$ONSCREEN" -eq 1 ]]; then
    # Force the X11 backend even on a Wayland session: `import` can only find a
    # window by title through X, and under the native Wayland plugin there is no
    # X window to find - which just makes the capture hang.
    QT_QPA_PLATFORM=xcb "$BUILD_DIR/gui/pipeeq-gui" > "$WORKDIR/gui.log" 2>&1 &
    GUI_PID=$!
    sleep 3
    # Capture ONLY the PipeEQ window, never the whole screen - this may be run
    # on a desktop someone is using. Bounded, because a missing window makes
    # `import` drop into interactive selection and wait forever.
    if ! timeout 10 import -window "PipeEQ" "$OUT" 2>"$WORKDIR/import.log"; then
        echo "could not capture a window titled PipeEQ:" >&2
        cat "$WORKDIR/import.log" >&2
    fi
else
    # The offscreen platform needs no compositor and puts no window on anyone's
    # screen, so this mode is a startup/wiring check rather than a picture: it
    # proves the GUI builds its widgets and completes its first round of D-Bus
    # calls without crashing. Use --onscreen for an actual image.
    QT_QPA_PLATFORM=offscreen "$BUILD_DIR/gui/pipeeq-gui" > "$WORKDIR/gui.log" 2>&1 &
    GUI_PID=$!
    sleep 3
    if kill -0 "$GUI_PID" 2>/dev/null; then
        echo "ok: the GUI started and stayed up against a live daemon"
    else
        echo "FAIL: the GUI exited early" >&2
    fi
fi

echo "--- daemon log ---"
cat "$WORKDIR/daemon.log"
echo "--- gui log ---"
cat "$WORKDIR/gui.log"
if [[ -f "$OUT" ]]; then
    echo "screenshot: $OUT"
else
    echo "no screenshot produced (install xvfb-run, or pass --onscreen)" >&2
fi

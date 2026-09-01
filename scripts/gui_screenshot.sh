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
DEMO=0
OPEN_EQ=0
SEED_CONFIG="${SEED_CONFIG:-}"

for arg in "$@"; do
    case "$arg" in
        --onscreen) ONSCREEN=1 ;;
        --demo) DEMO=1 ;;
        --open-eq) OPEN_EQ=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

GUI_ARGS=""
[[ "$DEMO" -eq 1 ]] && GUI_ARGS="$GUI_ARGS --demo"
[[ "$OPEN_EQ" -eq 1 ]] && GUI_ARGS="$GUI_ARGS --open-eq"

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

# --demo needs no daemon at all: that is the point of it.
if [[ "$DEMO" -eq 0 ]]; then
    "$BUILD_DIR/daemon/pipeeq-daemon" > "$WORKDIR/daemon.log" 2>&1 &
    DAEMON_PID=$!
    for _ in $(seq 1 50); do
        busctl --user list 2>/dev/null | grep -q org.pipeeq.Daemon1 && break
        sleep 0.1
    done
else
    echo "(demo mode: no daemon started)" > "$WORKDIR/daemon.log"
fi

if [[ "$ONSCREEN" -eq 1 ]]; then
    # Force the X11 backend even on a Wayland session: `import` can only find a
    # window by title through X, and under the native Wayland plugin there is no
    # X window to find - which just makes the capture hang.
    QT_QPA_PLATFORM=xcb "$BUILD_DIR/gui/pipeeq-gui" $GUI_ARGS > "$WORKDIR/gui.log" 2>&1 &
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
    # A private X server, so the capture puts nothing on anyone's real screen
    # and comes out the same size every run - which is what makes screenshots
    # comparable from one phase to the next.
    #
    # Two env fixes that are easy to get wrong and fail confusingly:
    #   - WAYLAND_DISPLAY must be UNSET. Qt prefers Wayland whenever it is set,
    #     so the app would connect to the real compositor and put a window on
    #     the user's actual screen while the capture of the private X server
    #     came out blank.
    #   - QT_QPA_PLATFORMTHEME must be cleared. A platform theme plugin opens
    #     its own connection to the display and complains when it can't.
    #
    # Output is redirected to a file rather than piped: a pipe whose reader
    # exits (`| head`) sends SIGPIPE and kills the GUI mid-capture.
    xvfb-run -a --server-args="-screen 0 ${GEOMETRY}x24" bash -c "
        env -u WAYLAND_DISPLAY QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME= \
            '$BUILD_DIR/gui/pipeeq-gui' $GUI_ARGS --geometry '$GEOMETRY' \
            > '$WORKDIR/gui-inner.log' 2>&1 &
        GUI=\$!
        sleep 4
        import -window root '$OUT'
        kill \$GUI 2>/dev/null
    " > "$WORKDIR/gui.log" 2>&1
    cat "$WORKDIR/gui-inner.log" >> "$WORKDIR/gui.log" 2>/dev/null
    GUI_PID=""
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

#!/usr/bin/env bash
# Checks that the GUI survives the daemon going away.
#
# Before the store moved D-Bus calls off the GUI thread, a call into a daemon
# that had just died blocked the paint loop for sdbus's 25-second default
# timeout - so this is a regression test for a freeze, not just for a message.
#
# Runs entirely inside a private X server against a throwaway config.
set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
WORKDIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$WORKDIR/config"
mkdir -p "$XDG_CONFIG_HOME"

cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

failures=0
report() {
    if [[ "$2" == "ok" ]]; then echo "ok: $1"; else echo "FAIL: $1" >&2; failures=$((failures + 1)); fi
}

xvfb-run -a --server-args="-screen 0 1100x720x24" bash -c '
    set -u
    "'"$BUILD_DIR"'/daemon/pipeeq-daemon" > "'"$WORKDIR"'/daemon.log" 2>&1 &
    DAEMON=$!
    for _ in $(seq 1 50); do
        busctl --user list 2>/dev/null | grep -q org.pipeeq.Daemon1 && break
        sleep 0.1
    done

    env -u WAYLAND_DISPLAY QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME= \
        "'"$BUILD_DIR"'/gui/pipeeq-gui" --geometry 1100x720 \
        > "'"$WORKDIR"'/gui.log" 2>&1 &
    GUI=$!
    sleep 3
    kill -0 $GUI 2>/dev/null && echo "GUI_UP_WITH_DAEMON" || echo "GUI_DOWN_WITH_DAEMON"

    # Pull the daemon out from under it.
    kill -9 $DAEMON 2>/dev/null
    sleep 4

    if kill -0 $GUI 2>/dev/null; then
        echo "GUI_SURVIVED_DAEMON_DEATH"
        # A frozen GUI still answers kill -0, so prove it is still PAINTING by
        # capturing it: a wedged Qt event loop leaves the window content stale
        # and, under a fresh X server with no compositor, blank.
        import -window root "'"$WORKDIR"'/after.png" 2>/dev/null
        COLORS=$(magick "'"$WORKDIR"'/after.png" -format %k info: 2>/dev/null)
        echo "COLORS_AFTER=$COLORS"
    else
        echo "GUI_DIED_WITH_DAEMON"
    fi
    kill $GUI 2>/dev/null
' > "$WORKDIR/run.log" 2>&1

grep -q GUI_UP_WITH_DAEMON "$WORKDIR/run.log" && report "the GUI starts against a live daemon" ok \
    || report "the GUI starts against a live daemon" bad
grep -q GUI_SURVIVED_DAEMON_DEATH "$WORKDIR/run.log" && report "the GUI survives the daemon being killed" ok \
    || report "the GUI survives the daemon being killed" bad

colors=$(grep -oP '(?<=COLORS_AFTER=)\d+' "$WORKDIR/run.log" || echo 0)
if [[ "${colors:-0}" -gt 50 ]]; then
    report "the GUI is still painting afterwards ($colors distinct colours)" ok
else
    report "the GUI is still painting afterwards (only ${colors:-0} distinct colours)" bad
fi

if [[ "$failures" -gt 0 ]]; then
    echo; echo "--- run log ---"; cat "$WORKDIR/run.log"
    echo "--- gui log ---"; cat "$WORKDIR/gui.log" 2>/dev/null
    exit 1
fi
echo; echo "All GUI resilience checks passed."

#!/usr/bin/env bash
# Asserts that window-level keyboard bindings actually reach the application.
#
# This needs REAL key events, injected through the X server with XTEST, not
# synthetic QKeyEvents. A QShortcut is matched by Qt's shortcut map on the way in
# from the window system; a hand-built QKeyEvent handed to QApplication::sendEvent
# bypasses that path entirely, so a unit test can pass against a shortcut that
# never fires for a user. Escape is verified here for that reason.
#
# Runs on a private X server, so nothing appears on anyone's real screen and no
# key is injected into whatever the user is actually doing.
set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
GEOMETRY="${GEOMETRY:-1280x860}"
WORKDIR="$(mktemp -d)"
FAILED=0

# A throwaway settings home, for two reasons. It keeps the real one alone - the
# GUI persists its geometry and selected channel on every page change, so a run
# here would otherwise leave demo state in the user's own settings. And it makes
# the comparison valid at all: with a shared settings file each run restores the
# previous run's selection, so two captures of the same page showed different
# channels and nothing could be concluded from the difference.
export XDG_CONFIG_HOME="$WORKDIR/config"
mkdir -p "$XDG_CONFIG_HOME"

cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    FAILED=1
}

for tool in xvfb-run import compare magick python3; do
    if ! command -v "$tool" > /dev/null; then
        echo "INCOMPLETE: $tool is not installed, so keyboard bindings were not checked." >&2
        exit 1
    fi
done
if ! python3 -c "import Xlib" 2>/dev/null; then
    echo "INCOMPLETE: python-xlib is not installed, so no key could be injected." >&2
    exit 1
fi

# Focus has to be set explicitly. There is no window manager on a bare Xvfb, so
# the input focus stays at PointerRoot and XTEST events would be delivered to
# whatever is under the pointer - which is the root window, not the app.
cat > "$WORKDIR/sendkey.py" <<'PYEOF'
import sys
import time

from Xlib import X, display
from Xlib.ext import xtest
from Xlib.XK import string_to_keysym


def find_window(root, name):
    for child in root.query_tree().children:
        try:
            if child.get_wm_name() == name:
                return child
        except Exception:
            pass
        found = find_window(child, name)
        if found is not None:
            return found
    return None


key_name = sys.argv[1]
d = display.Display()
window = find_window(d.screen().root, "PipeEQ")
if window is None:
    print("no window titled PipeEQ", file=sys.stderr)
    raise SystemExit(1)

d.set_input_focus(window, X.RevertToParent, X.CurrentTime)
d.sync()
time.sleep(0.3)

keycode = d.keysym_to_keycode(string_to_keysym(key_name))
if keycode == 0:
    print("no keycode for " + key_name, file=sys.stderr)
    raise SystemExit(1)
xtest.fake_input(d, X.KeyPress, keycode)
xtest.fake_input(d, X.KeyRelease, keycode)
d.sync()
time.sleep(0.6)
PYEOF

# The header band. It identifies the page - "< Back to mixer ... Copy to
# channel..." on the editor, the selection line and "SENDS INTO THIS CHANNEL" on
# the mixer - and holds no meters, so it is identical between two runs of the
# same page.
HEADER_CROP="900x70+0+45"

run_gui() {
    # $1: extra GUI arguments, $2: key to inject or "" for none, $3: output png
    local args="$1" key="$2" out="$3"
    # Each run gets its own settings directory, so no run can inherit the
    # selection another one left behind.
    rm -rf "$XDG_CONFIG_HOME"
    mkdir -p "$XDG_CONFIG_HOME"
    xvfb-run -a --server-args="-screen 0 ${GEOMETRY}x24" bash -c "
        env -u WAYLAND_DISPLAY QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME= \
            '$BUILD_DIR/gui/pipeeq-gui' --demo --geometry '$GEOMETRY' $args \
            > '$WORKDIR/gui.log' 2>&1 &
        GUI=\$!
        sleep 4
        if [[ -n '$key' ]]; then
            python3 '$WORKDIR/sendkey.py' '$key' 2>>'$WORKDIR/inject.log' || echo INJECT_FAILED >> '$WORKDIR/inject.log'
        fi
        import -window root '$out'
        # Bounded. SIGTERM alone leaves a GUI that ignores it running, and its
        # Xvfb with it, which would hang this script rather than fail it.
        kill \$GUI 2>/dev/null
        timeout 5 tail --pid=\$GUI -f /dev/null 2>/dev/null
        kill -9 \$GUI 2>/dev/null
    " > /dev/null 2>&1
    [[ -f "$out" ]] || return 1
    magick "$out" -crop "$HEADER_CROP" "${out%.png}_header.png"
}

# How many pixels differ in the header band between two captures.
header_diff() {
    compare -metric AE -fuzz 3% "${1%.png}_header.png" "${2%.png}_header.png" null: 2>&1 |
        awk '{print int($1+0)}'
}

echo "capturing the mixer page and the EQ editor page..."
run_gui "" "" "$WORKDIR/mixer.png" || { fail "could not capture the mixer page"; exit 1; }
run_gui "--open-eq" "" "$WORKDIR/editor.png" || { fail "could not capture the EQ page"; exit 1; }

BASELINE="$(header_diff "$WORKDIR/mixer.png" "$WORKDIR/editor.png")"
if [[ "$BASELINE" -lt 500 ]]; then
    # Without this the whole check is vacuous: if the two pages looked alike in
    # the compared region, "Escape went back" would pass whatever happened.
    fail "the two pages are indistinguishable in the compared region ($BASELINE pixels differ); the check would prove nothing"
    exit 1
fi
echo "ok: the two pages differ in the compared region ($BASELINE pixels)"

echo "injecting Escape on the EQ editor page..."
if ! run_gui "--open-eq" "Escape" "$WORKDIR/after_escape.png"; then
    fail "could not capture after injecting Escape"
    exit 1
fi
if grep -q INJECT_FAILED "$WORKDIR/inject.log" 2>/dev/null; then
    fail "the key could not be injected: $(cat "$WORKDIR/inject.log")"
    exit 1
fi

TO_MIXER="$(header_diff "$WORKDIR/after_escape.png" "$WORKDIR/mixer.png")"
TO_EDITOR="$(header_diff "$WORKDIR/after_escape.png" "$WORKDIR/editor.png")"

# Both tests, not just the comparison. Nearer the mixer page than the editor is
# not on its own evidence of anything: a capture that resembles NEITHER page -
# a window that never mapped, say - still lands nearer one of them, and would
# decide the result by whichever page happens to carry more ink. So it also has
# to actually look like the mixer page.
RESEMBLANCE_BOUND=$(( BASELINE / 4 ))
if [[ "$TO_MIXER" -lt "$TO_EDITOR" && "$TO_MIXER" -lt "$RESEMBLANCE_BOUND" ]]; then
    echo "ok: Escape returns from the EQ editor to the mixer"
    echo "    ($TO_MIXER pixels from the mixer page, $TO_EDITOR from the editor)"
elif [[ "$TO_MIXER" -ge "$RESEMBLANCE_BOUND" && "$TO_MIXER" -lt "$TO_EDITOR" ]]; then
    fail "the capture after Escape resembles neither page; the check proves nothing"
    echo "    ($TO_MIXER pixels from the mixer page, bound $RESEMBLANCE_BOUND)" >&2
else
    fail "Escape did not leave the EQ editor"
    echo "    ($TO_MIXER pixels from the mixer page, $TO_EDITOR from the editor)" >&2
fi

if [[ "$FAILED" -eq 0 ]]; then
    echo
    echo "All GUI input checks passed."
else
    echo
    echo "GUI input checks FAILED." >&2
fi
exit "$FAILED"

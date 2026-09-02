#!/usr/bin/env bash
# End-to-end check of the org.pipeeq.Daemon1 surface against a real daemon.
#
# Unlike the WSL scripts this replaces, every step ASSERTS: the old ones printed
# busctl output for a human to eyeball, which meant they kept "passing" after
# the behaviour they described had changed.
#
# Runs against a throwaway XDG_CONFIG_HOME so it can never touch a real config,
# and it never changes card profiles or default sinks.
set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
DAEMON="$BUILD_DIR/daemon/pipeeq-daemon"
BUS="busctl --user"
DEST="org.pipeeq.Daemon1 /org/pipeeq/Daemon1 org.pipeeq.Daemon1"

failures=0
check() {
    local what="$1" expected="$2" actual="$3"
    if [[ "$actual" == *"$expected"* ]]; then
        echo "ok: $what"
    else
        echo "FAIL: $what" >&2
        echo "      expected to contain: $expected" >&2
        echo "      actual:              $actual" >&2
        failures=$((failures + 1))
    fi
}

if [[ ! -x "$DAEMON" ]]; then
    echo "FAIL: $DAEMON not built" >&2
    exit 1
fi

WORKDIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$WORKDIR/config"
mkdir -p "$XDG_CONFIG_HOME"

cleanup() {
    [[ -n "${DAEMON_PID:-}" ]] && kill "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# The isolation promise above is only real if OUR daemon is the one answering.
# A daemon already running (systemd user unit, or bus activation from the
# installed .service file) keeps the name, ours exits with FileExists, and every
# check below then runs against the live daemon - mutating the real config and
# opening streams on real hardware.
if $BUS list 2>/dev/null | grep -q org.pipeeq.Daemon1; then
    echo "FAIL: org.pipeeq.Daemon1 is already owned by another process." >&2
    echo "      Stop it first (systemctl --user stop pipeeq-daemon), or this test" >&2
    echo "      would run against your real daemon and real config." >&2
    exit 1
fi

"$DAEMON" > "$WORKDIR/daemon.log" 2>&1 &
DAEMON_PID=$!

for _ in $(seq 1 50); do
    $BUS list 2>/dev/null | grep -q org.pipeeq.Daemon1 && break
    sleep 0.1
done
if ! $BUS list 2>/dev/null | grep -q org.pipeeq.Daemon1; then
    echo "FAIL: daemon never claimed the bus name" >&2
    cat "$WORKDIR/daemon.log" >&2
    exit 1
fi
# ...and that it is still ours, not a pre-existing one that won a race.
if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "FAIL: our daemon exited; the bus name is owned by something else" >&2
    cat "$WORKDIR/daemon.log" >&2
    exit 1
fi

# ---- a fresh config starts empty ----
check "no outputs on a fresh config" "a(sssbbuuu) 0" "$($BUS call $DEST ListOutputs)"
check "no inputs on a fresh config" "a(ssas) 0" "$($BUS call $DEST ListInputs)"

# ---- inputs ----
INPUT_ID=$($BUS call $DEST AddInput sas Music 0 | tr -d 's "')
check "AddInput returns an id" "input-" "$INPUT_ID"
check "the new input is stereo" '"FL" "FR"' "$($BUS call $DEST ListInputs)"

SURROUND_ID=$($BUS call $DEST AddInput sas Film 6 FL FR FC LFE RL RR | tr -d 's "')
check "a 5.1 input keeps its layout" '"FC" "LFE"' "$($BUS call $DEST ListInputs)"

# ---- outputs on a device that isn't present ----
OUTPUT_ID=$($BUS call $DEST AddOutput ss nonexistent_device Ghost | tr -d 's "')
check "AddOutput returns an id" "output-" "$OUTPUT_ID"
# Matched against THIS output's row rather than the whole reply: `check` is a
# substring glob, and every ListOutputs row carries both a connected and an
# autoConnect boolean, so looking for "false" anywhere always succeeded.
check "an absent device yields a disconnected output" "\"$OUTPUT_ID\" \"nonexistent_device\" \"Ghost\" false" \
    "$($BUS call $DEST ListOutputs)"

# A device that isn't there has no layout, so there are no channels to drive
# yet - the output is configured and waits, which is the documented behaviour.
check "a pending output has no channels yet" "a(ussdbssb) 0" \
    "$($BUS call $DEST GetOutputChannels s "$OUTPUT_ID" 2>&1 || true)"

# ---- an output on a REAL device, so there are channels to exercise ----
# Prefer an HDMI sink when there is one: it's the least likely to be part of a
# live monitoring chain on a machine someone is working on. This only opens a
# silent playback stream - it never touches card profiles or the default sink.
DEVICES=$($BUS call $DEST ListDevices | grep -oP '"alsa_output[^"]*"' | tr -d '"')
DEVICE=$(grep -i hdmi <<< "$DEVICES" | head -1)
[[ -z "$DEVICE" ]] && DEVICE=$(head -1 <<< "$DEVICES")
if [[ -z "$DEVICE" ]]; then
    # Silently skipping was worse than failing: it dropped ~70% of the
    # assertions - every per-channel gain, the whole link/unlink EQ-sharing
    # behaviour, sends, and position handling - and still printed a green
    # "All D-Bus smoke checks passed."
    echo "INCOMPLETE: no real ALSA sink present, so the channel, EQ, link and" >&2
    echo "            send checks could not run. Set PIPEEQ_ALLOW_NO_SINK=1 to" >&2
    echo "            treat that as acceptable." >&2
    if [[ -z "${PIPEEQ_ALLOW_NO_SINK:-}" ]]; then
        failures=$((failures + 1))
    fi
else
    REAL_ID=$($BUS call $DEST AddOutput ss "$DEVICE" Test | tr -d 's "')
    sleep 0.5
    CHANNELS=$($BUS call $DEST GetOutputChannels s "$REAL_ID")
    check "a present device yields channels" '"FL"' "$CHANNELS"
    check "a present device connects" "\"$REAL_ID\" \"$DEVICE\" \"Test\" true" \
        "$($BUS call $DEST ListOutputs)"

    # ---- per-channel gain ----
    check "SetChannelGain succeeds" "b true" \
        "$($BUS call $DEST SetChannelGain sud -- "$REAL_ID" 0 -6.5)"
    # Anchored to channel 0's row: "-6.5" anywhere in the reply would also have
    # passed if the gain had landed on the wrong channel.
    check "the gain landed on channel 0" "0 \"FL\" \"\" -6.5" \
        "$($BUS call $DEST GetOutputChannels s "$REAL_ID")"

    # ---- link groups: one set must move both members ----
    GROUP_ID=$($BUS call $DEST CreateLinkGroup saus "$REAL_ID" 2 0 1 Mains | tr -d 's "')
    check "CreateLinkGroup returns an id" "group-" "$GROUP_ID"
    $BUS call $DEST SetChannelGain sud -- "$REAL_ID" 0 -12.0 > /dev/null
    LINKED=$($BUS call $DEST GetOutputChannels s "$REAL_ID")
    linked_count=$(grep -o -- "-12" <<< "$LINKED" | wc -l)
    if [[ "$linked_count" -ge 2 ]]; then
        echo "ok: a gain set on a linked channel moved both members"
    else
        echo "FAIL: a gain set on a linked channel did not move both members" >&2
        echo "      $LINKED" >&2
        failures=$((failures + 1))
    fi

    # A channel already in a group must not join another.
    check "overlapping link groups are refused" '""' \
        "$($BUS call $DEST CreateLinkGroup saus "$REAL_ID" 2 1 0 Again)"

    # ---- per-channel EQ via the convenience methods ----
    check "SetChannelEqBandCount succeeds" "b true" \
        "$($BUS call $DEST SetChannelEqBandCount suu "$REAL_ID" 0 2)"
    check "SetChannelEqBand succeeds" "b true" \
        "$($BUS call $DEST SetChannelEqBand suusddd -- "$REAL_ID" 0 0 peaking 250 -4.5 1.2)"
    BANDS=$($BUS call $DEST GetChannelEqBands su "$REAL_ID" 0)
    check "the band round-trips" "250" "$BANDS"
    check "the band gain round-trips" "-4.5" "$BANDS"

    # An instance was created on demand and assigned to that channel.
    check "an EQ instance was created on demand" "eq-" "$($BUS call $DEST ListEqInstances s "$REAL_ID")"

    # Linked channels SHARE one curve: that is what linking means now, and it is
    # why there is no separate assignment step. Channel 1 is in the same group,
    # so it must report the same bands.
    LINKED_BANDS=$($BUS call $DEST GetChannelEqBands su "$REAL_ID" 1)
    check "a linked channel shares the curve" "250" "$LINKED_BANDS"
    check "a linked channel shares the gain too" "-4.5" "$LINKED_BANDS"
    check "both linked channels report one instance" "a(ssubu) 1" \
        "$($BUS call $DEST ListEqInstances s "$REAL_ID")"
    check "the shared instance covers 2 channels" "2" \
        "$($BUS call $DEST ListEqInstances s "$REAL_ID")"

    # ...and unlinking must actually separate them, or "unlink" would only
    # separate the faders while both channels still shared one curve.
    check "RemoveLinkGroup succeeds" "b true" \
        "$($BUS call $DEST RemoveLinkGroup ss "$REAL_ID" "$GROUP_ID")"
    check "unlinking splits the curve into per-channel copies" "a(ssubu) 2" \
        "$($BUS call $DEST ListEqInstances s "$REAL_ID")"
    # Each copy keeps the values, so unlinking changes nothing audible.
    check "the copy kept its band" "250" "$($BUS call $DEST GetChannelEqBands su "$REAL_ID" 1)"
    # Editing one now leaves the other alone.
    $BUS call $DEST SetChannelEqBand suusddd -- "$REAL_ID" 0 0 peaking 900 -1.0 1.0 > /dev/null
    check "after unlinking, editing one channel leaves the other alone" "250" \
        "$($BUS call $DEST GetChannelEqBands su "$REAL_ID" 1)"
    check "the edited channel took the new value" "900" \
        "$($BUS call $DEST GetChannelEqBands su "$REAL_ID" 0)"

    # Re-link, which shares the lowest-index member's curve across the group.
    GROUP_ID=$($BUS call $DEST CreateLinkGroup saus "$REAL_ID" 2 0 1 Mains | tr -d 's "')
    check "re-linking shares the leader's curve" "900" \
        "$($BUS call $DEST GetChannelEqBands su "$REAL_ID" 1)"
    check "re-linking prunes the orphaned instance" "a(ssubu) 1" \
        "$($BUS call $DEST ListEqInstances s "$REAL_ID")"

    # ---- sends ----
    check "SetSend succeeds" "b true" \
        "$($BUS call $DEST SetSend susd -- "$REAL_ID" 0 "$INPUT_ID" -3.0)"
    check "the send is reported" "$INPUT_ID" "$($BUS call $DEST GetSends s "$REAL_ID")"
    check "RemoveSend succeeds" "b true" \
        "$($BUS call $DEST RemoveSend sus "$REAL_ID" 0 "$INPUT_ID")"

    # ---- channel position is metadata, not a reconnect ----
    check "SetChannelPosition succeeds" "b true" \
        "$($BUS call $DEST SetChannelPosition sus "$REAL_ID" 0 FC)"
    check "the output stayed connected across a position change" "\"$REAL_ID\" \"$DEVICE\" \"Test\" true" \
        "$($BUS call $DEST ListOutputs)"

    check "RemoveOutput leaves no trace of it" "" "$($BUS call $DEST RemoveOutput s "$REAL_ID")"
    removed_check=$($BUS call $DEST ListOutputs)
    if [[ "$removed_check" == *"\"$REAL_ID\""* ]]; then
        echo "FAIL: the output was still listed after RemoveOutput" >&2
        failures=$((failures + 1))
    else
        echo "ok: RemoveOutput actually removed it"
    fi
fi

# ---- metering is off until armed ----
METER_LOG="$WORKDIR/monitor.log"
timeout 1 $BUS monitor org.pipeeq.Daemon1 > "$METER_LOG" 2>&1 || true
if grep -q "Meters" "$METER_LOG"; then
    echo "FAIL: Meters was emitted without SetMeteringEnabled" >&2
    failures=$((failures + 1))
else
    echo "ok: no Meters traffic before it is armed"
fi

$BUS call $DEST SetMeteringEnabled b true > /dev/null
timeout 1 $BUS monitor org.pipeeq.Daemon1 > "$METER_LOG" 2>&1 || true
meter_count=$(grep -c "Meters" "$METER_LOG" || true)
if [[ "$meter_count" -gt 5 ]]; then
    echo "ok: Meters is emitted once armed ($meter_count in ~1s)"
else
    echo "FAIL: expected repeated Meters signals once armed, saw $meter_count" >&2
    failures=$((failures + 1))
fi

# ---- inputs are removable ----
$BUS call $DEST RemoveInput s "$SURROUND_ID" > /dev/null
$BUS call $DEST RemoveInput s "$INPUT_ID" > /dev/null
check "inputs were removed" "a(ssas) 0" "$($BUS call $DEST ListInputs)"

# ---- the config was written, in v2 ----
sleep 1
if [[ -f "$XDG_CONFIG_HOME/pipeeq/config.json" ]]; then
    check "the config is v2" '"version": 2' "$(cat "$XDG_CONFIG_HOME/pipeeq/config.json")"
else
    echo "FAIL: no config was written" >&2
    failures=$((failures + 1))
fi

echo
if [[ "$failures" -gt 0 ]]; then
    echo "$failures check(s) FAILED"
    echo "--- daemon log ---"
    cat "$WORKDIR/daemon.log"
    exit 1
fi
echo "All D-Bus smoke checks passed."

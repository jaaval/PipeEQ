# PipeEQ

A Linux audio mixer/router: any number of inputs (virtual sinks apps can be
assigned to, e.g. "Music" vs "Voice Chat"), each independently mixed into
any number of physical outputs (e.g. speakers + headphones on one
soundcard) at its own level, with an independent volume + parametric EQ per
output. Built as a background service (`pipeeq-daemon`, does the real-time
audio processing via PipeWire) plus a control app (`pipeeq-gui`, a Qt6
client that talks to the daemon over D-Bus).

## Components

- `common/` (`libeqcore`) - biquad DSP math, `EqChain`, JSON config structs.
  Shared by both the daemon and the GUI so the EQ curve the GUI draws always
  matches what the daemon actually applies.
- `daemon/` (`pipeeq-daemon`) - the background service.
  - `InputSource` - a capture stream (virtual sink apps play into); any
    number of these can exist.
  - `AudioEngine::RouteEntry` - one output's *desired* configuration (device,
    gain, mute, EQ, per-input mix levels, auto-connect) paired with a live
    `OutputRoute` that exists only while the target device does. The
    configuration is the source of truth and outlives the stream, which is
    what lets an output be edited, saved and restored while its hardware is
    unplugged.
  - `OutputRoute` - a playback stream pinned to a physical device, mixing
    any subset of the current inputs (each at its own level) and running
    the result through a parametric EQ + master gain/mute. Everything
    `OutputRoute::onProcess()` reads is served from an atomically-swapped
    immutable snapshot rather than protected by `pw_thread_loop_lock()`,
    because PipeWire's own docs confirm that lock does **not** synchronize
    against a stream's `process()` callback when
    `PW_STREAM_FLAG_RT_PROCESS` is set (true for every stream here) -
    `process()` runs on PipeWire's own realtime thread in that case. See
    the class comment in `daemon/output_route.h` for the full design.
  - A D-Bus service (`org.pipeeq.Daemon1` on the session bus) for control.
- `gui/` (`pipeeq-gui`) - Qt6 control app: add/remove outputs and inputs,
  set gain/mute/auto-connect per output, edit the parametric EQ on an
  interactive curve (**EQ** tab), and set each input's mix level into the
  selected output (**Mixer** tab). Outputs whose device isn't currently
  present are listed dimmed as `(waiting)` and stay fully editable.
- `packaging/` - the systemd user unit and D-Bus session-activation file.

## Building

Dependencies: CMake ≥ 3.20, a C++20 compiler, `pkg-config`, `libpipewire-0.3`
(dev headers), `sdbus-c++`, `nlohmann_json`, and (optional) `Qt6` with the
`Widgets` component for the GUI.

**Arch:**

```sh
sudo pacman -S --needed cmake gcc pkgconf pipewire sdbus-cpp nlohmann-json qt6-base
```

**Debian / Ubuntu:**

```sh
sudo apt install build-essential cmake pkg-config libpipewire-0.3-dev \
    libsdbus-c++-dev nlohmann-json3-dev qt6-base-dev
```

**Fedora:**

```sh
sudo dnf install gcc-c++ cmake pkgconf-pkg-config pipewire-devel \
    sdbus-cpp-devel json-devel qt6-qtbase-devel
```

Exact package names/versions vary a bit by release - if one of these isn't
found, search your distro's package repo for the library name (e.g.
`sdbus-c++` or `nlohmann-json`).

Then:

```sh
./scripts/build.sh
```

This configures into `build/`, builds everything, and runs the `eqcore`
DSP self-tests (pure math checks - no audio hardware or running PipeWire
instance required).

## Running (manually, for development)

```sh
./build/daemon/pipeeq-daemon
```

This starts the virtual sink and the D-Bus service. In another terminal:

```sh
busctl --user call org.pipeeq.Daemon1 /org/pipeeq/Daemon1 org.pipeeq.Daemon1 ListDevices
```

(Note the `--user` flag - this is a per-user session-bus service, not a
system one.)

On first run (no saved config yet) the daemon creates one input named
"Default". Set it as your system's default audio output (e.g. in your
desktop's sound settings, or `wpctl`/`pavucontrol`) so application audio
flows into it, then use `pipeeq-gui` (or `busctl`) to add routes to your
actual output devices - new outputs hear every existing input at 0dB by
default. Add more inputs from the GUI's Mixer tab (or `AddInput` over
D-Bus) to assign different apps to different virtual sinks and mix them
independently per output; a newly added input starts silent on every
output until you explicitly turn it on for one, so it never suddenly
changes the level of an output you've already tuned.

## Installing as a persistent service

```sh
sudo cmake --install build
systemctl --user daemon-reload
systemctl --user enable --now pipeeq-daemon.service
```

This installs the daemon binary, the systemd **user** unit, and the D-Bus
session-activation file (so `pipeeq-gui` can auto-start the daemon on
demand even without the systemd unit enabled).

PipeWire itself normally runs as a per-user service, which is why
`pipeeq-daemon` is a `systemd --user` unit rather than a system-wide one.
A user unit only starts automatically once you log in, though - to have it
running from boot (e.g. on a headless or auto-login machine) without
requiring an interactive login session, enable lingering for your user:

```sh
loginctl enable-linger "$USER"
```

## Config

Inputs and routes (which devices have routes, their gain/mute/EQ bands,
and each route's mix level for each input) are persisted at
`$XDG_CONFIG_HOME/pipeeq/config.json` (falling back to
`~/.config/pipeeq/config.json`), and reloaded automatically on daemon
startup. Routes are matched back to devices by PipeWire node name, since
numeric node ids aren't stable across reboots. Configs saved before the
mixer feature existed (no inputs at all) are migrated automatically: one
default input is synthesized and wired into every saved route at 0dB, so
upgrading doesn't change any existing single-input setup's behavior.

The file is rewritten via a temporary file and a rename, so an interrupted
save can't leave a half-written config behind.

## Inputs

Every input is one independent PipeWire sink that applications can be
assigned to, so "Music" and "Voice Chat" are two separate entries in your
sound settings rather than one device with several channel pairs. Adding an
input (GUI **Mixer** tab, or `AddInput` over D-Bus) creates one immediately;
no restart, and nothing else has to be reconfigured.

For an input with id `input-2` and name `Voice Chat`, the resulting node is:

| property | value |
| --- | --- |
| `node.name` | `pipeeq_input_input-2` (what the OS remembers a default-sink choice by) |
| `node.description` | `Voice Chat` (what applications show) |
| `media.class` | `Audio/Sink` |
| format | `F32`, 48 kHz, 2 channels at `FL`/`FR` |

Ids are assigned once and then preserved in the config, so `node.name` is
stable: making an input your default output, or pinning an application to
it, survives daemon restarts. Renaming an input changes only its
description, so it doesn't break either. Inputs are also excluded from the
route target list (by the `pipeeq_input_` name prefix), so an input can
never be selected as an output and looped back into itself.

PipeEQ is **stereo-only** by design: an input is a stereo pair, an output
is a stereo pair, and the mixing and EQ both assume that. Channel positions
are advertised explicitly (see `daemon/audio_format.h`) - without them a
2-channel stream negotiates as the channel map `aux0,aux1`, which leaves
applications and volume UIs unable to tell left from right. Adding mono
would be a small extension of the same helper; genuine multichannel would
not, and isn't a goal - see below for how multi-channel *hardware* is
handled instead.

## Multi-channel devices as several stereo outputs

Interfaces that are really several stereo pairs often present themselves as
one multi-channel device. A Focusrite Scarlett 4i4, whose outputs 1/2 and
3/4 are meant for monitors and headphones, shows up as a single 4.0 sink
with the layout `[ FL, FR, RL, RR ]`.

PipeEQ splits that back apart: each **stereo pair** of a device is offered
as its own output target, so outputs 1/2 and 3/4 become two PipeEQ outputs
with fully independent gain, mute, EQ and per-input mix. They appear as
separate entries when adding an output ("… — Front L/R (ch 1-2)" and
"… — Rear L/R (ch 3-4)"), and an existing output can be re-pointed at a
different pair with the **Channels** dropdown without losing its EQ. Plain
stereo devices offer a single pair and the dropdown is hidden.

Two details make this work:

- A device's layout is only reported in its *node info*, not in the
  registry's summary properties, so the daemon binds each sink node to read
  `audio.position` (`daemon/channel_pair.h` turns that into the offered
  pairs). Node info is re-sent on profile changes, so switching a card's
  profile updates the available pairs live.
- Each output stream declares its two channel positions **and** sets
  `stream.dont-remix`. Without that flag PipeWire helpfully upmixes a stereo
  stream across the target's whole layout - one output ends up driving all
  four ports, which is why the monitor and headphone pairs used to get the
  same signal and couldn't be EQ'd apart.

If a device's profile changes so that an output's pair no longer exists, the
output is disconnected rather than left playing into channels the device no
longer has, and the GUI marks it `(channels n/a)` - distinct from
`(waiting)`, which means the device itself is absent.

## Connecting to hardware

Each output has an **auto-connect** flag (on by default, `auto_connect` in
the config, `SetRouteAutoConnect` over D-Bus). With it on, the daemon
connects that output:

- at startup, if the device is already there - `start()` waits for
  PipeWire's initial device enumeration to finish before restoring the
  config, so a cold start doesn't race it and conclude the hardware is
  missing;
- as soon as the device appears, if it isn't available yet (powered off,
  unplugged, a Bluetooth sink that hasn't paired, a USB interface plugged
  in later);
- again after the device goes away and comes back - a replug gives the node
  a new id, which the daemon notices and reconnects to.

An output whose device is missing is *not* forgotten. It stays in the
config and in the GUI (dimmed, marked `(waiting)`), keeps its gain, mute,
EQ and mix levels, and can still be edited - the changes are saved and
applied the moment it connects. That also means you can configure an output
for hardware you haven't plugged in yet.

Turning auto-connect off leaves an already-connected output running and
only stops it being reconnected later, so it's a way to park an output
without deleting its EQ.

Device arrivals are reported on PipeWire's own loop thread, which can't do
the connect/disconnect work itself without inverting the daemon's lock
order, so the main thread picks them up on a 200 ms tick - that interval is
the worst-case delay before an output whose device just appeared starts
playing.

## License

MIT - see [LICENSE](LICENSE).

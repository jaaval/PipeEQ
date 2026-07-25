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
  set gain/mute per output, edit the parametric EQ on an interactive curve
  (**EQ** tab), and set each input's mix level into the selected output
  (**Mixer** tab).
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

## License

MIT - see [LICENSE](LICENSE).

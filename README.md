# PipeEQ

A Linux audio output router: capture all system audio once, fan it out to
any number of physical outputs (e.g. speakers + headphones on one
soundcard), and control an independent volume + parametric EQ per output.
Built as a background service (`pipeeq-daemon`, does the real-time audio
processing via PipeWire) plus a control app (`pipeeq-gui`, a Qt6 client
that talks to the daemon over D-Bus).

## Components

- `common/` (`libeqcore`) - biquad DSP math, `EqChain`, JSON config structs.
  Shared by both the daemon and the GUI so the EQ curve the GUI draws always
  matches what the daemon actually applies.
- `daemon/` (`pipeeq-daemon`) - the background service. Creates a PipeWire
  virtual sink ("PipeEQ Virtual Sink") that appears as a normal audio
  output; any number of output routes, each a playback stream pinned to a
  physical device with its own gain/mute/EQ; and a D-Bus service
  (`org.pipeeq.Daemon1` on the session bus) for control.
- `gui/` (`pipeeq-gui`) - Qt6 control app: add/remove routes, set gain and
  mute, edit the parametric EQ on an interactive curve.
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

Once built, set "PipeEQ Virtual Sink" as your system's default audio output
(e.g. in your desktop's sound settings, or `wpctl`/`pavucontrol`) so
application audio flows into it, then use `pipeeq-gui` (or `busctl`) to add
routes to your actual output devices.

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

Route configuration (which devices have routes, their gain/mute/EQ bands)
is persisted at `$XDG_CONFIG_HOME/pipeeq/config.json` (falling back to
`~/.config/pipeeq/config.json`), and reloaded automatically on daemon
startup. Routes are matched back to devices by PipeWire node name, since
numeric node ids aren't stable across reboots.

## License

MIT - see [LICENSE](LICENSE).

# PipeEQ

A Linux audio router and per-channel equaliser, built on PipeWire.

Applications play into any number of **inputs** — virtual sinks that appear in
your sound settings, so "Music" and "Voice Chat" can be separate devices. Each
input is mixed into any number of **outputs** at its own level, where an output
is one physical device with all of its channels. Every channel of every output
has its own volume, mute and parametric EQ, so the front pair, the centre, the
subwoofer and the rears of a surround card are each adjustable on their own.
Channels can be linked to move together and share one EQ curve.

Routing is by channel position, not by index: a stereo input feeds the FL/FR of
whatever it is sent to, and a 5.1 input into a stereo output contributes only
its front pair. Inputs and outputs keep their settings while their hardware is
absent, so an output can be configured for an interface that is not plugged in
yet, and a replug restores it.

It ships as two programs: `pipeeq-daemon`, a background service that does the
realtime audio processing, and `pipeeq-gui`, a Qt6 mixer that controls it over
the session D-Bus (`org.pipeeq.Daemon1`).

Limits: 32 channels per output, 16 EQ bands per channel, 8 inputs feeding any
one output.

## Building

Dependencies: CMake ≥ 3.20, a C++20 compiler, `pkg-config`,
`libpipewire-0.3` (dev headers), `sdbus-c++` (1.x or 2.x), `nlohmann_json`,
and Qt6 `Widgets` for the GUI.

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

Package names vary between releases; if one is not found, search your distro's
repository for the library name.

Then:

```sh
./scripts/build.sh
```

This configures into `build/`, builds everything and runs the test suites. The
tests need neither audio hardware nor a running PipeWire.

Useful options: `-DPIPEEQ_BUILD_GUI=OFF` for a daemon-only build (leaving Qt6
out entirely), `-DPIPEEQ_BUILD_TESTS=OFF` to skip the suites.

## Running

```sh
./build/daemon/pipeeq-daemon     # in one terminal
./build/gui/pipeeq-gui           # in another
```

A first run starts empty. In the GUI, pick a device from the top bar and press
**Add output**, then **Add sink...** to create an input; set that input as your
system's default output so application audio flows into it. New channels start
with no sends, so nothing plays until you switch a send on — adding an output
never changes the level of one you have already tuned.

`pipeeq-gui --demo` runs the whole interface against synthetic data, with no
daemon and no PipeWire, which is useful for looking at the UI.

The daemon can also be driven directly:

```sh
busctl --user call org.pipeeq.Daemon1 /org/pipeeq/Daemon1 org.pipeeq.Daemon1 ListDevices
```

Note `--user`: this is a session-bus service, not a system one.

## Installing as a service

```sh
sudo cmake --install build
systemctl --user daemon-reload
systemctl --user enable --now pipeeq-daemon.service
```

That installs the binaries, the systemd **user** unit and a D-Bus
session-activation file, so the GUI can start the daemon on demand even
without the unit enabled.

PipeWire itself runs per-user, which is why this is a `systemd --user` unit. A
user unit only starts at login; to have it running from boot on a headless or
auto-login machine, enable lingering:

```sh
loginctl enable-linger "$USER"
```

## Configuration

Settings live in `$XDG_CONFIG_HOME/pipeeq/config.json` (usually
`~/.config/pipeeq/config.json`) and are reloaded at startup. Devices are
matched by PipeWire node name, since node ids are not stable across reboots.

Writes are atomic, and an unreadable config is never overwritten — the daemon
serves an empty configuration for that session and says so, rather than
destroying the file. A config from an older version is upgraded in place, with
the original kept alongside as `config.json.v1.bak`.

## License

MIT — see [LICENSE](LICENSE).

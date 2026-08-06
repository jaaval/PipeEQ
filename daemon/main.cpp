#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

#include <pipewire/pipewire.h>

#include "audio_engine.h"
#include "dbus_service.h"

namespace {

std::atomic<bool> g_running{true};

void onSignal(int /*signum*/) {
    g_running.store(false);
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    pw_init(&argc, &argv);

    pipeeq::AudioEngine engine;
    if (!engine.start()) {
        pw_deinit();
        return 1;
    }

    pipeeq::DbusService dbusService(engine);
    dbusService.start();

    // Device arrivals and departures are reported on PipeWire's loop thread,
    // which can't do the connect/disconnect work itself without inverting
    // the engine's lock order (see AudioEngine's class comment), so it hands
    // that off to this loop. The interval is the worst-case delay before an
    // output whose hardware just appeared starts playing.
    while (g_running.load()) {
        dbusService.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::printf("pipeeq-daemon: shutting down\n");
    dbusService.stop();
    engine.stop();
    pw_deinit();
    return 0;
}

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
    engine.start();
    std::printf("pipeeq-daemon: started virtual sink 'PipeEQ Virtual Sink'\n");

    pipeeq::DbusService dbusService(engine);
    dbusService.start();

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::printf("pipeeq-daemon: shutting down\n");
    dbusService.stop();
    engine.stop();
    pw_deinit();
    return 0;
}

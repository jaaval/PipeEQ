#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <mutex>

#include <pipewire/pipewire.h>

#include "audio_engine.h"
#include "dbus_service.h"

namespace {

std::atomic<bool> g_running{true};
std::mutex g_wakeMutex;
std::condition_variable g_wake;

void onSignal(int /*signum*/) {
    g_running.store(false);
    g_wake.notify_all();
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
    if (!dbusService.start()) {
        engine.stop();
        pw_deinit();
        return 1;
    }

    // Everything periodic - device reconciliation, the debounced config save,
    // signal emission - runs on DbusService's own service thread, which is also
    // the thread that dispatches incoming D-Bus calls. Unifying those is what
    // makes it possible to emit a signal from the reconciler at all; this used
    // to be a 200 ms sleep loop here that deliberately emitted nothing.
    {
        std::unique_lock<std::mutex> lock(g_wakeMutex);
        // Predicated wait rather than a bare one: a signal delivered between
        // the check and the wait would otherwise be missed until the timeout.
        g_wake.wait_for(lock, std::chrono::hours(24 * 365),
                         [] { return !g_running.load(); });
        while (g_running.load()) {
            g_wake.wait_for(lock, std::chrono::seconds(1), [] { return !g_running.load(); });
        }
    }

    std::printf("pipeeq-daemon: shutting down\n");
    dbusService.stop();
    engine.stop();
    pw_deinit();
    return 0;
}

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pipewire/pipewire.h>

#include "output_route.h"

namespace pipeeq {

struct DeviceInfo {
    uint32_t id;
    std::string nodeName;
    std::string description;
};

// Owns the PipeWire thread loop, the capture "virtual sink" stream that
// system audio plays into, and the set of OutputRoutes that fan that audio
// out to physical devices, each with its own gain/mute/EqChain.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Starts the thread loop, connects to PipeWire, and creates the capture
    // virtual sink. Call pw_init() before constructing/starting any engine.
    void start();
    void stop();

    std::vector<DeviceInfo> listDevices() const;

    // Adds a route targeting the currently-known device with this node.name.
    // Returns the new route id, or an empty string if the device isn't known.
    std::string addRoute(const std::string& deviceName, const std::string& displayName, double gainDb = 0.0);
    void removeRoute(const std::string& routeId);

    std::vector<RouteInfo> listRoutes() const;

    // Each returns false if routeId doesn't name a currently-active route.
    bool setRouteGain(const std::string& routeId, double gainDb);
    bool setRouteMuted(const std::string& routeId, bool muted);
    bool setRouteBandCount(const std::string& routeId, std::size_t count);
    bool setRouteBand(const std::string& routeId, std::size_t index, const eqcore::EqBand& band);
    std::vector<eqcore::EqBand> getRouteBands(const std::string& routeId) const;

    static constexpr int kNumChannels = 2;
    static constexpr uint32_t kSampleRateHz = 48000;

    // Public because they're referenced from file-scope pw_*_events structs
    // in audio_engine.cpp; not part of the intended public API otherwise.
    static void onRegistryGlobal(void* userdata, uint32_t id, uint32_t permissions, const char* type,
                                  uint32_t version, const spa_dict* props);
    static void onRegistryGlobalRemove(void* userdata, uint32_t id);

    static void onCaptureProcess(void* userdata);
    static void onCaptureStateChanged(void* userdata, pw_stream_state old, pw_stream_state state,
                                       const char* error);

private:
    void distributeToRoutes(const float* interleaved, std::size_t frames);

    // Callers must already hold pw_thread_loop_lock(loop_).
    OutputRoute* findRoute(const std::string& routeId);

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    spa_hook registryListener_{};

    pw_stream* captureStream_ = nullptr;
    spa_hook captureListener_{};

    mutable std::mutex devicesMutex_;
    std::vector<DeviceInfo> devices_;

    // Not guarded by its own mutex: routes_ is only mutated by callers
    // holding pw_thread_loop_lock(), and only read from inside a process
    // callback running on the loop's own thread - pw_thread_loop guarantees
    // those two never overlap.
    std::vector<std::unique_ptr<OutputRoute>> routes_;
    int nextRouteIndex_ = 1;
};

} // namespace pipeeq

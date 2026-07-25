#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>

#include "input_source.h"
#include "output_route.h"

namespace pipeeq {

struct DeviceInfo {
    uint32_t id;
    std::string nodeName;
    std::string description;
};

struct InputInfo {
    std::string id;
    std::string displayName;
};

// Owns the PipeWire thread loop, the set of InputSources (each a capture
// "virtual sink" apps can play into), and the set of OutputRoutes that mix
// any subset of those inputs and fan the result out to physical devices,
// each with its own gain/mute/EqChain.
//
// routes_ and inputs_ are plain containers, not synchronized: every method
// here is only ever called from the D-Bus dispatch thread (sdbus-c++
// serializes incoming method calls onto one thread) or, at startup, from
// the main thread before that dispatch thread exists - so mutations never
// race each other. Nothing on PipeWire's realtime thread iterates either
// container (see OutputRoute's class comment) - an OutputRoute only ever
// reads from InputSource ring buffers it was handed a shared_ptr to when a
// route subscribed, so removing an input/route here never invalidates
// something a concurrently-running process() callback is using.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Starts the thread loop and connects to PipeWire. Call pw_init()
    // before constructing/starting any engine.
    void start();
    void stop();

    std::vector<DeviceInfo> listDevices() const;

    // Inputs (capture sources apps/other clients play into).
    std::string addInput(const std::string& displayName);
    void removeInput(const std::string& inputId);
    std::vector<InputInfo> listInputs() const;

    // Adds a route targeting the currently-known device with this node.name.
    // Returns the new route id, or an empty string if the device isn't known.
    // Seeds every currently-known input onto the new route at 0dB, so a new
    // output hears everything by default (matching pre-mixer behavior).
    std::string addRoute(const std::string& deviceName, const std::string& displayName, double gainDb = 0.0);
    void removeRoute(const std::string& routeId);

    std::vector<RouteInfo> listRoutes() const;

    // Each returns false if routeId doesn't name a currently-active route.
    bool setRouteGain(const std::string& routeId, double gainDb);
    bool setRouteMuted(const std::string& routeId, bool muted);
    bool setRouteBandCount(const std::string& routeId, std::size_t count);
    bool setRouteBand(const std::string& routeId, std::size_t index, const eqcore::EqBand& band);
    std::vector<eqcore::EqBand> getRouteBands(const std::string& routeId) const;

    // Activates/updates or clears one input's mix level into one route.
    // False if either id is unknown.
    bool setRouteInputGain(const std::string& routeId, const std::string& inputId, double gainDb);
    bool removeRouteInput(const std::string& routeId, const std::string& inputId);
    std::vector<std::pair<std::string, double>> getRouteInputGains(const std::string& routeId) const;

    static constexpr int kNumChannels = 2;
    static constexpr uint32_t kSampleRateHz = 48000;

    // Public because they're referenced from file-scope pw_*_events structs
    // in audio_engine.cpp; not part of the intended public API otherwise.
    static void onRegistryGlobal(void* userdata, uint32_t id, uint32_t permissions, const char* type,
                                  uint32_t version, const spa_dict* props);
    static void onRegistryGlobalRemove(void* userdata, uint32_t id);

private:
    OutputRoute* findRoute(const std::string& routeId) const;
    InputSource* findInput(const std::string& inputId) const;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    spa_hook registryListener_{};

    mutable std::mutex devicesMutex_;
    std::vector<DeviceInfo> devices_;

    std::vector<std::unique_ptr<InputSource>> inputs_;
    std::vector<std::unique_ptr<OutputRoute>> routes_;
    int nextRouteIndex_ = 1;
    int nextInputIndex_ = 1;
};

} // namespace pipeeq

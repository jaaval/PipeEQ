#include "audio_engine.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

#include <spa/utils/dict.h>

namespace pipeeq {

namespace {

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = AudioEngine::onRegistryGlobal,
    .global_remove = AudioEngine::onRegistryGlobalRemove,
};

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    stop();
}

void AudioEngine::start() {
    loop_ = pw_thread_loop_new("pipeeq-loop", nullptr);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);

    pw_thread_loop_start(loop_);

    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry_, &registryListener_, &kRegistryEvents, this);
    pw_thread_loop_unlock(loop_);
}

void AudioEngine::stop() {
    if (!loop_) {
        return;
    }

    pw_thread_loop_lock(loop_);
    routes_.clear();
    inputs_.clear();
    if (core_) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    pw_thread_loop_unlock(loop_);

    pw_thread_loop_stop(loop_);

    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
}

std::vector<DeviceInfo> AudioEngine::listDevices() const {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    return devices_;
}

std::string AudioEngine::addInput(const std::string& displayName) {
    const std::string inputId = "input-" + std::to_string(nextInputIndex_++);

    pw_thread_loop_lock(loop_);
    inputs_.push_back(
        std::make_unique<InputSource>(core_, inputId, displayName, kNumChannels, kSampleRateHz));
    pw_thread_loop_unlock(loop_);

    return inputId;
}

void AudioEngine::removeInput(const std::string& inputId) {
    // Clear this input's slot on every route first, so no route keeps a
    // (harmless but pointless) reference after the InputSource is gone.
    for (auto& route : routes_) {
        route->removeInputSlot(inputId);
    }

    pw_thread_loop_lock(loop_);
    inputs_.erase(std::remove_if(inputs_.begin(), inputs_.end(),
                                  [&](const std::unique_ptr<InputSource>& in) { return in->id() == inputId; }),
                  inputs_.end());
    pw_thread_loop_unlock(loop_);
}

std::vector<InputInfo> AudioEngine::listInputs() const {
    std::vector<InputInfo> result;
    result.reserve(inputs_.size());
    for (const auto& in : inputs_) {
        result.push_back(InputInfo{in->id(), in->displayName()});
    }
    return result;
}

InputSource* AudioEngine::findInput(const std::string& inputId) const {
    auto it = std::find_if(inputs_.begin(), inputs_.end(),
                            [&](const std::unique_ptr<InputSource>& in) { return in->id() == inputId; });
    return it == inputs_.end() ? nullptr : it->get();
}

std::string AudioEngine::addRoute(const std::string& deviceName, const std::string& displayName,
                                   double gainDb) {
    uint32_t targetId = PW_ID_ANY;
    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        auto it = std::find_if(devices_.begin(), devices_.end(),
                                [&](const DeviceInfo& d) { return d.nodeName == deviceName; });
        if (it == devices_.end()) {
            return {};
        }
        targetId = it->id;
    }

    const std::string routeId = "route-" + std::to_string(nextRouteIndex_++);
    const std::string label = displayName.empty() ? deviceName : displayName;

    pw_thread_loop_lock(loop_);
    auto route = std::make_unique<OutputRoute>(core_, loop_, routeId, deviceName, label, targetId,
                                                kNumChannels, kSampleRateHz);
    pw_thread_loop_unlock(loop_);

    // New outputs hear everything by default (matches pre-mixer behavior).
    for (const auto& in : inputs_) {
        route->setInputGainDb(in->id(), in->ringBuffer(), 0.0);
    }
    if (gainDb != 0.0) {
        route->setGainDb(gainDb);
    }

    routes_.push_back(std::move(route));
    return routeId;
}

void AudioEngine::removeRoute(const std::string& routeId) {
    pw_thread_loop_lock(loop_);
    routes_.erase(std::remove_if(routes_.begin(), routes_.end(),
                                  [&](const std::unique_ptr<OutputRoute>& r) { return r->id() == routeId; }),
                  routes_.end());
    pw_thread_loop_unlock(loop_);
}

std::vector<RouteInfo> AudioEngine::listRoutes() const {
    std::vector<RouteInfo> result;
    result.reserve(routes_.size());
    for (const auto& route : routes_) {
        result.push_back(route->info());
    }
    return result;
}

OutputRoute* AudioEngine::findRoute(const std::string& routeId) const {
    auto it = std::find_if(routes_.begin(), routes_.end(),
                            [&](const std::unique_ptr<OutputRoute>& r) { return r->id() == routeId; });
    return it == routes_.end() ? nullptr : it->get();
}

bool AudioEngine::setRouteGain(const std::string& routeId, double gainDb) {
    OutputRoute* route = findRoute(routeId);
    if (route) {
        route->setGainDb(gainDb);
    }
    return route != nullptr;
}

bool AudioEngine::setRouteMuted(const std::string& routeId, bool muted) {
    OutputRoute* route = findRoute(routeId);
    if (route) {
        route->setMuted(muted);
    }
    return route != nullptr;
}

bool AudioEngine::setRouteBandCount(const std::string& routeId, std::size_t count) {
    OutputRoute* route = findRoute(routeId);
    if (route) {
        route->setBandCount(count);
    }
    return route != nullptr;
}

bool AudioEngine::setRouteBand(const std::string& routeId, std::size_t index, const eqcore::EqBand& band) {
    OutputRoute* route = findRoute(routeId);
    if (route) {
        route->setBand(index, band);
    }
    return route != nullptr;
}

std::vector<eqcore::EqBand> AudioEngine::getRouteBands(const std::string& routeId) const {
    OutputRoute* route = findRoute(routeId);
    return route ? route->bands() : std::vector<eqcore::EqBand>{};
}

bool AudioEngine::setRouteInputGain(const std::string& routeId, const std::string& inputId, double gainDb) {
    OutputRoute* route = findRoute(routeId);
    InputSource* input = findInput(inputId);
    if (!route || !input) {
        return false;
    }
    return route->setInputGainDb(inputId, input->ringBuffer(), gainDb);
}

bool AudioEngine::removeRouteInput(const std::string& routeId, const std::string& inputId) {
    OutputRoute* route = findRoute(routeId);
    if (!route) {
        return false;
    }
    route->removeInputSlot(inputId);
    return true;
}

std::vector<std::pair<std::string, double>> AudioEngine::getRouteInputGains(const std::string& routeId) const {
    OutputRoute* route = findRoute(routeId);
    return route ? route->inputGainsDb() : std::vector<std::pair<std::string, double>>{};
}

void AudioEngine::onRegistryGlobal(void* userdata, uint32_t id, uint32_t /*permissions*/, const char* type,
                                    uint32_t /*version*/, const spa_dict* props) {
    auto* self = static_cast<AudioEngine*>(userdata);
    if (!props || std::string_view(type) != PW_TYPE_INTERFACE_Node) {
        return;
    }

    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mediaClass || std::string_view(mediaClass) != "Audio/Sink") {
        return;
    }

    const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!nodeName) {
        return;
    }

    // Skip nodes this daemon itself created (its own input virtual sinks
    // and output routes), so they never show up as candidate route targets.
    const std::string_view nodeNameView(nodeName);
    if (nodeNameView.starts_with("pipeeq_input_") || nodeNameView.starts_with("pipeeq_route_")) {
        return;
    }

    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

    std::lock_guard<std::mutex> lock(self->devicesMutex_);
    self->devices_.push_back(DeviceInfo{id, nodeName, description ? description : nodeName});
}

void AudioEngine::onRegistryGlobalRemove(void* userdata, uint32_t id) {
    auto* self = static_cast<AudioEngine*>(userdata);
    std::lock_guard<std::mutex> lock(self->devicesMutex_);
    self->devices_.erase(
        std::remove_if(self->devices_.begin(), self->devices_.end(), [id](const DeviceInfo& d) { return d.id == id; }),
        self->devices_.end());
}

} // namespace pipeeq
